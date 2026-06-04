#pragma once

// Renderer — ECS → DrawPacket producer + RenderGraph owner.
// PVF: per-attribute ByteAddressBuffer SRVs; no DX12 types in this header.

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/FrameCB.h"
#include "Graphics/MeshManager.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RenderWorker.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"  // VolLight nested type used by-value
#include "Graphics/SceneBVH.h"
#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"

#include "ECS/HierarchyComponents.h"
#include "ECS/Components.h"
#include "Scene/SceneLoader.h"
#include "ECS/AnimationComponents.h"
#include "ECS/AnimationSystem.h"
#include "ECS/IKSystem.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowSystem.h"
#include "ECS/DecalSystem.h"
#include "ECS/DecalSpawner.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Resource/SkeletonAsset.h"
#include "Graphics/SkinningBuffers.h"
#include "Graphics/MaterialCBVRing.h"
#include "Graphics/MaterialSRVRing.h"
#include "Graphics/TAAJitterState.h"
#include "Graphics/ShadowFrustumCompute.h"
#include "Graphics/ReflectionProbeManager.h"
#include "Graphics/SkinnedMeshSubsystem.h"
// SkinningPass / ClusterPass / DecalPass / ReflectionProbeCapturePass /
// SpotShadowPass are forward-declared below; full headers in Renderer.cpp.
#include "Resource/MeshSystem.h"
#include "Resource/MeshLibrary.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/DecalMaterialLibrary.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/DDGISceneAS.h"
#include "ECS/DDGIComponents.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

#include "Resource/ProceduralMesh.h"  // PrimitiveMeshType

namespace Resource { class MaterialSystem; }
namespace PostProcess { class Stack; class VolumeSystem; class EntityVolumeSource; }
namespace ShaderReflect { struct Reflection; }

class World;
class PickingPass;
class LightingPass;
class SkyboxPass;
class TransparentPass;
class ShadowPass;
class ShadowSystem;
class OutlinePass;
class AutoExposurePass;
class BloomPass;
class LensFlarePass;
class ToneMapPass;
class TAAPass;
class FXAAPass;
class XeGTAOPass;
class CASPass;
class GlassShatterPass;
class SkinningPass;
class ClusterPass;
class DecalPass;
class ReflectionProbeCapturePass;
class SpotShadowPass;
class CloudPass;
class CullingPass;
class GBufferPass;
class TerrainPass;
class DebugWirePass;
class HiZPass;
class SSRPass;
class SSRResolvePass;
class SSRTemporalPass;
class SSRUpsamplePass;
class SSRCompositePass;
class SSRDepthHierarchyPass;
class SceneColorPyramidPass;
class SSRSubsystem;
class SceneVoxelPass;
class SkyIBLPass;
class DDGIPass;
class DDGIProbeDebugPass;
class UIPass;
class WorldUIBillboardPass;
class DebugIconPass;

// CPU-side cbuffer mirrors used by Renderer's per-frame UPLOAD-heap CBs.
// Public so FrameCB<T> can be instantiated in Renderer.h; layouts MUST stay
// in sync with the matching HLSL — keep all field edits in the .cpp's
// authoritative copies in mind (see Renderer.cpp top-of-file).
namespace RendererDetail
{
    // viewProj is JITTERED for SV_POSITION; curViewProjNoJitter feeds velocity.
    struct alignas(16) PerViewCB
    {
        float viewProj[16];            // current, jittered (rasterization)
        float prevViewProj[16];        // previous frame, unjittered (velocity)
        float curViewProjNoJitter[16]; // current, unjittered (velocity numerator)
    };

    // Mirror of light_cb.hlsli — do NOT reorder fields without touching both.
    struct alignas(16) LightCB
    {
        float    lightDir[3];    float pad0;
        float    lightColor[3];  float pad1;
        float    cameraPos[3];   float pad3;
        float    invViewProj[16];
        uint32_t iblRadianceMips;
        float    iblStrength;
        uint32_t iblUseSH;
        float    aerialMaxDistKm;
        float    shadowMatrix[4][16];
        float    cascadeSplits[4];
        float    cascadeTexelWorldSize[4];
        float    shadowBias;
        float    shadowStrength;
        float    shadowMapTexelSize;
        float    shadowBlendRange;
        uint32_t shadowFrameIndex;
        float    shadowNormalOffset;
        float    _shadowPad0;
        float    _shadowPad1;
        float    cameraForward[3];
        float    _shadowPad2;
        float    viewMatrix[16];
        float    clusterNearZ;
        float    clusterFarZ;
        uint32_t clusterLightCount;
        float    nprMinBrightness;
        uint32_t reflectionProbeCount;
        float    reflectionPad[3];
        uint32_t ddgiVolumeCount;
        uint32_t ddgiEnabled;
        float    ddgiDiffuseScale;
        float    skyIBLDiffuseScale;
        float    ddgiAONearFieldStrength;
        uint32_t viewMode;            // global view-mode switch — mirrors light_cb.hlsli viewMode (was _ddgiPad0)
        float    _ddgiPad1;
        float    _ddgiPad2;
    };

