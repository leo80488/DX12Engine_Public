#pragma once

// ChainPhysicsSystem — Verlet-integration + PBD distance-constraint solver
// for hair / skirt / cloth bone chains parsed from PMX skeleton naming.
//
// Also includes SpringBone simulation for jiggle bones (Chest, Butt, etc.)
// that are single bones with no children — uses spring-damper dynamics instead
// of Verlet/PBD.
//
// Call order each frame (CPU path, before LocalToWorldSystem):
//   1. AnimationSystem + IK + PostIKGrants   (fill poses)
//   2. ChainPhysicsSystem::Update(world, dt) (simulate chains + springs, write poses)
//   3. LocalToWorldSystem                    (accumulate world matrices)
//
// Chain detection: scans SkeletonAsset.boneNames[] for patterns:
//   Hair / PonyTail / hair   → hair chains  (independent strands, Verlet+PBD)
//   Skirt / skirt             → skirt chains (ring topology, Verlet+PBD)
//   Chest / Butt              → spring bones (single jiggle bones, spring-damper)

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

// ---------------------------------------------------------------------------
// ECS component: tag an entity for chain physics simulation
// ---------------------------------------------------------------------------
struct ChainPhysicsComponent
{
    bool  enabled         = true;

    // ---- Hair parameters ----
    float damping         = 0.8f;    // velocity retention per frame (lower = settles faster)
    float gravity         = -30.0f;  // Y-axis gravity strength
    float stiffness       = 1.5f;    // global constraint stiffness multiplier
    int   iterations      = 7;       // constraint iterations per frame
    float maxVelocity     = 0.5f;    // velocity clamp (world units/frame, 0=unlimited)
    float localStiffness  = 0.02f;   // pull toward rest pose (0=free, 1=locked)
    int   substeps        = 1;       // collision substeps per frame (1~8, higher=less tunneling)

    // ---- Skirt overrides (if < 0, uses hair value as fallback) ----
    float skirtDamping        = 0.15f;   // cloth settles faster than hair
    float skirtGravity        = -5.0f;   // less aggressive gravity
    float skirtStiffness      = 2.0f;    // stiffer structural constraints
    float skirtMaxVelocity    = 0.5f;    // tighter velocity clamp
    float skirtLocalStiffness = 0.3f;   // stronger pull toward rest pose
    float skirtHorizDecay     = 1.0f;   // horizontal constraint decay root→tip (0=tip has 0 stiffness, 1=uniform)

    // ---- Spring bone (jiggle) parameters — virtual root ----
    float springStiffness = 120.0f;  // spring constant k (50~300)
    float springDamping   = 12.0f;   // damping coefficient d (1~30)
    float springMass      = 1.0f;    // mass — affects inertia (0.1~10)
    float springGravity   = -2.0f;   // Y-axis gravity for subtle droop
    float springMaxDisp   = 2.0f;    // max displacement from goal (world units)

    // ---- Spring bone child parameters (L/R relative to virtual root) ----
    float springChildStiffness = 350.0f;  // very stiff — follows root closely
    float springChildDamping   = 20.0f;
    float springChildMass      = 0.5f;    // low mass — fast response
    float springChildGravity   = -0.5f;
    float springChildMaxDisp   = 0.5f;    // small displacement from root
};

// ---------------------------------------------------------------------------
// CapsuleColliderComponent — per-skeleton capsule colliders for cloth/hair.
// Attach to the same entity as SkeletonComponent + ChainPhysicsComponent.
// Each capsule is defined by two bone endpoints + radius.
// ---------------------------------------------------------------------------
struct CapsuleColliderComponent
{
    static constexpr int MAX_CAPSULES = 16;

    struct CapsuleDef
    {
        uint32_t boneA    = 0;       // skeleton bone index for endpoint A
        uint32_t boneB    = 0;       // skeleton bone index for endpoint B
        float    radius   = 0.05f;   // collision radius (world units)
        bool     enabled  = true;

        // Fine-tuning offsets applied in the bone's local space.
        DirectX::XMFLOAT3 offsetA = { 0.f, 0.f, 0.f }; // local offset from boneA
        DirectX::XMFLOAT3 offsetB = { 0.f, 0.f, 0.f }; // local offset from boneB
    };

    CapsuleDef capsules[MAX_CAPSULES];
    int        count = 0;
};

