#pragma once

// SkinnedMeshSubsystem — every piece of state the Renderer used to track
// directly for skeletal animation + GPU skinning.
//
// Ownership includes:
//   - Skeleton / clip / morph-clip registries
//   - Pose ring buffer + skinned-vertex ring (per-frame GPU scratch)
//   - ECS systems: Animation / IK / ChainPhysics / LocalToWorld / Socket /
//                  Follow / SkinMatrix
//   - SkinningPass (compute dispatch that transforms rest-pose → skinned)
//   - Per-frame job list, prev-pose cache, descriptor-slot cache,
//     anim-culling visible set
//
// Public API mirrors the old Renderer methods 1:1: Init() at startup,
// BuildSkinJobs() each frame, RegisterSkinnedMesh / RegisterSkinnedMeshFull
// at entity creation time. Renderer.h now just forwards its accessors to
// this member.

#include "ECS/AnimationComponents.h"     // MeshSkinnedComponent, SkinningOutputComponent
#include "ECS/AnimationSystem.h"         // AnimationSystem, LocalToWorldSystem, SkinMatrixSystem
#include "ECS/IKSystem.h"
#include "ECS/FootIKTargetSystem.h"
#include "ECS/CharacterStateSystem.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowSystem.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Resource/SkeletonAsset.h"      // SkeletonRegistry, ClipLibrary, BlendVertex
#include "Graphics/SkinningBuffers.h"    // PoseRingBuffer, SkinnedVertexRing
#include "Scene/SceneLoader.h"           // SceneLoader::SkinnedMeshPending
#include "RenderGraph/RenderPass/SkinningPass.h" // SkinDispatchDesc, SkinningPass
#include "ECS/ECS.h"                     // Entity

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IGraphicsDevice;
class MeshManager;

class SkinnedMeshSubsystem
{
public:
    // Init pose / vertex rings, register per-frame ring slots in the bindless
    // table, construct ECS animation systems, and create the SkinningPass.
    // Must run once during Renderer::Compile().
    void Init(IGraphicsDevice& gfx, MeshManager& meshMgr);

    // Walk every MeshSkinnedComponent in @p world and build a per-entity
    // SkinDispatchDesc list. Also patches each entity's MeshDescriptor to
    // point at this frame's skinned output streams. Called from BeginFrame.
    void BuildSkinJobs(World& world, MeshManager& meshMgr);

    // Upload rest-pose + blend data for one mesh and add
    // MeshSkinnedComponent + SkinningOutputComponent to @p e.
    void RegisterSkinnedMesh(IGraphicsDevice& gfx,
                             World& world, Entity e,
                             const DirectX::XMFLOAT3* positions,
                             const DirectX::XMFLOAT3* normals,
                             const BlendVertex*       blendData,
                             uint32_t                 vertexCount,
                             uint32_t                 baseMeshDescIdx);

    // All-in-one registration path: creates UV/index/tangent buffers, builds
    // the base MeshDescriptor, then delegates to RegisterSkinnedMesh. Also
    // attaches SkeletonComponent + AnimationComponent (+ auto-detected
    // ChainPhysicsComponent on hair/skirt bones) to the character root.
    bool RegisterSkinnedMeshFull(IGraphicsDevice& gfx,
                                 MeshManager&     meshMgr,
                                 World&           world,
                                 const SceneLoader::SkinnedMeshPending& pending);

    // Registries.
    SkeletonRegistry&  GetSkeletonRegistry()  { return m_skeletonRegistry;  }
    ClipLibrary&       GetClipLibrary()       { return m_clipLibrary;       }
    MorphClipLibrary&  GetMorphClipLibrary()  { return m_morphClipLibrary;  }

    // ECS systems (may be null before Init).
    AnimationSystem*     GetAnimationSystem()    { return m_animSystem.get();    }
    IKSystem*            GetIKSystem()           { return m_ikSystem.get();      }
    FootIKTargetSystem*  GetFootIKSystem()       { return m_footIKSystem.get();  }
    CharacterStateSystem* GetCharacterStateSystem() { return m_characterStateSystem.get(); }
    void SetAnimationClipSystem(Resource::AnimationClipSystem* cs);
    void                 SetPhysicsSystem(DX12Physics::PhysicsSystem* p) { m_physics = p; }
    DX12Physics::PhysicsSystem* GetPhysicsSystem() { return m_physics; }
    ChainPhysicsSystem*  GetChainPhysicsSystem() { return m_chainPhysicsSystem.get(); }
    LocalToWorldSystem*  GetLocalToWorldSystem() { return m_localToWorldSystem.get(); }
    SocketSystem*        GetSocketSystem()       { return m_socketSystem.get();  }
    FollowSystem*        GetFollowSystem()       { return m_followSystem.get();  }
    SkinMatrixSystem*    GetSkinMatrixSystem()   { return m_skinMatrixSystem.get(); }