    // Mirror of Terrain.{ms,ps,as}.hlsl cbuffer TerrainCB; each block is 16B aligned.
    struct alignas(16) TerrainParamsCB
    {
        float    worldOriginX, worldOriginY;
        float    worldSize;
        float    heightScale;
        float    heightmapUVOffsetX, heightmapUVOffsetY;
        float    heightmapUVScaleX,  heightmapUVScaleY;
        float    heightmapTexel;
        uint32_t hasHeightmap;
        uint32_t hasSplatmap;
        float    worldCenterY;
        int32_t  layerBindlessIdx[4];
        float    layerTilingScale[4];
        int32_t  layerNormalIdx[4];
        int32_t  layerARMIdx[4];
        int32_t  layerDispIdx[4];
        uint32_t tilesPerSide;
        uint32_t enableFrustumCull;
        float    _pad8a, _pad8b;
        float    layerMinHeight   [4];
        float    layerMaxHeight   [4];
        float    layerFadeHeight  [4];
        float    layerMinSlopeDeg [4];
        float    layerMaxSlopeDeg [4];
        float    layerFadeSlopeDeg[4];
        float    frustumPlanes[6][4];
    };
}

class Renderer
{
public:
    explicit Renderer(IGraphicsDevice& gfx);
    ~Renderer();

    // ===== Lifecycle =====
    // Build GPU resources, init passes, register them with the RenderGraph.
    void Compile();
    // Invalidate per-entity caches; call before World::Clear().
    void OnWorldClear();
    // ECS → DrawPackets + per-frame CB upload (rebuilds projection on resize).
    void BeginFrame(World& world, FrameIndex frame, float dt, uint32_t vpW, uint32_t vpH);
    // CPU animation chain: character state → AnimationSystem sample → IK →
    // ChainPhysics → LocalToWorld → Socket → Follow → bone AABB merge →
    // SkinMatrix → BuildSkinJobs. Drives the PoseRingBuffer / VertexRing
    // writes that the SkinningPass consumes in Render(). Lives outside
    // BeginFrame so the scheduler can place it in the Animation phase
    // (DesignMd/System_Scheduler_Architecture.md §9.1) — must run BEFORE
    // BeginFrame each frame so SkinningOutputComponent offsets line up with
    // the slot Render() reads. No-op if Skinning subsystem isn't initialised.
    void TickAnimationChain(World& world, FrameIndex frame, float dt);
    // Execute the deferred rendering graph; each pass allocates its own CL.
    RHI::CommandList Render();
    // Hot-reload shaders on every pass. Caller must guarantee GPU is idle.
    void ReloadShaders();

    // ===== System injection =====
    void SetMaterialSystem(Resource::MaterialSystem* matSys) { m_matSys = matSys; }
    void SetMeshSystem(Resource::MeshSystem* meshSys)        { m_meshSys = meshSys; }
    void SetMeshLibrary(Resource::MeshLibrary* meshLib)      { m_meshLib = meshLib; }
    void SetTextureSystem(Resource::TextureSystem* ts)       { m_texSys  = ts; }
    void SetResourceManager(Resource::ResourceManager* rm)   { m_resMgr  = rm; }
    Resource::MeshLibrary* GetMeshLibrary() const            { return m_meshLib; }
    MeshManager&           GetMeshManager()                  { return m_meshMgr; }
    const MeshManager&     GetMeshManager() const            { return m_meshMgr; }

    // Track a MeshLibrary handle loaded for the current world.  OnWorldClear
    // releases every tracked handle so per-reload VB/IB allocations don't
    // accumulate (MeshLibrary::Load does NOT dedupe by path — same .meshlib
    // reloaded N times produces N independent GPU buffer pairs).  Called by
    // SceneInstanceLoader after each successful MeshLibrary::Load.
    void TrackWorldMeshLibrary(Resource::Handle h)
    {
        if (h.IsValid()) m_worldMeshLibs.push_back(h);
    }

    // ===== Camera / view / draw list =====
    void SetCamera(const RenderCamera& cam) { m_camera = cam; }
    void SetClearColor(const float clearColor[4]) { std::memcpy(m_clearColor, clearColor, sizeof(m_clearColor)); }
    const RenderView& GetView() const       { return m_view; }
    DrawList GetDrawList(DrawFilter f) const;

    // ===== Picking =====
    // Store a pending pick request; PickingPass executes the GPU copy in Render().
    void RequestPick(float pixelX, float pixelY);
    // After IGraphicsDevice::FlushAndWait(), read the pick result.
    bool ResolvePick(Entity& outEntity);

    // Editor-only highlight: force an OutlinePass Custom-filter draw for this
    // entity even if its MaterialComponent::OUTLINE flag is off. NullEntity
    // disables the highlight. App pushes EditorLayer's selection here before
    // each BeginFrame. Does NOT mutate MaterialComponent.
    void   SetPickingOutlineEntity(Entity e) { m_pickingOutlineEntity = e; }
    Entity GetPickingOutlineEntity() const   { return m_pickingOutlineEntity; }

    // ===== Pass accessors — geometry / shadows / lighting =====
    OutlinePass*             GetOutlinePass()        { return m_outlinePass; }
    DebugWirePass*           GetDebugWirePass()      { return m_debugWirePass.get(); }
    SkyIBLPass*              GetSkyIBLPass()         { return m_skyIBLPass; }
    VolumetricFogPass*       GetVolumetricFogPass()  { return m_volFogPass; }
    CloudPass*               GetCloudPass()          { return m_cloudPass; }
    DecalPass*               GetDecalPass()          { return m_decalPass; }
    class VideoPass*         GetVideoPass()          { return m_videoPass; }
    class VideoQuadPass*     GetVideoQuadPass()      { return m_videoQuadPass; }