// ---------------------------------------------------------------------------
// ChainPhysicsSystem
// ---------------------------------------------------------------------------
class ChainPhysicsSystem
{
public:
    explicit ChainPhysicsSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Simulate all entities with ChainPhysicsComponent.
    // Reads/writes AnimationSystem::LocalPose via GetMutableLocalPose().
    void Update(World& world, float dt,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // Force re-detection of chains for an entity (e.g. after skeleton change).
    void Invalidate(Entity e);

    // Drop simulation state for a destroyed entity. Wired through
    // World::AddEntityDestroyListener via Renderer::OnEntityDestroyed.
    void OnEntityDestroyed(Entity e) { m_entityData.erase(e); }

    // Clear all cached entity data (call before World::Clear).
    void ClearAll() { m_entityData.clear(); }

private:
    // ----- Internal data structures ------------------------------------------

    struct Particle
    {
        DirectX::XMFLOAT3 position;      // current simulated world position
        float              invMass;       // 0 = pinned root
        DirectX::XMFLOAT3 prevPosition;  // previous frame position (Verlet)
        uint32_t           boneIndex;     // skeleton bone index
        DirectX::XMFLOAT3 restPosition;  // animation rest position (updated each frame)
    };

    struct Constraint
    {
        uint32_t particleA;
        uint32_t particleB;
        float    restLength;
        float    stiffness;
        float    segmentT = 0.f; // 0=root, 1=tip — used for horizontal stiffness decay
    };

    enum class StrandType { Hair, Skirt };

    struct Strand
    {
        StrandType type;
        uint32_t   particleOffset; // into m_particles[]
        uint32_t   particleCount;
        int        ringIndex;      // skirt: ring position (0..N-1); hair: -1
    };

    // SpringBone — single jiggle bone (Chest, Butt, etc.)
    // Uses spring-damper ODE: F = -k*(x - rest) - d*v + gravity
    struct SpringBone
    {
        uint32_t           boneIndex;       // skeleton bone index (0xFFFFFFFF for virtual)
        DirectX::XMFLOAT3 restLocalPos;    // bind-pose local position
        DirectX::XMFLOAT3 currentWorldPos; // simulated world position
        DirectX::XMFLOAT3 velocity;        // world-space velocity

        float stiffness = 70.0f;  // spring constant k (50~200)
        float damping   = 5.0f;   // damping coefficient d (5~20)
        float mass      = 8.0f;    // mass (affects inertia)
        float radius    = 0.0f;    // collision radius (future use)

        // Virtual root support (Chest_Root grouping)
        int32_t  parentSpringIdx = -1;  // index into springBones[]; -1 = independent
        bool     isVirtual       = false; // true = no real bone, physics-only grouping node
        uint32_t parentBoneIndex = ~0u;   // for virtual: skeleton parent bone for world xform
        // Child offset from virtual root's goal (set at init, used each frame)
        DirectX::XMFLOAT3 offsetFromRoot = { 0.f, 0.f, 0.f };
    };

    // Per-entity physics data (initialized once, simulated each frame).
    struct EntityData
    {
        std::vector<Particle>  particles;
        std::vector<Strand>    strands;

        // Constraint color groups (for parallel-safe solving).
        // [0..1] = vertical even/odd, [2..3] = horizontal even/odd, [4..5] = shear even/odd
        static constexpr int kMaxGroups = 6;
        std::vector<Constraint> constraintGroups[kMaxGroups];
        int groupCount = 0;

        // Spring bones (jiggle physics)
        std::vector<SpringBone> springBones;

        // Pre-computed bone world matrices (skeleton space), rebuilt each frame.
        // Avoids repeated parent-chain traversals in UpdateRoots / LocalSpace / capsules.
        std::vector<DirectX::XMFLOAT4X4> boneWorldMatCache;

        bool initialized = false;
    };

    // ----- Chain detection ---------------------------------------------------
    void InitEntity(Entity e, const SkeletonAsset& skel,
                    const AnimationSystem::LocalPose* poses);

    // DFS from 'root' through physics children. At forks, each branch becomes
    // its own chain (shared ancestors are duplicated as the root prefix).
    // Appends all discovered linear chains to 'outChains'.
    void TraceChains(uint32_t root, const SkeletonAsset& skel,
                     const std::vector<bool>& isPhysBone,
                     const std::vector<std::vector<uint32_t>>& children,
                     std::vector<uint32_t>& current,
                     std::vector<std::vector<uint32_t>>& outChains);

    // Compute bind-pose world position for a bone.
    DirectX::XMFLOAT3 ComputeBindWorldPos(uint32_t bone,
                                           const SkeletonAsset& skel,
                                           const AnimationSystem::LocalPose* poses);

    // ----- Capsule collision data (computed per-frame from CapsuleColliderComponent) --
    struct WorldCapsule { DirectX::XMFLOAT3 a, b; float radius; };

    // ----- Simulation --------------------------------------------------------
    void Simulate(EntityData& data, const ChainPhysicsComponent& cfg, float dt,
                  AnimationSystem::LocalPose* poses, const SkeletonAsset& skel,
                  const WorldCapsule* capsules = nullptr, int capsuleCount = 0);

    // Push particles out of capsule colliders.
    void SolveCapsuleCollision(EntityData& data,
                               const WorldCapsule* capsules, int capsuleCount);

    // Update root particles to follow their bone's current world position.
    void UpdateRoots(EntityData& data,
                     AnimationSystem::LocalPose* poses,
                     const SkeletonAsset& skel);

    // Verlet integration for a single strand.
    void IntegrateStrand(EntityData& data, const Strand& strand, float dt,
                         float damping, const DirectX::XMFLOAT3& gravity,
                         float maxVelocity);

    // Solve all constraint groups (one pass).
    void SolveConstraints(EntityData& data, float globalStiffness);

    // Write simulated particle positions back to bone local poses.
    void WriteBonePoses(EntityData& data,
                        AnimationSystem::LocalPose* poses,
                        const SkeletonAsset& skel);

    // Compute animation rest positions and pull particles toward them.
    void ApplyLocalSpaceConstraint(EntityData& data,
                                   AnimationSystem::LocalPose* poses,
                                   const SkeletonAsset& skel,
                                   float localStiffness);

    // ----- Spring bone simulation --------------------------------------------
    void SimulateSpringBones(EntityData& data, const ChainPhysicsComponent& cfg,
                             float dt, AnimationSystem::LocalPose* poses,
                             const SkeletonAsset& skel);

    void WriteSpringBonePoses(EntityData& data,
                              AnimationSystem::LocalPose* poses,
                              const SkeletonAsset& skel);

    // Compute world transform for a bone (same logic as IKSystem).
    DirectX::XMMATRIX ComputeWorldTransform(uint32_t boneIndex,
                                             const AnimationSystem::LocalPose* poses,
                                             const SkeletonAsset& skel) const;

    // ----- Members -----------------------------------------------------------
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;

    std::unordered_map<Entity, EntityData> m_entityData;
};