    // GPU rings (non-const — callers write into them each frame).
    PoseRingBuffer&    GetPoseRingBuffer()    { return m_poseBuffer; }
    SkinnedVertexRing& GetVertexRing()        { return m_vertRing;   }

    // Pass + jobs.
    SkinningPass*                        GetSkinningPass()     { return m_skinningPass; }
    std::vector<SkinDispatchDesc>&       GetSkinJobs()         { return m_skinJobs;     }

    // Per-frame state accessed by culling code.
    std::unordered_set<Entity>& GetAnimVisibleSet() { return m_animVisibleSet; }
    bool IsAnimCullingEnabled() const                { return m_animCullingEnabled; }
    void SetAnimCullingEnabled(bool v)               { m_animCullingEnabled = v; }

    // Entity → skinned-output MeshDescriptor slot cache (cleared on OnWorldClear).
    std::unordered_map<Entity, uint32_t>& GetSkinnedMeshDescCache() { return m_skinnedMeshDescCache; }

    // Entity → previous frame pose info (cleared on OnWorldClear).
    struct PrevPoseEntry { uint32_t poseByteOffset = ~0u; uint32_t boneCount = 0; };
    std::unordered_map<Entity, PrevPoseEntry>& GetPrevPoseCache() { return m_prevPoseCache; }

    // Clear entity-keyed caches (called from Renderer::OnWorldClear).
    // No re-register needed for the skinned vertex ring: Init() now uses
    // MeshDescriptorHeap::RegisterPersistentBuffer, so posBindlessIdx[] /
    // nrmBindlessIdx[] slot indices stay valid across world reloads.
    void OnWorldClear();

    // Drop all per-entity caches keyed on `e`. Fired from World's entity-
    // destroy listener (wired via Renderer::SubscribeToWorld). Without this,
    // a CreateEntity that recycles `e`'s ID would inherit stale mesh-desc
    // indices and a bogus prev-pose offset — TAA velocity for the new
    // entity would then reproject from the old skeleton's pose.
    void OnEntityDestroyed(Entity e);

    bool IsInitialised() const { return m_initialised; }

private:
    // Registries.
    SkeletonRegistry m_skeletonRegistry;
    ClipLibrary      m_clipLibrary;
    MorphClipLibrary m_morphClipLibrary;

    // GPU rings.
    PoseRingBuffer    m_poseBuffer;
    SkinnedVertexRing m_vertRing;

    // ECS systems.
    std::unique_ptr<AnimationSystem>    m_animSystem;
    std::unique_ptr<IKSystem>           m_ikSystem;
    std::unique_ptr<FootIKTargetSystem> m_footIKSystem;
    std::unique_ptr<CharacterStateSystem> m_characterStateSystem;
    // Borrowed pointer — App.cpp wires this after PhysicsSystem::Init via
    // SetPhysicsSystem so FootIKTargetSystem can raycast.
    DX12Physics::PhysicsSystem*         m_physics = nullptr;
    std::unique_ptr<ChainPhysicsSystem> m_chainPhysicsSystem;
    std::unique_ptr<LocalToWorldSystem> m_localToWorldSystem;
    std::unique_ptr<SocketSystem>       m_socketSystem;
    std::unique_ptr<FollowSystem>       m_followSystem;
    std::unique_ptr<SkinMatrixSystem>   m_skinMatrixSystem;

    // SkinningPass (owned; non-owning pointer exposed to Render()).
    std::unique_ptr<SkinningPass> m_skinPassOwned;
    SkinningPass*                 m_skinningPass = nullptr;

    // Per-frame job list built by BuildSkinJobs, consumed by SkinningPass::Execute.
    std::vector<SkinDispatchDesc> m_skinJobs;

    // Caches keyed by entity.
    std::unordered_map<Entity, uint32_t>      m_skinnedMeshDescCache;
    std::unordered_map<Entity, PrevPoseEntry> m_prevPoseCache;

    // Anim-culling — previous frame's visible set that gated animation work.
    std::unordered_set<Entity> m_animVisibleSet;
    bool                       m_animCullingEnabled = true;

    bool m_initialised = false;
};