    // ===== AA mode =====
    enum class AAMode : uint32_t
    {
        None     = 0,
        FXAA     = 1,
        TAA      = 2,
        FXAA_TAA = 3,   // TAA first, FXAA on TAA's resolved output
    };
    AAMode GetAAMode() const { return m_aaMode; }
    void   SetAAMode(AAMode m);

    // ===== Global view mode — Unity/Unreal-style viewmode switch =====
    // Applies to ALL objects (opaque/terrain via the deferred LightingPass
    // branch; transparent via its forward shader). Wireframe additionally
    // forces FILL_MODE_WIREFRAME on the geometry passes and suppresses the
    // background passes (skybox/clouds/fog) + TAA for a clean dark-bg result.
    enum class ViewMode : uint32_t
    {
        Lit       = 0,
        Unlit     = 1,
        Wireframe = 2,
    };
    ViewMode GetViewMode() const { return m_viewMode; }
    void     SetViewMode(ViewMode m);

    // ===== Pass accessors — post-process =====
    TAAPass*          GetTAAPass()          { return m_taaPass.get(); }
    FXAAPass*         GetFXAAPass()         { return m_fxaaPass.get(); }
    ToneMapPass*      GetToneMapPass()      { return m_toneMapPass.get(); }
    AutoExposurePass* GetAutoExposurePass() { return m_autoExposurePass.get(); }
    BloomPass*        GetBloomPass()        { return m_bloomPass.get(); }
    LensFlarePass*    GetLensFlarePass()    { return m_lensFlarePass.get(); }
    XeGTAOPass*       GetXeGTAOPass()       { return m_xegtaoPass.get(); }
    CASPass*          GetCASPass()          { return m_casPass.get(); }
    GlassShatterPass* GetGlassShatterPass() { return m_glassShatterPass.get(); }
    // One-shot: capture Tonemap output, init shard physics from impactUV [0..1],
    // composite shards until duration elapses. Re-call to restart.
    void TriggerGlassShatter(float impactU, float impactV);

    // ===== Pass accessors — SSR =====
    // All forward through m_ssrSubsystem; signatures preserved so SSRDebugWindow
    // and other editor tooling keep compiling unchanged.
    SSRPass*               GetSSRPass();
    SSRResolvePass*        GetSSRResolvePass();
    SSRTemporalPass*       GetSSRTemporalPass();
    SSRUpsamplePass*       GetSSRUpsamplePass();
    SSRCompositePass*      GetSSRCompositePass();
    SSRDepthHierarchyPass* GetSSRDepthHierPass();
    SceneColorPyramidPass* GetSceneColorPyramidPass();
    // SSR trace SRV (RGBA16F: hitUV.xy, confidence.z, rayLen.w). 0 before first Execute.
    uint64_t GetSSRResultSrv() const;
    SSRSubsystem*          GetSSRSubsystem() { return m_ssrSubsystem.get(); }

    // ===== Pass accessors — UI =====
    UIPass*               GetUIPass()      { return m_uiPass.get(); }
    WorldUIBillboardPass* GetWorldUIPass() { return m_worldUIPass.get(); }
    // Editor-only debug billboard icons (light/camera gizmos). DebugDrawSystem
    // fills it each frame via AddIcon(); empty (no draw) in Game builds.
    DebugIconPass*        GetDebugIconPass() { return m_debugIconPass.get(); }
    // Lazy-load (and cache) a debug-icon texture by asset path and return its
    // bindless table index (== handle_id), or 0xFFFFFFFF if missing / not ready.
    // Generic so any debug icon kind works by just passing its texture path.
    uint32_t              GetDebugIconBindless(const char* path);

    // ===== Final viewport output =====
    // SRV GPU handle of the final tone-mapped LDR output (use as editor viewport tex).
    uint64_t GetFinalOutputSrvHandle() const;
    // Capture tonemap output to PNG. Synchronous (flushes GPU). False on error.
    bool CaptureViewportToPNG(const char* path);

    // ===== Post-process stack & volumes =====
    PostProcess::Stack*              GetPostProcessStack()         { return m_postProcessStack.get(); }
    const PostProcess::Stack*        GetPostProcessStack() const   { return m_postProcessStack.get(); }
    PostProcess::VolumeSystem*       GetPostProcessVolumes()       { return m_postProcessVolumes.get(); }
    const PostProcess::VolumeSystem* GetPostProcessVolumes() const { return m_postProcessVolumes.get(); }

    // ===== Reflection probes =====
    uint64_t GetReflectionProbeArraySrv()  const { return m_probeMgr.GetArraySrv(); }
    // Per-frame SRV — picks the current ring slot of the probe StructuredBuffer.
    uint64_t GetReflectionProbeBufferSrv() const { return m_probeMgr.GetBufferSrv(m_gfx); }
    uint32_t GetActiveProbeCount()         const { return m_probeMgr.GetActiveProbeCount(); }
    // Editor-driven bake API. BakeAllProbes walks m_lastWorld → call after BeginFrame.
    void BakeProbe(uint32_t cubeSlice);
    void BakeAllProbes();

    // Export every non-realtime, baked reflection probe's cubemap to a .itex
    // beside @p worldPath (in a "<worldStem>_probes" folder) and stamp the
    // relative path onto each ReflectionProbeComponent::bakedCubemapPath.
    // Call right BEFORE Resource::SaveScene so the component serializer
    // persists the freshly-written paths. On the next LoadScene,
    // BuildScene_UploadProbes restores those cubemaps straight off disk
    // instead of enqueueing a full re-bake. Realtime probes are skipped
    // (their path is cleared — they always re-bake on an interval).
    void ExportBakedProbeCubemaps(World& world, const std::string& worldPath);

