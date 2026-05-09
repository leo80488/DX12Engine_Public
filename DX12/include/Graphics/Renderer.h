#pragma once

// Renderer — ECS → DrawPacket producer + RenderGraph owner.
// PVF: per-attribute ByteAddressBuffer SRVs; no DX12 types in this header.

#include "Graphics/IGraphicsDevice.h"
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
class XeGTAOPass;
class CASPass;
class GlassShatterPass;
class SkinningPass;
class ClusterPass;
class DecalPass;
class ReflectionProbeCapturePass;
class SpotShadowPass;
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
class SceneVoxelPass;
class SkyIBLPass;
class DDGIPass;
class DDGIProbeDebugPass;
class UIPass;
class WorldUIBillboardPass;

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

    // ===== Pass accessors — geometry / shadows / lighting =====
    OutlinePass*             GetOutlinePass()        { return m_outlinePass; }
    DebugWirePass*           GetDebugWirePass()      { return m_debugWirePass.get(); }
    SkyIBLPass*              GetSkyIBLPass()         { return m_skyIBLPass; }
    VolumetricFogPass*       GetVolumetricFogPass()  { return m_volFogPass; }
    DecalPass*               GetDecalPass()          { return m_decalPass; }

    // ===== Pass accessors — post-process =====
    TAAPass*          GetTAAPass()          { return m_taaPass.get(); }
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
    SSRPass*               GetSSRPass()              { return m_ssrPass.get(); }
    SSRResolvePass*        GetSSRResolvePass()       { return m_ssrResolvePass.get(); }
    SSRTemporalPass*       GetSSRTemporalPass()      { return m_ssrTemporalPass.get(); }
    SSRUpsamplePass*       GetSSRUpsamplePass()      { return m_ssrUpsamplePass.get(); }
    SSRCompositePass*      GetSSRCompositePass()     { return m_ssrCompositePass.get(); }
    SSRDepthHierarchyPass* GetSSRDepthHierPass()     { return m_ssrDepthHierPass.get(); }
    SceneColorPyramidPass* GetSceneColorPyramidPass(){ return m_sceneColorPyramidPass.get(); }
    // SSR trace SRV (RGBA16F: hitUV.xy, confidence.z, rayLen.w). 0 before first Execute.
    uint64_t GetSSRResultSrv() const;

    // ===== Pass accessors — UI =====
    UIPass*               GetUIPass()      { return m_uiPass.get(); }
    WorldUIBillboardPass* GetWorldUIPass() { return m_worldUIPass.get(); }

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
    uint64_t GetReflectionProbeBufferSrv() const { return m_probeMgr.GetBufferSrv(); }
    uint32_t GetActiveProbeCount()         const { return m_probeMgr.GetActiveProbeCount(); }
    // Editor-driven bake API. BakeAllProbes walks m_lastWorld → call after BeginFrame.
    void BakeProbe(uint32_t cubeSlice);
    void BakeAllProbes();

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
    class TracerSystem* GetTracerSystem() { return m_tracerSystem.get(); }
    class BeamSystem*   GetBeamSystem()   { return m_beamSystem.get(); }

    // ===== Skeletal animation =====
    SkeletonRegistry& GetSkeletonRegistry() { return m_skin.GetSkeletonRegistry(); }
    ClipLibrary&      GetClipLibrary()      { return m_skin.GetClipLibrary();      }
    MorphClipLibrary& GetMorphClipLibrary() { return m_skin.GetMorphClipLibrary(); }
    AnimationSystem*  GetAnimationSystem()  { return m_skin.GetAnimationSystem();  }
    IKSystem*         GetIKSystem()         { return m_skin.GetIKSystem();         }
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

    // ===== Viewport / window tracking =====
    uint32_t m_vpWidth     = 0;
    uint32_t m_vpHeight    = 0;
    uint32_t m_lastWindowW = 0;
    uint32_t m_lastWindowH = 0;
    uint32_t m_lastRenderW = 0;
    uint32_t m_lastRenderH = 0;

    // ===== Per-frame upload buffers (UPLOAD heap, persistently mapped) =====
    static constexpr uint32_t kMaxInstances = 32768; // ~2.5 MB instance buf @ 32k
    static constexpr uint32_t kMaxMaterials = 4096;
    RHI::GPUBuffer m_instanceBuffer;        void* m_instanceBufferMapped  = nullptr;
    RHI::GPUBuffer m_perObjectCB;           void* m_perObjectCBMapped     = nullptr;
    RHI::GPUBuffer m_lightCB;               void* m_lightCBMapped         = nullptr;
    RHI::GPUBuffer m_materialBuffer;        void* m_materialBufferMapped  = nullptr;
    RHI::GPUBuffer m_terrainCB;             void* m_terrainCBMapped       = nullptr;
    RHI::GPUBuffer m_spotShadowVPBuffer;    void* m_spotShadowVPMapped    = nullptr;
    uint64_t       m_spotShadowVPSrv = 0;

    // ===== ExecuteIndirect =====
    RHI::GPUBuffer             m_indirectArgBuffer;  // DEFAULT: IndirectDrawCommand[]
    RHI::GPUBuffer             m_indirectArgUpload;  // UPLOAD: CPU staging
    void*                      m_indirectArgMapped = nullptr;
    RHI::GPUBuffer             m_drawCountBuffer;    // DEFAULT: GPU count
    RHI::GPUBuffer             m_drawCountUpload;
    void*                      m_drawCountMapped   = nullptr;
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
    Resource::TextureHandle m_lightIconHandle = Resource::kInvalidTextureHandle;
    uint64_t                m_lightIconSRV    = 0;
    Resource::TextureHandle m_moonHandle      = Resource::kInvalidTextureHandle;
    uint64_t                m_moonSRV         = 0;
    uint64_t                m_nprRampTexHandle = 0;

    // ===== Picking =====
    Entity       m_instanceSlotToEntity[kMaxInstances]{}; // slot index → ECS Entity
    PickingPass* m_pickingPass = nullptr; // owned by m_graph

    // ===== Render passes — geometry & shadows =====
    GBufferPass*                  m_gbufferPass    = nullptr; // owned by m_graph
    TerrainPass*                  m_terrainPass    = nullptr; // owned by m_graph
    SkyboxPass*                   m_skyboxPass     = nullptr; // owned by m_graph
    TransparentPass*              m_transparentPass = nullptr; // owned by m_graph
    LightingPass*                 m_lightingPass   = nullptr; // owned by m_graph
    OutlinePass*                  m_outlinePass    = nullptr; // owned by m_graph
    SkyIBLPass*                   m_skyIBLPass     = nullptr; // owned by m_graph
    VolumetricFogPass*            m_volFogPass     = nullptr; // owned by m_graph
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
    std::unique_ptr<AutoExposurePass> m_autoExposurePass;
    std::unique_ptr<BloomPass>        m_bloomPass;
    std::unique_ptr<LensFlarePass>    m_lensFlarePass;
    std::unique_ptr<ToneMapPass>      m_toneMapPass;
    std::unique_ptr<XeGTAOPass>       m_xegtaoPass;
    std::unique_ptr<CASPass>          m_casPass;
    std::unique_ptr<GlassShatterPass> m_glassShatterPass;
    TAAJitterState                    m_taaJitter;

    // ===== Render passes — SSR =====
    std::unique_ptr<SSRPass>               m_ssrPass;
    std::unique_ptr<SSRResolvePass>        m_ssrResolvePass;
    std::unique_ptr<SSRTemporalPass>       m_ssrTemporalPass;
    std::unique_ptr<SSRUpsamplePass>       m_ssrUpsamplePass;
    std::unique_ptr<SSRCompositePass>      m_ssrCompositePass;
    std::unique_ptr<SSRDepthHierarchyPass> m_ssrDepthHierPass;
    std::unique_ptr<SceneColorPyramidPass> m_sceneColorPyramidPass;
    uint32_t                               m_ssrFrameIndex = 0;

    // ===== Render passes — debug & UI =====
    std::unique_ptr<DebugWirePass>         m_debugWirePass;
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

    // BuildRenderScene + extracted phases (the rest run inline inside BuildRenderScene).
    void BuildRenderScene(World& world);
    void BuildScene_SyncTerrain(World& world);     // Phase 0b
    void BuildScene_UploadLights(World& world);    // Phase 4
    void BuildScene_UploadProbes(World& world);    // Phase 5
    void BuildScene_UpdateDDGI  (World& world);    // Phase 6
    // Bake at most one queued probe per frame (cost: 6 forward + 42 prefilter).
    void ProcessProbeBakeQueue(RHI::CommandList colorLastCL);
};