    // ===== DDGI =====
    bool                                     IsDDGIReady() const { return m_ddgiReady; }
    DDGI::DDGIVolumeManager&                 GetDDGIManager()       { return m_ddgiMgr; }
    const DDGI::DDGIVolumeManager&           GetDDGIManager() const { return m_ddgiMgr; }
    IndirectLightingSettingsComponent&       GetIndirectLightingSettings()       { return m_ddgiSettings; }
    const IndirectLightingSettingsComponent& GetIndirectLightingSettings() const { return m_ddgiSettings; }
    DDGIProbeDebugPass*                      GetDDGIProbeDebugPass() { return m_ddgiProbeDebugPass.get(); }

    // ===== Decal subsystem =====
    Resource::DecalMaterialLibrary& GetDecalMaterialLibrary() { return m_decalMaterialLibrary; }
    DecalSpawner&                   GetDecalSpawner()         { return m_decalSpawner; }

    // ===== VFX subsystems =====
    class TracerSystem*     GetTracerSystem()     { return m_tracerSystem.get(); }
    class BeamSystem*       GetBeamSystem()       { return m_beamSystem.get(); }
    class AfterimageSystem* GetAfterimageSystem() { return m_afterimageSystem.get(); }

    // ===== Skeletal animation =====
    SkeletonRegistry& GetSkeletonRegistry() { return m_skin.GetSkeletonRegistry(); }
    ClipLibrary&      GetClipLibrary()      { return m_skin.GetClipLibrary();      }
    MorphClipLibrary& GetMorphClipLibrary() { return m_skin.GetMorphClipLibrary(); }
    AnimationSystem*  GetAnimationSystem()  { return m_skin.GetAnimationSystem();  }
    IKSystem*         GetIKSystem()         { return m_skin.GetIKSystem();         }
    FootIKTargetSystem* GetFootIKSystem()   { return m_skin.GetFootIKSystem();     }
    CharacterStateSystem* GetCharacterStateSystem() { return m_skin.GetCharacterStateSystem(); }
    void              SetPhysicsSystem(DX12Physics::PhysicsSystem* p) { m_skin.SetPhysicsSystem(p); }
    void              SetAnimationClipSystem(Resource::AnimationClipSystem* cs) { m_skin.SetAnimationClipSystem(cs); }
    PoseRingBuffer&   GetPoseRingBuffer()   { return m_skin.GetPoseRingBuffer();   }
    // Register a skinned entity. Adds MeshSkinnedComponent + SkinningOutputComponent.
    void RegisterSkinnedMesh(World& world, Entity e,
                             const DirectX::XMFLOAT3* positions,
                             const DirectX::XMFLOAT3* normals,
                             const BlendVertex*       blendData,
                             uint32_t                 vertexCount,
                             uint32_t                 baseMeshDescIdx)
    {
        m_skin.RegisterSkinnedMesh(m_gfx, world, e, positions, normals,
                                   blendData, vertexCount, baseMeshDescIdx);
    }
    // All-in-one registration from SceneLoader::SkinnedMeshPending.
    bool RegisterSkinnedMeshFull(World& world, const SceneLoader::SkinnedMeshPending& pending)
    {
        return m_skin.RegisterSkinnedMeshFull(m_gfx, m_meshMgr, world, pending);
    }

    // ===== Custom shader queries =====
    // Reflection / error string for a material's custom PS (id<0 → nullptr/empty).
    const ShaderReflect::Reflection* GetCustomShaderReflection(int customShaderID) const;
    std::string                      GetCustomShaderError(int customShaderID) const;

    // ===== Runtime toggles =====
    bool IsGPUCullingEnabled() const   { return m_gpuCullingEnabled; }
    void SetGPUCullingEnabled(bool v)  { m_gpuCullingEnabled = v; }
    bool IsIndirectDrawEnabled() const { return m_useIndirectDraw; }
    void SetIndirectDrawEnabled(bool v){ m_useIndirectDraw = v; }
    bool IsAnimCullingEnabled() const  { return m_skin.IsAnimCullingEnabled(); }
    void SetAnimCullingEnabled(bool v) { m_skin.SetAnimCullingEnabled(v); }
    bool IsSSAOEnabled() const         { return m_ssaoEnabled; }
    void SetSSAOEnabled(bool v)        { m_ssaoEnabled = v; }
    bool IsSSREnabled()  const         { return m_ssrEnabled; }
    void SetSSREnabled(bool v)         { m_ssrEnabled = v; }
    // Editor-only: hide all light icon billboards (entities with LightData +
    // BillboardComponent). Skips DrawCandidate emission for those entities so
    // the billboard quad never reaches the GBuffer / Transparent pass.
    bool AreLightBillboardsVisible() const { return m_lightBillboardsVisible; }
    void SetLightBillboardsVisible(bool v) { m_lightBillboardsVisible = v; }

private:
    // ===== Core =====
    IGraphicsDevice& m_gfx;
    MeshManager      m_meshMgr;
    float            m_clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    RenderCamera     m_camera;
    RenderView       m_view;
    RG::RenderGraph  m_graph;
    World*           m_lastWorld    = nullptr;
    FrameIndex       m_currentFrame = 0;
    float            m_deltaTime    = 0.0f;
    float            m_globalTimeSec = 0.0f;  // accumulated; tracer noise scroll
    std::vector<DrawPacket> m_drawPackets;

    // ===== Per-frame light build state (Phase 0 → Phase 4) =====
    struct ResolvedLight {
        DirectX::XMFLOAT3 position;  float radius;
        DirectX::XMFLOAT3 color;     float intensity;
        DirectX::XMFLOAT3 direction; float spotAngle;
        LightType type;
        bool      castsShadow;          // SpotShadowPass opt-in
        uint32_t  shadowSliceIdx;       // 0xFFFFFFFF = no map
        // Index into m_volumetricLights when this light owns a VolumetricLightComponent.
        uint32_t  volLightIdx = 0xFFFFFFFFu;
    };
    std::vector<ResolvedLight>               m_frameLights;
    std::vector<VolumetricFogPass::VolLight> m_volumetricLights;
    bool  m_sunVolumetric      = false;  // any directional+VolumetricLightComponent
    float m_sunVolumetricScale = 1.0f;
    ShadowFrustumCompute m_shadowFrustum; // CSM frustum OBB + plane caches

    // ===== Runtime toggles =====
    bool m_ssaoEnabled        = false;
    bool m_ssrEnabled         = true;
    bool m_useIndirectDraw    = false;
    bool m_gpuCullingEnabled  = true;
    // Default OFF: light icons are an editor debug gizmo driven by the editor's
    // DebugDrawSystem (Lights debug category), which sets this true each frame.
    // Game builds never touch it, so light icons are stripped from shipping.
    bool m_lightBillboardsVisible = false;

    // ===== Viewport / window tracking =====
    uint32_t m_vpWidth     = 0;
    uint32_t m_vpHeight    = 0;
    uint32_t m_lastWindowW = 0;
    uint32_t m_lastWindowH = 0;
    uint32_t m_lastRenderW = 0;
    uint32_t m_lastRenderH = 0;

    // ===== Per-entity prev-frame world matrix (for TAA velocity correctness) =====
    // GBuffer VS computes prev-frame clip pos for the velocity buffer; the
    // skinned path reads prev positions in MESH-LOCAL space (Skin.cs.hlsl
    // outputs bone-deformed but un-world-transformed), so projecting them
    // through prevViewProj alone misses the entity's prev-frame world
    // transform. Track it CPU-side and feed via GPUInstanceData.prevWorld.
    std::unordered_map<Entity, DirectX::XMFLOAT4X4> m_prevWorldMatrices;

    // ===== Per-frame upload buffers (UPLOAD heap, persistently mapped) =====
    // Triple-buffered (3 slots) so pipelined frame pacing (CPU runs 2 frames
    // ahead via GraphicsDX12::BeginFrame) doesn't race CPU writes against
    // in-flight GPU reads. CBs use FrameCB<T>; SRV/staging upload buffers use
    // manual per-frame arrays (FrameCB hard-codes CONSTANT_BUFFER bind flag).
    static constexpr uint32_t kMaxInstances = 32768; // ~2.5 MB instance buf @ 32k
    static constexpr uint32_t kMaxMaterials = 4096;
    static constexpr uint32_t kFrameSlots   = 3;     // matches GraphicsDX12::FrameCount

    FrameCB<RendererDetail::PerViewCB>       m_perObjectCB;
    FrameCB<RendererDetail::LightCB>         m_lightCB;
    FrameCB<RendererDetail::TerrainParamsCB> m_terrainCB;

    RHI::GPUBuffer m_instanceBuffer       [kFrameSlots];
    void*          m_instanceBufferMapped [kFrameSlots] = {};
    RHI::GPUBuffer m_spotShadowVPBuffer   [kFrameSlots];
    void*          m_spotShadowVPMapped   [kFrameSlots] = {};
    uint64_t       m_spotShadowVPSrv     [kFrameSlots] = {};

    // Material buffer — triple-buffered: BuildDrawListAndUploadInstances writes
    // it per frame from scratch (matIdx resets to 0 each frame, slots get fresh
    // data based on visible materials). The prev-batch (slot,hash) cache only
    // dedupes WITHIN a frame, not across frames — camera movement → different
    // visible material set → different per-slot data → CPU/GPU race without
    // triple-buffering. Flicker observed in motion before this fix.
    RHI::GPUBuffer m_materialBuffer       [kFrameSlots];
    void*          m_materialBufferMapped [kFrameSlots] = {};

    // ===== ExecuteIndirect =====
    RHI::GPUBuffer             m_indirectArgBuffer;  // DEFAULT: IndirectDrawCommand[]
    RHI::GPUBuffer             m_indirectArgUpload [kFrameSlots];  // UPLOAD: CPU staging
    void*                      m_indirectArgMapped [kFrameSlots] = {};
    RHI::GPUBuffer             m_drawCountBuffer;    // DEFAULT: GPU count
    RHI::GPUBuffer             m_drawCountUpload   [kFrameSlots];
    void*                      m_drawCountMapped   [kFrameSlots] = {};
    uint32_t                   m_indirectDrawCount = 0;
    std::vector<IndirectGroup> m_indirectGroups;

    // ===== Virtual texture handles (RG-managed) =====
    RG::RGTextureHandle m_albedoHandle;
    RG::RGTextureHandle m_normalHandle;
    RG::RGTextureHandle m_surfaceHandle;
    RG::RGTextureHandle m_depthHandle;
    RG::RGTextureHandle m_velocityHandle;
    RG::RGTextureHandle m_emissiveHandle;

    // ===== System refs (injected; not owned) =====
    Resource::MaterialSystem*  m_matSys  = nullptr;
    Resource::MeshSystem*      m_meshSys = nullptr;
    Resource::MeshLibrary*     m_meshLib = nullptr;
    Resource::TextureSystem*   m_texSys  = nullptr;
    Resource::ResourceManager* m_resMgr  = nullptr;

    // ===== Texture caches (per-entity, lifetime independent of editor UI) =====
    struct MatTexEntry {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };
    std::unordered_map<Entity, std::array<MatTexEntry, MaterialComponent::TEXTURESLOT_COUNT>> m_matTexCache;
    std::unordered_map<Entity, std::unordered_map<std::string, MatTexEntry>>                  m_customMatTexCache;

    // Texture-handle releases queued by OnWorldClear that wait for the next
    // BeginFrame's SyncMaterialTextures pass to Acquire same-path handles
    // first. Releasing inline drops TextureSystem refcounts to 0, FreeSlot
    // wipes the path-hash mapping, and the next Acquire re-uploads the
    // texture from CPU bytes even when reloading the SAME world. Deferring
    // by one frame lets the new world's Acquire bump refcount 1→2 BEFORE
    // we Release back to 1 — slot survives, no GPU re-upload (doc §8.1).
    std::vector<Resource::TextureHandle> m_pendingMatTexReleaseAfterNextSync;

    // MeshLibrary handles loaded for the currently-bound world.  Two-stage
    // release: each OnWorldClear releases the previous clear's pending set
    // and parks the current one — every lib lives at least one world reload
    // before being freed.  Inline release (even with full multi-queue sync)
    // still triggered a delayed TDR; the lag-one-world margin avoids it.
    std::vector<Resource::Handle> m_worldMeshLibs;
    std::vector<Resource::Handle> m_meshLibsPendingRelease;

    // ===== Skybox / IBL textures =====
    struct SkyboxTexEntry {
        std::string             path;
        Resource::TextureHandle handle           = Resource::kInvalidTextureHandle;
        uint64_t                lastLoggedHandle = 0;
    };
    SkyboxTexEntry m_skyboxTexCache[3]; // [0]=irradiance, [1]=radiance, [2]=skybox visual
    SkyboxTexEntry m_brdfLutEntry;      // pre-integrated BRDF LUT (2D)
    uint64_t       m_brdfLutSrv             = 0; // cached per frame for SSRComposite
    uint64_t       m_skyRadianceSrvForDDGI  = 0; // DDGI miss shader; 0 if no skybox

    // ===== Lazy-loaded misc textures =====
    // Generic debug-icon texture cache, keyed by asset path. One Acquire per
    // unique path; GetDebugIconBindless() returns the bindless index. Adding a
    // new debug icon kind is just a new path — no new member needed here.
    std::unordered_map<std::string, Resource::TextureHandle> m_debugIconTex;
    Resource::TextureHandle m_moonHandle      = Resource::kInvalidTextureHandle;
    uint64_t                m_moonSRV         = 0;
    uint64_t                m_nprRampTexHandle = 0;

    // ===== Picking =====
    Entity       m_instanceSlotToEntity[kMaxInstances]{}; // slot index → ECS Entity
    PickingPass* m_pickingPass = nullptr; // owned by m_graph
    // Editor selection highlight. Treated as NullEntity in game builds.
    Entity       m_pickingOutlineEntity = NullEntity;

    // ===== Render passes — geometry & shadows =====
    GBufferPass*                  m_gbufferPass    = nullptr; // owned by m_graph
    TerrainPass*                  m_terrainPass    = nullptr; // owned by m_graph
    SkyboxPass*                   m_skyboxPass     = nullptr; // owned by m_graph
    TransparentPass*              m_transparentPass = nullptr; // owned by m_graph
    LightingPass*                 m_lightingPass   = nullptr; // owned by m_graph
    OutlinePass*                  m_outlinePass    = nullptr; // owned by m_graph (pre-TAA composite)
    SkyIBLPass*                   m_skyIBLPass     = nullptr; // owned by m_graph
    VolumetricFogPass*            m_volFogPass     = nullptr; // owned by m_graph
    CloudPass*                    m_cloudPass      = nullptr; // owned by m_graph
    class VideoPass*              m_videoPass      = nullptr; // owned by m_graph
    class VideoQuadPass*          m_videoQuadPass  = nullptr; // owned by m_graph
    SceneVoxelPass*               m_sceneVoxelPass = nullptr; // owned by m_graph
    SpotShadowPass*               m_spotShadowPass = nullptr; // owned by m_graph
    DecalPass*                    m_decalPass      = nullptr; // owned by m_graph
    std::unique_ptr<ShadowSystem> m_shadowSystem;             // CSM math + stabilization
    std::unique_ptr<ShadowPass>   m_shadowPass;               // standalone (not in graph)
    std::unique_ptr<ClusterPass>  m_clusterPass;
    std::unique_ptr<CullingPass>  m_cullingPass;              // GPU culling, pre-graph
    std::unique_ptr<HiZPass>      m_hiZPass;                  // post-GBuffer depth

    // ===== Render passes — post-process compute =====
    std::unique_ptr<TAAPass>          m_taaPass;
    std::unique_ptr<FXAAPass>         m_fxaaPass;
    // Re-applies TAA/FXAA enable from m_aaMode, force-OFF in Wireframe view
    // (thin lines ghost badly under temporal AA). Called by both SetAAMode and
    // SetViewMode so AA state stays consistent across the two toggles.
    void UpdateAAEnabled();

    AAMode                            m_aaMode = AAMode::TAA;  // default unchanged from prior versions
    ViewMode                          m_viewMode = ViewMode::Lit;
    std::unique_ptr<AutoExposurePass> m_autoExposurePass;
    std::unique_ptr<BloomPass>        m_bloomPass;
    std::unique_ptr<LensFlarePass>    m_lensFlarePass;
    std::unique_ptr<ToneMapPass>      m_toneMapPass;
    std::unique_ptr<XeGTAOPass>       m_xegtaoPass;
    std::unique_ptr<CASPass>          m_casPass;
    std::unique_ptr<GlassShatterPass> m_glassShatterPass;
    TAAJitterState                    m_taaJitter;

    // ===== Render passes — SSR =====
    // Single owning subsystem; individual passes accessed via GetSSRxxx().
    std::unique_ptr<SSRSubsystem>          m_ssrSubsystem;

    // ===== Render passes — debug & UI =====
    std::unique_ptr<DebugWirePass>         m_debugWirePass;
    std::unique_ptr<DebugIconPass>         m_debugIconPass;
    std::unique_ptr<UIPass>                m_uiPass;
    std::unique_ptr<WorldUIBillboardPass>  m_worldUIPass;

    // ===== Reflection probes =====
    ReflectionProbeManager m_probeMgr;

    // ===== DDGI =====
    DDGI::DDGIVolumeManager             m_ddgiMgr;
    std::unique_ptr<DDGIPass>           m_ddgiPass;
    std::unique_ptr<DDGIProbeDebugPass> m_ddgiProbeDebugPass;
    DDGISceneAS                         m_ddgiSceneAS;
    bool                                m_ddgiReady = false;
    IndirectLightingSettingsComponent   m_ddgiSettings;

    // ===== Skeletal animation + GPU skinning =====
    SkinnedMeshSubsystem m_skin;

    // ===== Custom-material rings (Phase E/F) =====
    MaterialCBVRing       m_customMatCbvRing;
    std::vector<uint64_t> m_matCustomCbvVA;     // per-MaterialBuffer-slot GPU VA (per frame)
    MaterialSRVRing       m_customMatSrvRing;
    std::vector<uint64_t> m_matCustomTexHandle;

    // ===== Decals =====
    Resource::DecalMaterialLibrary m_decalMaterialLibrary;
    DecalSpawner                   m_decalSpawner;
    DecalLifetimeSystem            m_decalLifetimeSystem;

    // ===== Post-process orchestration =====
    std::unique_ptr<PostProcess::Stack>              m_postProcessStack;
    std::unique_ptr<PostProcess::VolumeSystem>       m_postProcessVolumes;
    std::unique_ptr<PostProcess::EntityVolumeSource> m_postProcessEntityVolumes;

    // ===== VFX — particles / trails / tracers / beams =====
    std::unique_ptr<class ParticleSystem>     m_particleSystem;
    std::unique_ptr<class ParticleSimPass>    m_particleSimPass;
    class ParticleRenderPass*                 m_particleRenderPass = nullptr; // m_graph
    std::unique_ptr<class TrailSystem>        m_trailSystem;
    std::unique_ptr<class TrailUpdatePass>    m_trailUpdatePass;
    class TrailRenderPass*                    m_trailRenderPass = nullptr;    // m_graph
    std::unique_ptr<class TracerSystem>       m_tracerSystem;
    std::unique_ptr<class TracerSimPass>      m_tracerSimPass;
    class TracerRenderPass*                   m_tracerRenderPass = nullptr;   // m_graph
    std::unique_ptr<class BeamSystem>         m_beamSystem;
    std::unique_ptr<class BeamSimPass>        m_beamSimPass;
    std::unique_ptr<class AfterimageSystem>      m_afterimageSystem;
    std::unique_ptr<class AfterimageCapturePass> m_afterimageCapturePass;

    // ===== Terrain texture caches =====
    struct TerrainTexSlot {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };
    struct TerrainLayerSlots {
        TerrainTexSlot albedo;
        TerrainTexSlot normal;
        TerrainTexSlot arm;       // R=AO, G=Roughness, B=Metalness
        TerrainTexSlot disp;      // single-channel height for height-blend
    };
    struct TerrainTexCache {
        TerrainTexSlot                   heightmap;
        TerrainTexSlot                   splatmap;
        std::array<TerrainLayerSlots, 4> layers;
    };
    std::unordered_map<Entity, TerrainTexCache> m_terrainTexCache;

    // ===== Scene BVH (rebuilt per frame for frustum culling) =====
    SceneBVH m_sceneBVH;

    // ===== Render workers (dedicated threads + binary_semaphore) =====
    RenderWorker m_renderWorkers[3];   // [0]=shadow [1]=unused [2]=compute
    bool         m_workersStarted = false;

    // ===== World destroy-listener (per-entity cleanup hook) =====
    World*                              m_subscribedWorld         = nullptr;
    World::EntityDestroyListenerHandle  m_entityDestroyListenerId = 0;

    // ===== Internal helpers =====
    void EnsureWorkers();
    // Render() sub-stages extracted for readability (behavior-preserving). The
    // rest of Render() (worker kicks, fences, cross-queue CL wiring) stays inline
    // because those phases share command-list lifetimes that can't cross a return.
    void Render_BindFrameResources(uint32_t frameSlot); // bind CBs/SRVs to passes; allocates no CLs
    void Render_ComputePrepass();                       // skinning/particle/trail/tracer/beam compute CLs
    void SubscribeToWorld(World* world);
    void OnEntityDestroyed(Entity e);
    void InitMeshManager();
    void InitSkinningSystems();
    void CreateConstantBuffers();
    void UploadFrameData(FrameIndex frame, uint32_t vpW, uint32_t vpH);
    void SyncMaterialTextures(Entity e, MaterialComponent& mc);
    void SyncMaterialCustomTextures(Entity e, MaterialComponent& mc);
    void SyncSkyboxIBL(World& world);
    uint32_t RegisterMeshLibMesh(const MeshLibRef& ref);

    // Unified VFX: drain PendingMeshVFXSpawns — load each requested mesh path
    // into the MeshLibrary and patch the pre-created VFX entity with a
    // MeshLibRef (+ material). Called from BeginFrame before BuildRenderScene.
    void DrainMeshVFXSpawns(World& world);

    // DrawCandidate — per-draw staging record built during BuildRenderScene's
    // gather phase, then sorted + emitted into DrawPackets. MUST stay trivially
    // copyable: the parallel MeshLibRef gather merges shards via raw std::memcpy.
    struct DrawCandidate
    {
        Entity     entity;
        uint32_t   meshDescSlot;
        uint32_t   indexCount;
        DirectX::XMFLOAT4X4 worldMatrix;
        const MaterialComponent* mc;
        uint64_t       texBaseColor  = 0;
        uint64_t       texSurfaceMap = 0;
        uint64_t       texNormalMap  = 0;
        uint32_t       matHash       = 0;
        DrawFilter     filter        = DrawFilter::Opaque;
        PermutationKey perm          = {};
        // World-space depth-sort centroid; MeshLibRef overrides w/ WorldAabb centre to fix off-pivot meshes.
        DirectX::XMFLOAT3 worldCenter = { 0.0f, 0.0f, 0.0f };
        float          depth              = 0.0f;  // view-space Z (transparent painter's-algorithm sort)
        float          outlinePixels      = 2.0f;  // per-material outline width (Custom packets only)
        bool           screenSpaceOutline = true;  // enable screen-space edge detection
        bool           isPickingOutline   = false; // editor selection (x-ray inverted hull)
        uint8_t        stencilRef         = 1;     // 1=PBR, 2=NPR
        uint8_t        shadowCullMode     = 0;     // ShadowCullMode raw; 0=Default back-cull
        uint8_t        castShadow         = 1;     // 0 = material opted out of ShadowPass
        uint32_t       prevPosElementBase = 0xFFFFFFFFu; // TAA: skinned prev pos (0xFFFFFFFF = static)
        uint32_t       customPSID         = 0;     // 0 = default GBuffer PS; non-zero = dynamic id
        // Phase 3 author-intent visibility — copied verbatim into DrawPacket.
        // viewMask defaults to All so legacy emitters (lights, billboards, beams)
        // that don't read VisibilityComponent don't get accidentally hidden.
        uint32_t       viewMask           = ViewBit::All;
        bool           renderInMainPass   = true;
    };

    // Material → filter/permutation/stencil/depth classification. Promoted from a
    // BuildRenderScene-local lambda so the gather helpers can share it; reads
    // m_camera + the passed camera forward (view-space depth).
    void     ApplyBlendMode(DrawCandidate& c, const DirectX::XMFLOAT3& camForward);
    // Lazy-resolve customShaderPath → dynamic PS id, registering in BOTH the
    // GBuffer and Transparent shader libraries (+ reflection sync). Returns 0
    // when the material has no custom shader. Promoted from a local lambda.
    uint32_t ResolveCustomPSID(const MaterialComponent& m);

    // BuildRenderScene is a thin orchestrator over the BuildScene_* helpers
    // below. Order + the few cross-helper locals (the candidate list, the BVH
    // masks, the camera forward) are documented at the call site.
    void BuildRenderScene(World& world);
    // -- gather/cull stages (run in sequence inside BuildRenderScene) --
    void BuildScene_SyncMaterials(World& world);   // material tex sync + NPR ramp + deferred release
    void BuildScene_CullBVH(World& world,
                            std::vector<uint8_t>& bvhVisibleMask,
                            std::vector<uint8_t>& bvhTestedMask); // Phase 0.5 BVH + frustum cull
    void BuildScene_DebugWireframes(World& world); // debug overlay (independent of the gather)
    void BuildScene_GatherPrimitivesAndLights(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward); // shares ONE entity loop (see note)
    void BuildScene_GatherMeshLibRefs(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const std::vector<uint8_t>& bvhTestedMask,
                            const std::vector<uint8_t>& bvhVisibleMask,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_GatherBillboards(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_GatherBeams(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_GatherAfterimages(std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_SortAndEmit(const std::vector<DrawCandidate>& candidates);
    // -- post-gather phases --
    void BuildScene_SyncTerrain(World& world);     // Phase 0b
    void BuildScene_UploadLights(World& world);    // Phase 4
    void BuildScene_UploadProbes(World& world);    // Phase 5
    void BuildScene_UpdateDDGI  (World& world);    // Phase 6
    // Bake at most one queued probe per frame (cost: 6 forward + 42 prefilter).
    void ProcessProbeBakeQueue(RHI::CommandList colorLastCL);
};
