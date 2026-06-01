#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"
#include "Graphics/IVideoDecoder.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "Graphics/GPUInstanceData.h"
#include "Graphics/IndirectDrawCommand.h"

// render passes
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/GlassShatterPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/SceneVoxelPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/SkinningPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "RenderGraph/RenderPass/TracerPasses.h"
#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "RenderGraph/RenderPass/AfterimageCapturePass.h"
#include "RenderGraph/RenderPass/TrailPasses.h"
#include "RenderGraph/RenderPass/CullingPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/SSRPass.h"
#include "RenderGraph/RenderPass/SSRDepthHierarchyPass.h"
#include "RenderGraph/RenderPass/SceneColorPyramidPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/VideoPass.h"
#include "RenderGraph/RenderPass/VideoQuadPass.h"

// post-process
#include "PostProcess/PostProcessStack.h"
#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/VolumeSystem.h"
#include "PostProcess/EntityVolumeSource.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/BillboardComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/VFXSpawnRequests.h"   // unified VFX Lane-B mailboxes (Tracer/Decal/Afterimage/Mesh)
#include "ECS/TrailComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/TODSystems.h"
#include "ECS/VideoComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/MaterialReflectionSync.h"

// resource system
#include "Resource/MaterialSerializer.h"  // Resource::LoadMaterial (mesh VFX material)
#include "Resource/ProceduralMesh.h"
#include "Resource/MaterialSystem.h"

// engine systems
#include "System/Log.h"
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
using TerrainParamsCB = RendererDetail::TerrainParamsCB;
static_assert(sizeof(TerrainParamsCB) == 336,
    "TerrainParamsCB layout drift — sync Terrain.{ms,ps,as}.hlsl + Renderer.h");

// ---------------------------------------------------------------------------
Renderer::Renderer(IGraphicsDevice& gfx) : m_gfx(gfx) {}

Renderer::~Renderer()
{
    // Safety path for "world outlives renderer": skip listener removal —
    // m_subscribedWorld may dangle since the scene is usually torn down first.
    m_subscribedWorld         = nullptr;
    m_entityDestroyListenerId = 0;

    // Shutdown worker threads before releasing GPU resources.
    if (m_workersStarted)
        for (auto& w : m_renderWorkers)
            w.Shutdown();

    // Triple-buffered CBs — FrameCB::Destroy unmaps + releases each slot.
    m_perObjectCB.Destroy(m_gfx);
    m_lightCB    .Destroy(m_gfx);
    m_terrainCB  .Destroy(m_gfx);

    // Triple-buffered non-CB upload buffers.
    for (uint32_t i = 0; i < kFrameSlots; ++i)
    {
        if (m_instanceBufferMapped[i])  m_gfx.UnmapBuffer(m_instanceBuffer[i]);
        if (m_spotShadowVPMapped[i])    m_gfx.UnmapBuffer(m_spotShadowVPBuffer[i]);
        if (m_indirectArgMapped[i])     m_gfx.UnmapBuffer(m_indirectArgUpload[i]);
        if (m_drawCountMapped[i])       m_gfx.UnmapBuffer(m_drawCountUpload[i]);
        if (m_materialBufferMapped[i])  m_gfx.UnmapBuffer(m_materialBuffer[i]);
    }

    // Release reflection-probe ring (cubemap-array texture is freed by the
    // RHI on Renderer teardown; ProbeManager only owns the StructuredBuffer).
    m_probeMgr.Shutdown(m_gfx);

    // Release all cached material texture handles.
    if (m_texSys)
    {
        for (auto& [entity, cache] : m_matTexCache)
            for (auto& entry : cache)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, m_gfx);

        // Release terrain textures (heightmap + splatmap + 4 layers × 4 maps per entity).
        for (auto& [entity, cache] : m_terrainTexCache)
        {
            auto rel = [&](TerrainTexSlot& slot) {
                if (slot.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(slot.handle, m_gfx);
            };
            rel(cache.heightmap);
            rel(cache.splatmap);
            for (auto& l : cache.layers)
            {
                rel(l.albedo);
                rel(l.normal);
                rel(l.arm);
                rel(l.disp);
            }
        }

        // Release skybox IBL texture handles.
        for (auto& entry : m_skyboxTexCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, m_gfx);

        // Release BRDF LUT.
        if (m_brdfLutEntry.handle != Resource::kInvalidTextureHandle)
            m_texSys->Release(m_brdfLutEntry.handle, m_gfx);

        // Release decal material textures; dangling DecalComponent shared_ptrs become inert.
        m_decalMaterialLibrary.Shutdown(*m_texSys, m_gfx);
    }
}

// ---------------------------------------------------------------------------
void Renderer::Compile()
{
    InitMeshManager();
    CreateConstantBuffers();

    // ---- Graph-scope GBuffer textures (stable handles across frames) -------
    // R11G11B10_FLOAT albedo: HDR emissive isn't clipped at 1; shaders use linear directly.
    // isUAV on decal-targeted RTs so DecalPass blends in-place; RG handles UAV→SRV transition.
    m_albedoHandle   = m_graph.CreateTexture("GBuffer0_Albedo",
        { RHI::Format::R11G11B10_FLOAT,     0, 0, false, true,  L"GBuffer0_Albedo"   });
    m_normalHandle   = m_graph.CreateTexture("GBuffer1_Normal",
        { RHI::Format::R16G16B16A16_FLOAT,  0, 0, false, true,  L"GBuffer1_Normal"   });
    m_surfaceHandle  = m_graph.CreateTexture("GBuffer2_Surface",
        { RHI::Format::R8G8B8A8_UNORM,     0, 0, false, true,  L"GBuffer2_Surface"  });
    m_depthHandle    = m_graph.CreateTexture("GBuffer_Depth",
        { RHI::Format::D24_UNORM_S8_UINT,  0, 0, true,  false, L"GBuffer_Depth"     });
    m_velocityHandle = m_graph.CreateTexture("GBuffer3_Velocity",
        // isSRV=true: velocity is read as an SRV by TAA / XeGTAO / SSR (outside
        // the graph, via direct SRV handles), so it needs SHADER_RESOURCE + an
        // SRV. Without this velocitySrv is 0 and those passes never get real
        // motion vectors (and the velocity slot is left unbound → GBV).
        { RHI::Format::R16G16_FLOAT,        0, 0, false, false, L"GBuffer3_Velocity", /*isSRV*/true });
    m_emissiveHandle = m_graph.CreateTexture("GBuffer4_Emissive",
        { RHI::Format::R16G16B16A16_FLOAT,  0, 0, false, false, L"GBuffer4_Emissive" });

    // ---- Register passes — each owns its ShaderLibrary + PSOCache ----------
    // ShadowSystem owns cascade math; ShadowPass runs standalone (own CL + Texture2DArray).
    m_shadowSystem = std::make_unique<ShadowSystem>();
    m_shadowSystem->Init(m_gfx);
    m_shadowPass = std::make_unique<ShadowPass>();
    m_shadowPass->Init(m_gfx);
    m_shadowPass->SetShadowSystem(m_shadowSystem.get());

    {
        auto gbufPass = std::make_unique<GBufferPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle, m_emissiveHandle);
        m_gbufferPass = gbufPass.get();
        m_graph.AddPass(std::move(gbufPass));
    }
    {
        // TerrainPass writes into the same GBuffer right after GBufferPass; depth-tests
        // against scene geometry. Self-disables w/o mesh-shader support or no TerrainComponent.
        auto pass = std::make_unique<TerrainPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle, m_emissiveHandle);
        m_terrainPass = pass.get();
        m_graph.AddPass(std::move(pass));

        // ShadowPass renders depth-only terrain into each CSM cascade; standalone pass
        // needs the TerrainParams CB handed to it directly (not via graph binding).
        // The actual buffer pointer is rebound per-frame in Render() because the
        // TerrainCB is now triple-buffered (one slot per in-flight frame).
        if (m_shadowPass)
            m_shadowPass->SetTerrainPass(m_terrainPass);
    }
    {
        // Compute-only env-cube → SH projection. Must precede LightingPass (PS samples SH buffer).
        auto pass = std::make_unique<SkyIBLPass>();
        m_skyIBLPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Spot shadow atlas. Must precede LightingPass + VolumetricFogPass (both sample its depth).
        auto pass = std::make_unique<SpotShadowPass>();
        m_spotShadowPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // DecalPass: GBuffer→Lighting; UAV-blends into albedo/normal/surface, depth as SRV.
        auto pass = std::make_unique<DecalPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle);
        m_decalPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<LightingPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_emissiveHandle);
        m_lightingPass = pass.get();
        // Probe array SRV (TextureCubeArray) is persistent — wire once. The
        // probe StructuredBuffer SRV rotates each frame and is rebound below
        // in Render() to the current ring slot.
        m_lightingPass->SetReflectionProbes(
            m_probeMgr.GetArraySrv(), 0);
        // Cluster probe grid/index handles wired post-ClusterPass::Init (end of Compile).
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<SkyboxPass>(m_depthHandle);
        m_skyboxPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Volumetric clouds: composite OVER skybox, BEFORE volumetric fog
        // (so fog can attenuate cloud god-rays / aerial perspective later).
        auto pass = std::make_unique<CloudPass>(m_depthHandle);
        m_cloudPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Video overlay: composites decoded NV12 frames over HDR scene
        // color. Runs after clouds so cinematics can dim the rendered
        // world by setting VideoComponent alpha < 1.
        auto pass = std::make_unique<VideoPass>();
        m_videoPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Scene occupancy voxelization. Precedes VolumetricFogPass (LightInject samples grid).
        // Inert with zero AABBs — shader clears, falls back to screen-space shadow.
        auto svp = std::make_unique<SceneVoxelPass>();
        m_sceneVoxelPass = svp.get();
        m_graph.AddPass(std::move(svp));
    }
    {
        // Volumetric fog: scattering between Lighting/Skybox and PP chain. Reads depth, blends HDR.
        auto pass = std::make_unique<VolumetricFogPass>(m_depthHandle);
        m_volFogPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<TransparentPass>(m_depthHandle);
        m_transparentPass = pass.get();
        // Forward transparent iterates ALL probes per pixel — no cluster grid binding.
        // Probe StructuredBuffer SRV is per-frame; rebound in Render() below.
        m_transparentPass->SetReflectionProbes(
            m_probeMgr.GetArraySrv(), 0);
        m_graph.AddPass(std::move(pass));
    }
    {
        // World-space video quads: depth test on, depth write off — same
        // depth-ordering convention as transparent / particle passes.
        // Drawn after TransparentPass so it composites over translucent
        // surfaces (a video panel BEHIND a glass pane reads through correctly
        // via depth test, IN FRONT obscures the glass).
        auto pass = std::make_unique<VideoQuadPass>(m_depthHandle);
        m_videoQuadPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Particles after Transparent: depth-test ON, depth-write OFF (additive pre-alpha, no TAA block).
        auto pass = std::make_unique<ParticleRenderPass>(m_depthHandle);
        m_particleRenderPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Trail ribbons after particles — same depth-test-no-write + alpha blend.
        auto pass = std::make_unique<TrailRenderPass>(m_depthHandle);
        m_trailRenderPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        // Tracer beams (cylindrical billboard, additive); reads depth SRV for soft-particle fade.
        auto pass = std::make_unique<TracerRenderPass>(m_depthHandle);
        m_tracerRenderPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<OutlinePass>(m_depthHandle, m_normalHandle);
        m_outlinePass = pass.get();
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<PickingPass>();
        m_pickingPass = pass.get();
        m_graph.AddPass(std::move(pass));
    }

    m_graph.Compile(m_gfx);

    // Procedural atmospheric sky by default; SetAtmosphereEnabled(false) falls back to HDRI.
    if (m_skyIBLPass)
        m_skyIBLPass->SetAtmosphereEnabled(true);

    // ---- Post-processing compute passes (run outside graph, sequentially) ---
    m_taaPass = std::make_unique<TAAPass>();
    m_taaPass->Init(m_gfx);

    m_fxaaPass = std::make_unique<FXAAPass>();
    m_fxaaPass->Init(m_gfx);

    // Apply default AA mode (sets per-pass enabled flags).
    SetAAMode(m_aaMode);

    m_autoExposurePass = std::make_unique<AutoExposurePass>();
    m_autoExposurePass->Init(m_gfx);

    m_bloomPass = std::make_unique<BloomPass>();
    m_bloomPass->Init(m_gfx);

    m_lensFlarePass = std::make_unique<LensFlarePass>();
    m_lensFlarePass->Init(m_gfx);

    m_toneMapPass = std::make_unique<ToneMapPass>();
    m_toneMapPass->Init(m_gfx);

    m_xegtaoPass = std::make_unique<XeGTAOPass>();
    m_xegtaoPass->Init(m_gfx);

    m_casPass = std::make_unique<CASPass>();
    m_casPass->Init(m_gfx);

    // GlassShatterPass: post-tonemap on restoreCL, composites shards to Tonemap UAV.
    // Active only Trigger()..Trigger()+duration; zero-cost otherwise.
    m_glassShatterPass = std::make_unique<GlassShatterPass>();
    m_glassShatterPass->Init(m_gfx);

    // PostProcess::Stack adapters are non-owning; the unique_ptrs above keep passes alive.
    m_postProcessStack = std::make_unique<PostProcess::Stack>();
    m_postProcessStack->RegisterEffect(std::make_unique<PostProcess::CASEffect>(m_casPass.get()));
    m_postProcessStack->RegisterEffect(std::make_unique<PostProcess::AutoExposureEffect>(m_autoExposurePass.get()));
    m_postProcessStack->RegisterEffect(std::make_unique<PostProcess::BloomEffect>(m_bloomPass.get()));
    m_postProcessStack->RegisterEffect(std::make_unique<PostProcess::LensFlareEffect>(m_lensFlarePass.get()));
    m_postProcessStack->RegisterEffect(std::make_unique<PostProcess::ToneMapEffect>(m_toneMapPass.get()));

    // Two volume sources: standalone registry (editor/scripted) + ECS (VolumeComponent entities).
    m_postProcessVolumes       = std::make_unique<PostProcess::VolumeSystem>();
    m_postProcessEntityVolumes = std::make_unique<PostProcess::EntityVolumeSource>();
    m_postProcessStack->AddVolumeSource(m_postProcessVolumes.get());
    m_postProcessStack->AddVolumeSource(m_postProcessEntityVolumes.get());

    InitSkinningSystems();

    LOG_INFO("Renderer: Compile complete");
}

// ---------------------------------------------------------------------------
void Renderer::SubscribeToWorld(World* world)
{
    if (world == m_subscribedWorld) return;

    if (m_subscribedWorld && m_entityDestroyListenerId != 0)
    {
        m_subscribedWorld->RemoveEntityDestroyListener(m_entityDestroyListenerId);
        m_entityDestroyListenerId = 0;
    }

    m_subscribedWorld = world;

    if (world)
    {
        m_entityDestroyListenerId = world->AddEntityDestroyListener(
            [this](Entity e) { OnEntityDestroyed(e); });
    }
}

void Renderer::OnEntityDestroyed(Entity e)
{
    // Drop the entity's material cache row so a recycled ID doesn't inherit stale handles.
    if (m_texSys)
    {
        if (auto it = m_matTexCache.find(e); it != m_matTexCache.end())
        {
            for (auto& entry : it->second)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, m_gfx);
            m_matTexCache.erase(it);
        }
        if (auto it = m_customMatTexCache.find(e); it != m_customMatTexCache.end())
        {
            for (auto& [_name, entry] : it->second)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, m_gfx);
            m_customMatTexCache.erase(it);
        }
    }

    // Fan out to owned subsystems — each erases its own per-entity maps.
    m_skin.OnEntityDestroyed(e);
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->OnEntityDestroyed(e);
    if (m_particleSystem)
        m_particleSystem->OnEntityDestroyed(m_gfx, e);
    if (m_afterimageSystem)
        m_afterimageSystem->OnEntityDestroyed(e);

    // Beam slot release: listener fires BEFORE pool teardown, so the BeamComponent is still readable.
    if (m_beamSystem && m_subscribedWorld)
    {
        if (auto* pool = m_subscribedWorld->GetPool<BeamComponent>())
        {
            if (auto* beam = pool->Get(e))
            {
                if (beam->beamSlot != 0xFFFFFFFFu)
                {
                    m_beamSystem->Release(beam->beamSlot);
                    beam->beamSlot = 0xFFFFFFFFu;
                }
            }
        }
    }
}

void Renderer::OnWorldClear()
{
    // BeginFrame re-subscribes to whichever World binds next.
    SubscribeToWorld(nullptr);

    m_skin.OnWorldClear();
    if (m_afterimageSystem) m_afterimageSystem->OnWorldClear();
    m_meshMgr.OnWorldClear();  // clears per-library caches + bumps generation
    // (Skinned vertex ring's bindless slots survive automatically — they were
    //  registered via MeshDescriptorHeap::RegisterPersistentBuffer at Init.)
    // DDGI BLAS cache keyed on MeshLibRef → stale after teardown; drop to force rebuild.
    // Passes the DX12 backend so the BLAS result buffers — which the current
    // frame's already-recorded DDGI command list still references — get
    // deferred-released instead of freed inline.
    m_ddgiSceneAS.OnWorldClear(static_cast<GraphicsDX12&>(m_gfx));

    // World::Clear() does NOT fire per-entity destroy listeners (see ECS.h
    // comment) — so OnEntityDestroyed's Release path is bypassed on world
    // reload. We still need to drop refs to balance refcounts, but releasing
    // INLINE breaks the path-hash cache hit case (doc §8.1 Load-then-Release):
    //   inline Release → refcount=0 → FreeSlot → wipes path mapping
    //   next world's Acquire same path → fresh slot → full GPU re-upload.
    // Instead, stash handles in a deferred-release queue. The next BeginFrame
    // runs SyncMaterialTextures BEFORE FlushPendingMatTexReleases, so
    // same-path Acquires bump refcount 1→2; the deferred Release then drops
    // back to 1 and the slot survives. Shared textures across reloads keep
    // their GPU upload (Bistro: ~800 textures stay warm on reload).
    if (m_texSys)
    {
        for (auto& [_e, entries] : m_matTexCache)
            for (auto& entry : entries)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_pendingMatTexReleaseAfterNextSync.push_back(entry.handle);
        for (auto& [_e, byName] : m_customMatTexCache)
            for (auto& [_n, entry] : byName)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_pendingMatTexReleaseAfterNextSync.push_back(entry.handle);
    }
    m_matTexCache.clear();
    m_customMatTexCache.clear();

    // MeshLibrary leak fix: Load() does NOT dedupe by path, so loading a
    // different world allocates fresh VB+IB GPU buffers per .meshlib while
    // the old world's buffers stay resident.  Release with one *world's*
    // lag — release the libs queued at the PREVIOUS OnWorldClear, not the
    // ones queued just now.
    //
    // Why lag and not inline (even with WaitIdleAndReleaseDeferred before
    // OnWorldClear): a full multi-queue sync STILL produced a delayed TDR
    // ~5 s after reload.  Suspected: MeshDescriptorHeap's bindless SRV
    // table keeps stale descriptors pointing at the just-freed memory
    // until a future frame's RegisterBuffer overwrites them; even though
    // no live MeshDescriptor references those slots, *something* (DXR
    // BLAS prefetch?) is still reaching them.  Lagging one world is a
    // bigger margin than any in-flight pipeline can outlive.
    if (m_meshLib)
        for (Resource::Handle h : m_meshLibsPendingRelease)
            m_meshLib->Release(h, m_gfx);
    m_meshLibsPendingRelease = std::move(m_worldMeshLibs);
    m_worldMeshLibs.clear();

    // Prune DDGI BLAS entries whose meshlib slot just transitioned to
    // refCount==0 above. Without this, A→B→A cycles leak ~200 MB / cycle
    // because m_blasCache's ComPtr keeps the BLAS resource resident even
    // after the source VB+IB went away (project_loadworld_phase12).
    m_ddgiSceneAS.PruneStaleBLAS(m_meshLib, static_cast<GraphicsDX12&>(m_gfx));

    // Release decal-material textures so TextureSystem refcounts balance across reloads.
    if (m_texSys)
        m_decalMaterialLibrary.Shutdown(*m_texSys, m_gfx);
    // Cached entity IDs would collide with World::Clear's free-list recycling.
    m_decalSpawner.OnWorldClear();
    m_drawPackets.clear();
    m_skin.GetSkinJobs().clear();
    m_lastWorld = nullptr;
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->ClearAll();
    LOG_INFO("Renderer::OnWorldClear — all per-entity caches invalidated");
}

// ---------------------------------------------------------------------------
void Renderer::BeginFrame(World& world, FrameIndex frame, float dt , uint32_t vpW, uint32_t vpH)
{
	m_deltaTime    = dt;
    m_lastWorld    = &world;
    m_currentFrame = frame;
    // Re-target entity-destroy listener on World change; steady state = pointer compare.
    SubscribeToWorld(&world);
    // Recompile graph (recreates GBuffers) on resize so they sync with PickingPass + scaling.
    const uint32_t winW    = m_gfx.GetWidth();
    const uint32_t winH    = m_gfx.GetHeight();
    const uint32_t renderW = m_gfx.GetRenderWidth();
    const uint32_t renderH = m_gfx.GetRenderHeight();
    if (winW != m_lastWindowW || winH != m_lastWindowH ||
        renderW != m_lastRenderW || renderH != m_lastRenderH)
    {
        m_graph.MarkDirty();
        m_graph.Compile(m_gfx);   // explicit recompile so Execute() never recompiles during parallel recording
        if (m_taaPass) m_taaPass->InvalidateHistory();
        m_lastWindowW = winW;
        m_lastWindowH = winH;
        m_lastRenderW = renderW;
        m_lastRenderH = renderH;
    }

    // SSR resize: EnsureTexture invalidates descriptors on resize; refresh LightingPass SRV
    // before graph.Execute so the new binding is live when LightingPass records.
    if (m_ssrSubsystem && m_lightingPass)
    {
        uint64_t srv = m_ssrSubsystem->OnResize(renderW, renderH);
        // Disabled → pass 0 so the (1 - ssrConf) IBL dampening drains to no-op.
        m_lightingPass->SetSSRResult(m_ssrEnabled ? srv : 0);
    }

    // (MeshDescriptorHeap::BeginFrame and the entire animation chain now
    //  run in TickAnimationChain during the Animation phase BEFORE we get
    //  here. By the time BeginFrame fires, the heap is on a fresh slot and
    //  SkinningOutputComponent offsets / ring writes are in place for
    //  Render's SkinningPass.)

    // (Animation chain now runs in the Animation phase before BeginFrame —
    //  see Renderer::TickAnimationChain. By the time we get here,
    //  SkinningOutputComponent offsets and the PoseRingBuffer / VertexRing
    //  writes are already in place for this frame's SkinningPass.)

    // ---- Unified VFX "Lane B" mailbox drain --------------------------------
    // VFXSpawnSystem (BoneAttachment phase) deposited spawn requests for the
    // Renderer-owned VFX backends it can't reach from the pure-ECS layer
    // (Tracer/Afterimage GPU pools, Decal material library, runtime Mesh load).
    // Drain them here — BEFORE the afterimage/tracer BeginFrame passes consume
    // their own pending-spawn queues — so the effects land this same frame.
    {
        // Tracer: imperative GPU ring (Spawn enqueues; tracer BeginFrame drains).
        if (m_tracerSystem)
            world.ForEach<PendingTracerSpawns>([&](Entity, PendingTracerSpawns& mb){
                for (auto& r : mb.reqs)
                    m_tracerSystem->Spawn(r.start, r.end, r.color, r.width, r.lifetime, r.noiseTexBindless);
                mb.reqs.clear();
            });

        // Afterimage: one Spawn per skinned sub-mesh belonging to the spawner
        // character (resolved via SkeletonRef; legacy single-entity skins match
        // by identity). The target handle guards against a recycled spawner.
        if (m_afterimageSystem)
            world.ForEach<PendingAfterimageSpawns>([&](Entity, PendingAfterimageSpawns& mb){
                for (auto& r : mb.reqs) {
                    if (!world.IsHandleValid(r.target)) continue;
                    const Entity root = r.target.entity;
                    if (auto* pSkin = world.GetPool<MeshSkinnedComponent>()) {
                        const auto& ents = pSkin->Entities();
                        for (size_t i = 0; i < ents.size(); ++i) {
                            const Entity me = ents[i];
                            Entity owner = me;
                            if (auto* ref = world.GetComponent<SkeletonRef>(me)) owner = ref->entity;
                            if (owner == root)
                                m_afterimageSystem->Spawn(me, r.lifetime, r.color);
                        }
                    }
                }
                mb.reqs.clear();
            });

        // Decal: collect-then-spawn — SpawnAtSurface creates entities, so we
        // drain the mailbox into a local list first to keep the pool view stable.
        {
            std::vector<PendingDecalSpawns::Req> decals;
            world.ForEach<PendingDecalSpawns>([&](Entity, PendingDecalSpawns& mb){
                for (auto& r : mb.reqs) decals.push_back(std::move(r));
                mb.reqs.clear();
            });
            for (auto& r : decals) {
                auto mat = m_decalMaterialLibrary.Find(r.materialName);
                if (!mat) {
                    LOG_WARNING("VFX decal: material '%s' not found in DecalMaterialLibrary",
                                r.materialName.c_str());
                    continue;
                }
                m_decalSpawner.SpawnAtSurface(world, mat, r.position, r.normal,
                                              r.size, r.depth, r.lifetime, r.fadeOutDuration, r.rollZ);
            }
        }

        // Mesh VFX: resolve path → GPU mesh + patch the pre-created entity.
        DrainMeshVFXSpawns(world);
    }

    // Resolve pending afterimage spawns + update slot lifetimes. Must run after
    // BuildSkinJobs so SkinningOutputComponent offsets are the freshly-allocated
    // ones for this frame's ring slot.
    if (m_afterimageSystem)
        m_afterimageSystem->BeginFrame(world, m_meshMgr.GetDescriptorHeap(),
                                       m_skin.GetVertexRing(), dt);

    m_shadowFrustum.Compute(world, m_camera, vpW, vpH);
    BuildRenderScene(world);
    UploadFrameData(frame, vpW, vpH);

    // Particle/Tracer CPU bookkeeping — must run before TracerSimPass::Execute reads in Render().
    m_globalTimeSec += dt;
    if (m_tracerSystem)
        m_tracerSystem->BeginFrame(dt, static_cast<uint32_t>(frame), m_globalTimeSec);

    // Beam system: upload control points + params before BeamSimPass::Execute.
    if (m_beamSystem)
    {
        // Patch global time into every active beam's params so wobble runs everywhere.
        m_beamSystem->BeginFrame(m_globalTimeSec);
    }

    if (m_particleSystem)
    {
        m_particleSystem->CollectEmitters(world, dt, static_cast<uint32_t>(frame));
        m_particleSystem->UpdateSystemCB(dt, static_cast<uint32_t>(frame));

        // Resolve pending mesh-shape emitters → bindless mesh-desc + world matrix.
        for (const auto& req : m_particleSystem->GetPendingMeshResolves())
        {
            const Entity srcE = req.sourceEntity;
            if (srcE == NullEntity) continue;

            DirectX::XMFLOAT4X4 worldMat;
            DirectX::XMStoreFloat4x4(&worldMat, DirectX::XMMatrixIdentity());
            if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(srcE))
                worldMat = gt->matrix;

            uint32_t descSlot   = 0xFFFFFFFFu;
            uint32_t indexCount = 0;

            // Try MeshLibRef path (scene meshes via MeshLibrary).
            if (MeshLibRef* mlr = world.GetComponent<MeshLibRef>(srcE);
                mlr && mlr->IsValid() && m_meshLib)
            {
                descSlot = m_meshMgr.RegisterMeshLibMesh(*m_meshLib, *mlr);
                if (const auto* entry = m_meshLib->GetEntry(mlr->libHandle, mlr->meshId))
                    indexCount = entry->indexCount;
            }
            // Fallback: primitive MeshHandle (cube/sphere/cone etc).
            else if (const MeshHandle* mh = world.GetComponent<MeshHandle>(srcE);
                     mh && mh->IsValid()
                     && mh->gpuMeshID < static_cast<uint32_t>(PrimitiveMeshType::Count))
            {
                const auto& mesh = m_meshMgr.GetPrimitive(mh->gpuMeshID);
                descSlot   = mesh.meshDescSlot;
                indexCount = mesh.indexCount;
            }

            if (descSlot != RHI::kInvalidBufferIndex && indexCount >= 3)
            {
                m_particleSystem->PatchMeshEmitter(
                    req.emitterSlotIndex, descSlot, indexCount, worldMat);
            }
        }
    }

    // ---- Trail system — CPU-side per-frame bookkeeping ---------------------
    if (m_trailSystem)
    {
        m_trailSystem->CollectTrails(world, dt);
        m_trailSystem->UpdateSystemCB(dt);
    }
}

// ---------------------------------------------------------------------------
// TickAnimationChain — extracted from BeginFrame in the Phase-based-scheduler
// refactor so the entire animation chain (character state → AnimationSystem
// sample → IK → ChainPhysics → LocalToWorld → Socket/Follow → bone AABB
// merge → SkinMatrix → BuildSkinJobs) can live in the scheduler's Animation
// phase. Called from RendererAnimationChainSystem each frame BEFORE
// BeginFrame so SkinningOutputComponent offsets and ring-buffer writes are
// already in place when SkinningPass runs in Render(). No-op if the Skinning
// subsystem isn't initialised.
//
// Note: animation visibility culling (m_view.boundingFrustum /
// m_shadowFrustum) uses PREVIOUS frame's frustums — same behaviour as
// pre-refactor when this block ran inside BeginFrame before UploadFrameData
// recomputed them. Documented at the original comment site.
void Renderer::TickAnimationChain(World& world, FrameIndex frame, float dt)
{
    // Rotate the MeshDescriptor triple-buffer to a fresh GPU slot FIRST —
    // must run BEFORE BuildSkinJobs / AfterimageSystem::BeginFrame because
    // both call UpdateMesh(ThisFrame) which writes into the active slot.
    // Memcpys the CPU master into the new slot so any transient patches
    // lingering from 3 frames ago are wiped. Unconditional — non-skinning
    // meshes also live in this heap and need a valid current slot bound
    // in Render(), so this MUST run even when m_skin isn't initialised.
    //
    // Slot source MUST be m_gfx.GetFrameIndex() (== swap-chain backbuffer
    // index), NOT the App's monotonic ctx.frame: GraphicsDX12 fences
    // m_frameFenceValues[GetFrameIndex()], so the slot we rotate to and
    // memcpy into must be the same one App's WaitForNextFrameSlot() call
    // (just above the Animation phase in App::Run) just drained. The two
    // can drift apart after swap-chain recreation (resize / Alt+Enter).
    m_meshMgr.GetDescriptorHeap().BeginFrame(m_gfx.GetFrameIndex());

    if (!m_skin.IsInitialised()) return;

    // Animation culling uses PREVIOUS frame visibility — 1-frame stale pose for new-in-frustum.
    const std::unordered_set<Entity>* animActivePtr = nullptr;
    if (m_skin.IsAnimCullingEnabled())
    {
        m_skin.GetAnimVisibleSet().clear();

        int totalSkeletons = 0;
        int noAabbCount    = 0;
        int frustumPass    = 0;
        int shadowPass     = 0;
        int culledCount    = 0;

        // Skeleton roots usually lack WorldAabb — merge Children AABBs (root as fallback).
        // Iterate SkeletonComponent pool dense array (avoids 22k hash lookups on Bistro).
        auto* pSkel      = world.GetPool<SkeletonComponent>();
        auto* pWorldAabb = world.GetPool<WorldAabb>();
        auto* pChildren  = world.GetPool<Children>();
        const size_t skelN = pSkel ? pSkel->Data().size() : 0;
        const auto&  skelEnts = pSkel ? pSkel->Entities() : std::vector<Entity>{};
        const auto&  skelData = pSkel ? pSkel->Data()     : std::vector<SkeletonComponent>{};
        for (size_t si = 0; si < skelN; ++si)
        {
            const Entity e = skelEnts[si];
            const SkeletonComponent* skel = &skelData[si];
            if (skel->assetIndex == kInvalidAnimHandle) continue;

            ++totalSkeletons;

            // Build merged AABB from skeleton root + its children.
            using namespace DirectX;
            XMVECTOR vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
            XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
            bool hasAabb = false;

            auto mergeAabb = [&](const WorldAabb* wa) {
                if (!wa) return;
                vmin = XMVectorMin(vmin, XMVectorSet(wa->min.x, wa->min.y, wa->min.z, 0));
                vmax = XMVectorMax(vmax, XMVectorSet(wa->max.x, wa->max.y, wa->max.z, 0));
                hasAabb = true;
            };

            // Check root entity's own AABB
            if (pWorldAabb) mergeAabb(pWorldAabb->Get(e));

            // Merge children AABBs (mesh sub-entities)
            const Children* ch = pChildren ? pChildren->Get(e) : nullptr;
            if (ch)
            {
                for (Entity child : ch->entities)
                    if (pWorldAabb) mergeAabb(pWorldAabb->Get(child));
            }

            if (!hasAabb) { m_skin.GetAnimVisibleSet().insert(e); ++noAabbCount; continue; }

            XMFLOAT3 lo, hi;
            XMStoreFloat3(&lo, vmin);
            XMStoreFloat3(&hi, vmax);

            BoundingBox bb;
            bb.Center  = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
            bb.Extents = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f };

            // Camera frustum test — using BoundingFrustum::Intersects (fast 6-plane path
            // suspected of startup-flicker via ExtractFrustumPlanes).
            if (m_view.boundingFrustum.Intersects(bb))
            {
                m_skin.GetAnimVisibleSet().insert(e);
                ++frustumPass;
                continue;
            }

            // Test shadow frustums — shadow casters need animation too.
            if (m_shadowFrustum.IsValid())
            {
                bool inShadow = false;
                for (int sc = 0; sc < ShadowFrustumCompute::kCascadeCount; ++sc)
                    if (ShadowFrustumCompute::AabbVs6Planes(bb, m_shadowFrustum.GetPlanes(sc).planes)) { inShadow = true; break; }
                if (inShadow) { m_skin.GetAnimVisibleSet().insert(e); ++shadowPass; continue; }
            }

            ++culledCount;
        }
        animActivePtr = &m_skin.GetAnimVisibleSet();
    }

    // All per-frame upload rings rotate on m_gfx.GetFrameIndex() — see the
    // MeshDescriptorHeap comment above for why ctx.frame is not the right
    // source (swap-chain backbuffer index is what App's WaitForNextFrameSlot
    // drained the slot fence on).
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    m_skin.GetPoseRingBuffer().BeginFrame(frameSlot);
    // Rotate ring BEFORE any custom-material packing happens this frame.
    m_customMatCbvRing.BeginFrame(frameSlot);
    m_customMatSrvRing.BeginFrame(frameSlot);
    m_skin.GetVertexRing().BeginFrame(frameSlot);
    // Character state machine — advances Lua-defined state transitions
    // (primary/secondary cross-fade + clip lazy-bind) before
    // AnimationSystem samples, so the sampler picks up freshly-flipped
    // clips on the same frame the BT / Logic script switched states.
    if (auto* csys = m_skin.GetCharacterStateSystem())
        csys->Update(world, dt, m_skin.GetClipLibrary());
    m_skin.GetAnimationSystem()->Update(world, dt, animActivePtr);
    // FootIK target solver — raycasts the physics world straight down
    // from each foot effector and rewrites the IK control bone's pose
    // BEFORE the CCD solver runs, so PMX characters' feet snap to
    // terrain rather than the clip's flat-ground baseline. No-op if
    // physics isn't wired or no entity carries FootIKComponent.
    m_skin.GetFootIKSystem()->Update(world,
                                     m_skin.GetPhysicsSystem(),
                                     animActivePtr);
    m_skin.GetIKSystem()->Update(world, animActivePtr);
    m_skin.GetAnimationSystem()->ApplyPostIKGrants(world);
    m_skin.GetChainPhysicsSystem()->Update(world, dt, animActivePtr);
    m_skin.GetLocalToWorldSystem()->Update(world, m_skin.GetPoseRingBuffer(), animActivePtr);
    m_skin.GetSocketSystem()->Update(world);
    // Flat-binding followers update post-Socket to see fresh bone transforms.
    m_skin.GetFollowSystem()->Update(world);

    // Decal lifetimes tick after transform/follow so expired entities are gone before upload.
    m_decalLifetimeSystem.Update(world, dt);

    // Drain queued events; subscribers see consistent post-simulation state. Reentrancy deferred.
    EventBus::Get().DispatchAll();

    // ---- Skeleton-level bone AABB merge (parallel per-skeleton) ----------------
    {
        constexpr int kMaxAABBChildren = 64;
        struct AABBJob {
            const XMFLOAT4X4*      matrices;
            const SkeletonAsset*   asset;
            uint32_t               boneCount;
            const GlobalTransform* gt;
            WorldAabb*             rootAabb;
            WorldAabb* childAabbs[64];
            int        childCount;
        };
        std::vector<AABBJob> aabbJobs;

        // Iterate SkeletonComponent pool directly — avoids hash lookups; cache pool pointers.
        auto* pSkel2      = world.GetPool<SkeletonComponent>();
        auto* pGlobalXf2  = world.GetPool<GlobalTransform>();
        auto* pWorldAabb2 = world.GetPool<WorldAabb>();
        auto* pChildren2  = world.GetPool<Children>();
        const size_t skelN2 = pSkel2 ? pSkel2->Data().size() : 0;
        const auto&  skelEnts2 = pSkel2 ? pSkel2->Entities() : std::vector<Entity>{};
        const auto&  skelData2 = pSkel2 ? pSkel2->Data()     : std::vector<SkeletonComponent>{};
        const uint32_t poseWriteHead = m_skin.GetPoseRingBuffer().GetWriteHead();
        aabbJobs.reserve(skelN2);
        for (size_t si = 0; si < skelN2; ++si)
        {
            const Entity e = skelEnts2[si];
            const SkeletonComponent* skel = &skelData2[si];
            if (skel->boneCount == 0) continue;
            if (skel->poseByteOffset == ~0u) continue;
            const uint32_t boneEndBytes = skel->poseByteOffset + skel->boneCount * 64;
            if (boneEndBytes > poseWriteHead * 64) continue;

            const XMFLOAT4X4* matrices = m_skin.GetPoseRingBuffer().ReadMapped(skel->poseByteOffset);
            if (!matrices) continue;

            AABBJob job;
            job.matrices  = matrices;
            job.boneCount = skel->boneCount;
            job.asset     = (skel->assetIndex != kInvalidSkeletonIndex)
                ? &m_skin.GetSkeletonRegistry().Get(skel->assetIndex) : nullptr;
            job.gt        = pGlobalXf2  ? pGlobalXf2->Get(e)  : nullptr;
            job.rootAabb  = pWorldAabb2 ? pWorldAabb2->Get(e) : nullptr;
            job.childCount = 0;

            const Children* ch = pChildren2 ? pChildren2->Get(e) : nullptr;
            if (ch)
            {
                for (Entity child : ch->entities)
                {
                    if (job.childCount >= kMaxAABBChildren) break;
                    WorldAabb* cwa = pWorldAabb2 ? pWorldAabb2->Get(child) : nullptr;
                    if (cwa) job.childAabbs[job.childCount++] = cwa;
                }
            }
            aabbJobs.push_back(job);
        }

        auto processAABB = [](const AABBJob& j)
        {
            XMVECTOR vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
            XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);

            if (j.asset && j.asset->hasBoneAABBs)
            {
                for (uint32_t b = 0; b < j.boneCount; ++b)
                {
                    const auto& ra = j.asset->boneRestAABBs[b];
                    if (ra.localMin.x > ra.localMax.x) continue;
                    const XMMATRIX boneMat = XMLoadFloat4x4(&j.matrices[b]);
                    const float lx[2] = { ra.localMin.x, ra.localMax.x };
                    const float ly[2] = { ra.localMin.y, ra.localMax.y };
                    const float lz[2] = { ra.localMin.z, ra.localMax.z };
                    for (int iz = 0; iz < 2; ++iz)
                    for (int iy = 0; iy < 2; ++iy)
                    for (int ix = 0; ix < 2; ++ix)
                    {
                        XMVECTOR w = XMVector3TransformCoord(
                            XMVectorSet(lx[ix], ly[iy], lz[iz], 1.f), boneMat);
                        vmin = XMVectorMin(vmin, w);
                        vmax = XMVectorMax(vmax, w);
                    }
                }
            }
            else
            {
                for (uint32_t b = 0; b < j.boneCount; ++b)
                {
                    XMVECTOR bp = XMVectorSet(
                        j.matrices[b]._41, j.matrices[b]._42, j.matrices[b]._43, 0);
                    vmin = XMVectorMin(vmin, bp);
                    vmax = XMVectorMax(vmax, bp);
                }
            }

            const float kPad = 0.05f;
            vmin = XMVectorSubtract(vmin, XMVectorReplicate(kPad));
            vmax = XMVectorAdd     (vmax, XMVectorReplicate(kPad));

            if (j.gt)
            {
                const XMMATRIX worldMat = XMLoadFloat4x4(&j.gt->matrix);
                XMFLOAT3 lo, hi; XMStoreFloat3(&lo, vmin); XMStoreFloat3(&hi, vmax);
                const float cx[2]={lo.x,hi.x}, cy[2]={lo.y,hi.y}, cz[2]={lo.z,hi.z};
                vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
                vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
                for (int iz=0;iz<2;++iz) for (int iy=0;iy<2;++iy) for (int ix=0;ix<2;++ix)
                {
                    XMVECTOR w = XMVector3TransformCoord(
                        XMVectorSet(cx[ix],cy[iy],cz[iz],1.f), worldMat);
                    vmin = XMVectorMin(vmin, w);
                    vmax = XMVectorMax(vmax, w);
                }
            }

            XMFLOAT3 rmin, rmax;
            XMStoreFloat3(&rmin, vmin);
            XMStoreFloat3(&rmax, vmax);

            if (j.rootAabb) { j.rootAabb->min = rmin; j.rootAabb->max = rmax; }
            for (int c = 0; c < j.childCount; ++c)
            { j.childAabbs[c]->min = rmin; j.childAabbs[c]->max = rmax; }
        };

        if (aabbJobs.size() > 1)
        {
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(aabbJobs.size()),
                [&](uint32_t i) { processAABB(aabbJobs[i]); });
        }
        else if (!aabbJobs.empty())
        {
            processAABB(aabbJobs[0]);
        }
    }

    m_skin.GetSkinMatrixSystem()->Update(world, m_skin.GetPoseRingBuffer(), m_skin.GetVertexRing());
    m_skin.BuildSkinJobs(world, m_meshMgr);
}

// ---------------------------------------------------------------------------
DrawList Renderer::GetDrawList(DrawFilter f) const
{
    const auto* begin = m_drawPackets.data();
    const auto* end   = begin + m_drawPackets.size();
    const auto* first = std::find_if(begin, end,
        [f](const DrawPacket& dp) { return dp.filter == f; });
    if (first == end) return {};
    const auto* last = std::find_if(first, end,
        [f](const DrawPacket& dp) { return dp.filter != f; });
    return { first, static_cast<size_t>(last - first) };
}

// ---------------------------------------------------------------------------
void Renderer::EnsureWorkers()
{
    if (m_workersStarted) return;
    m_renderWorkers[0].index = 0;
    m_renderWorkers[0].Start(L"RenderWorker_Shadow");
    m_renderWorkers[2].index = 2;
    m_renderWorkers[2].Start(L"RenderWorker_Compute");
    m_workersStarted = true;
    LOG_INFO("Renderer: started render workers (Shadow, Compute)");
}

// ---------------------------------------------------------------------------
// AA mode selector — drives per-pass enabled flags. Jitter follows TAA's
// IsEnabled() in BuildRenderScene, so toggling TAA off here also disables
// projection jitter (FXAA-only mode renders without jitter as expected).
void Renderer::SetAAMode(AAMode m)
{
    m_aaMode = m;
    UpdateAAEnabled();
}

// ---------------------------------------------------------------------------
// Global view mode (Lit/Unlit/Wireframe). The per-frame plumbing — LightCB
// .viewMode, FILL_MODE_WIREFRAME on the geometry passes, and background-pass
// suppression — is applied in Render_BindFrameResources from m_viewMode. Here
// we only need to refresh the AA enable, since Wireframe force-disables it.
void Renderer::SetViewMode(ViewMode m)
{
    m_viewMode = m;
    UpdateAAEnabled();
}

// ---------------------------------------------------------------------------
// Single source of truth for TAA/FXAA enable: derived from m_aaMode but
// forced OFF in Wireframe view (thin lines ghost/smear under temporal AA, and
// disabling TAA also drops projection jitter — desired for a stable wireframe).
void Renderer::UpdateAAEnabled()
{
    const bool wire   = (m_viewMode == ViewMode::Wireframe);
    const bool taaOn  = !wire && ((m_aaMode == AAMode::TAA)  || (m_aaMode == AAMode::FXAA_TAA));
    const bool fxaaOn = !wire && ((m_aaMode == AAMode::FXAA) || (m_aaMode == AAMode::FXAA_TAA));
    if (m_taaPass)  m_taaPass->SetEnabled(taaOn);
    if (m_fxaaPass) m_fxaaPass->SetEnabled(fxaaOn);
}

// ---------------------------------------------------------------------------
void Renderer::ReloadShaders()
{
    // Standalone passes first — these aren't visited by m_graph.ReloadShaders.
    if (m_shadowPass) m_shadowPass->ReloadShaders(m_gfx);
    // DDGI's RTPSO + relight CS aren't in the render graph either — its own
    // ReloadShaders rebuilds the state object from disk on hot edit.
    if (m_ddgiPass)   m_ddgiPass->ReloadShaders(m_gfx);
    if (m_uiPass)         m_uiPass->ReloadShaders();
    if (m_worldUIPass)    m_worldUIPass->ReloadShaders();
    // SSR sub-passes live behind the subsystem; this re-fetches all 7
    // compute shaders + rebuilds PSOs + resets temporal history.
    if (m_ssrSubsystem) m_ssrSubsystem->ReloadShaders(m_gfx);

    m_graph.ReloadShaders(m_gfx);
}

// ---------------------------------------------------------------------------
void Renderer::Render_BindFrameResources(uint32_t frameSlot)
{
    m_graph.BindConstantBuffer("PerView",     m_perObjectCB.CurrentBuffer(m_gfx));
    m_graph.BindConstantBuffer("LightCB",     m_lightCB.CurrentBuffer(m_gfx));
    if (m_terrainCB.IsValid())
        m_graph.BindConstantBuffer("TerrainParams", m_terrainCB.CurrentBuffer(m_gfx));
    if (m_skyboxPass)
        m_graph.BindConstantBuffer("SkyCB", m_skyboxPass->GetSkyCB(m_gfx));
    if (m_volFogPass)
        m_graph.BindConstantBuffer("VolApplyCB", m_volFogPass->GetApplyCB(m_gfx));
    m_graph.BindBuffer("InstanceBuffer",      m_instanceBuffer[frameSlot]);
    m_graph.BindBuffer("MeshDescriptors",     m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer());
    m_graph.BindBuffer("MaterialBuffer",      m_materialBuffer[frameSlot]);

    // Wire the (per-frame) terrain CB into ShadowPass — that's a standalone pass
    // outside the graph so it needs the raw buffer pointer rebound each frame.
    if (m_shadowPass && m_terrainCB.IsValid())
        m_shadowPass->SetTerrainParamsCB(&m_terrainCB.CurrentBuffer(m_gfx));
    m_graph.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());
    m_graph.SetDrawList(&m_drawPackets);

    // ---- Global view-mode application (Lit / Unlit / Wireframe) -------------
    // Color for the deferred geometry (opaque + terrain) is handled in
    // Lighting.ps via LightCB.viewMode; transparent handles it in its forward
    // shader. Here we only drive the CPU-side state Wireframe needs: flip the
    // geometry passes to FILL_MODE_WIREFRAME and hide the background-filling
    // passes so the wire reads as flat lines on a dark background. Set every
    // frame so leaving Wireframe restores prior state with no transition logic.
    {
        const bool wire = (m_viewMode == ViewMode::Wireframe);
        if (m_gbufferPass)     m_gbufferPass->SetWireframe(wire);
        if (m_terrainPass)     m_terrainPass->SetWireframe(wire);
        if (m_transparentPass) m_transparentPass->SetWireframe(wire);
        if (m_skyboxPass)      m_skyboxPass->SetViewModeHidden(wire);
        if (m_cloudPass)       m_cloudPass->SetViewModeHidden(wire);
        if (m_volFogPass)      m_volFogPass->SetViewModeHidden(wire);
    }

    if (m_shadowPass && m_lightingPass)
        m_lightingPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());

    // Bind clustered lighting SRVs to LightingPass.
    if (m_clusterPass && m_lightingPass)
        m_lightingPass->SetClusterSRVs(
            m_clusterPass->GetLightsSRVHandle(),
            m_clusterPass->GetLightIndexSRVHandle(),
            m_clusterPass->GetLightGridSRVHandle());

    // Spot shadow atlas + per-slice VP buffer; consumed by both Lighting and VolumetricFog.
    // VP buffer is triple-buffered — feed the current frame's SRV handle.
    if (m_spotShadowPass && m_lightingPass)
        m_lightingPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv[frameSlot]);
    if (m_spotShadowPass && m_volFogPass)
        m_volFogPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv[frameSlot]);

    // Bind NPR ramp texture to LightingPass (cached in BuildRenderScene).
    if (m_lightingPass)
        m_lightingPass->SetRampTexture(m_nprRampTexHandle);

    // Bind MaterialBuffer SRV so the Lighting pass can read per-material NPR params.
    // SRV handle resolved per frame from the current slot of the triple-buffered ring.
    if (m_lightingPass && m_materialBuffer[frameSlot].IsValid())
        m_lightingPass->SetMaterialBuffer(m_gfx.GetBufferSRVGpuHandle(m_materialBuffer[frameSlot]));

    // Bind previous frame's XeGTAO SSAO texture to LightingPass (one-frame latency).
    if (m_lightingPass && m_xegtaoPass && m_ssaoEnabled)
        m_lightingPass->SetSSAOHandle(m_xegtaoPass->GetAOSrvHandle());
    else if (m_lightingPass)
        m_lightingPass->SetSSAOHandle(0);

    // Feed per-frame state to the volumetric fog pass.
    if (m_volFogPass && m_lightCB.Current(m_gfx))
    {
        auto* lb = m_lightCB.Current(m_gfx);

        // Matrices uploaded TRANSPOSED for HLSL row-vector mul(pos, matrix). Pair invViewProj
        // with un-jittered VP — voxel-derived worldPos must be jitter-stable for reprojection.
        using namespace DirectX;
        DirectX::XMFLOAT4X4 vp{},  invVp{}, prevVp{};
        XMMATRIX vpLoaded      = XMLoadFloat4x4(&m_view.viewProjMatrixNoJitter);
        XMMATRIX prevLoaded    = XMLoadFloat4x4(&m_taaJitter.GetPrevViewProjNoJitter());
        XMMATRIX invVpNoJitter = XMMatrixInverse(nullptr, vpLoaded);
        XMStoreFloat4x4(&vp,     XMMatrixTranspose(vpLoaded));
        XMStoreFloat4x4(&prevVp, XMMatrixTranspose(prevLoaded));
        XMStoreFloat4x4(&invVp,  XMMatrixTranspose(invVpNoJitter));

        DirectX::XMFLOAT3 camPos { lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
        // Per-frame index drives temporal jitter; wraps every 8 frames for clean Halton cycle.
        static uint32_t s_volFogFrame = 0;
        m_volFogPass->SetFrameState(vp, invVp, prevVp, camPos,
                                     m_view.nearZ, m_view.farZ,
                                     s_volFogFrame++);

        DirectX::XMFLOAT3 sunDir   { lb->lightDir[0],   lb->lightDir[1],   lb->lightDir[2] };
        DirectX::XMFLOAT3 sunColor { lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
        // Sun only contributes when a directional light has VolumetricLightComponent.
        const float sunStrength = m_sunVolumetric ? m_sunVolumetricScale : 0.0f;
        m_volFogPass->SetSun(sunDir, sunColor, sunStrength);

        DirectX::XMFLOAT3 camFwd { lb->cameraForward[0], lb->cameraForward[1], lb->cameraForward[2] };
        m_volFogPass->SetCameraForward(camFwd);
        // Forward the camera-stack hard-cut signal to every temporal-history
        // pass. When the LiveCamera reports !historyValid (HardCutTo, first
        // frame, cinematic shot boundary), all of them invalidate together.
        m_volFogPass->SetExternalHistoryValid(m_camera.historyValid);
        if (m_xegtaoPass) m_xegtaoPass->SetExternalHistoryValid(m_camera.historyValid);

        if (m_shadowPass)
        {
            DirectX::XMFLOAT4X4 sm[3];
            std::memcpy(&sm[0], lb->shadowMatrix[0], sizeof(sm[0]));
            std::memcpy(&sm[1], lb->shadowMatrix[1], sizeof(sm[1]));
            std::memcpy(&sm[2], lb->shadowMatrix[2], sizeof(sm[2]));
            DirectX::XMFLOAT3 splits { lb->cascadeSplits[0],
                                      lb->cascadeSplits[1],
                                      lb->cascadeSplits[2] };
            m_volFogPass->SetShadowState(m_shadowPass->GetShadowArrayGpuHandle(),
                                         sm, splits,
                                         lb->shadowMapTexelSize,
                                         lb->shadowBias,
                                         lb->shadowStrength);
        }

        // Per-frame volumetric-only light list (filtered by VolumetricLightComponent above).
        m_volFogPass->SetVolumetricLights(m_volumetricLights);

        // Tier-2 triangle-precision occupancy grid for spotlight shadowing without shadow maps.
        // Camera-snapped to voxel size so sub-voxel drift doesn't force re-voxelise.
        if (m_sceneVoxelPass && m_volFogPass->IsEnabled())
        {
            using namespace DirectX;
            constexpr float    kHalfExtent = 32.0f;            // ±32 m cube
            constexpr uint32_t kDim        = SceneVoxelPass::kGridDim;
            constexpr float    kVoxelSize  = (2.0f * kHalfExtent) / float(kDim);

            auto snap = [](float v) {
                return std::floor(v / kVoxelSize + 0.5f) * kVoxelSize;
            };
            const XMFLOAT3 centre {
                snap(m_view.cameraPosition.x),
                snap(m_view.cameraPosition.y),
                snap(m_view.cameraPosition.z)
            };
            const XMFLOAT3 gMin { centre.x - kHalfExtent,
                                  centre.y - kHalfExtent,
                                  centre.z - kHalfExtent };
            const XMFLOAT3 gMax { centre.x + kHalfExtent,
                                  centre.y + kHalfExtent,
                                  centre.z + kHalfExtent };
            m_sceneVoxelPass->SetGridBounds(gMin, gMax);

            // Shader bindings: InstanceBuffer + MeshDescriptors SRVs + bindless g_Buffers[] table.
            m_sceneVoxelPass->SetSceneBindings(
                m_gfx.GetBufferSRVGpuHandle(m_instanceBuffer[frameSlot]),
                m_gfx.GetBufferSRVGpuHandle(m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer()),
                m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());

            // Per-mesh dispatches from opaque packets only; CPU-side WorldAabb cull keeps cost
            // bounded to the grid neighbourhood (avoids scene-wide dispatch jitter).
            m_sceneVoxelPass->ResetDraws();
            World* lastWorld = m_lastWorld;
            for (const DrawPacket& dp : m_drawPackets)
            {
                if (dp.filter != DrawFilter::Opaque) continue;
                if (dp.vertexOrIndexCount < 3)       continue;
                const uint32_t triCount = dp.vertexOrIndexCount / 3u;
                for (uint32_t inst = 0; inst < dp.instanceCount; ++inst)
                {
                    const uint32_t instSlot = dp.instanceOffset + inst;
                    if (lastWorld && instSlot < kMaxInstances)
                    {
                        const Entity e = m_instanceSlotToEntity[instSlot];
                        if (e != NullEntity)
                        {
                            const WorldAabb* wa = lastWorld->GetComponent<WorldAabb>(e);
                            if (wa && wa->min.x <= wa->max.x)
                            {
                                if (wa->max.x < gMin.x || wa->min.x > gMax.x) continue;
                                if (wa->max.y < gMin.y || wa->min.y > gMax.y) continue;
                                if (wa->max.z < gMin.z || wa->min.z > gMax.z) continue;
                            }
                        }
                    }
                    m_sceneVoxelPass->PushDraw(dp.meshDescriptorIndex,
                                               instSlot,
                                               triCount);
                }
            }

            m_volFogPass->SetVoxelOcclusion(
                m_sceneVoxelPass->GetOccupancySrvHandle(), gMin, gMax, kDim);
        }
        else if (m_volFogPass)
        {
            // Disabled: force "no grid bound" branch + clear queued draws so the empty bail fires.
            if (m_sceneVoxelPass) m_sceneVoxelPass->ResetDraws();
            m_volFogPass->SetVoxelOcclusion(
                0, DirectX::XMFLOAT3{0,0,0}, DirectX::XMFLOAT3{0,0,0}, 0);
        }
    }
}

// ---------------------------------------------------------------------------
void Renderer::Render_ComputePrepass()
{
    // ---- Phase 0: Skinning CS — writes SkinnedVertexRing UAVs; trailing UAV barrier for GBuffer.
    //              Afterimage capture runs on the same CL, right after SkinningPass,
    //              so it sees the freshly-skinned vertices and emits its own UAV barrier
    //              on the snapshot pool before any later pass reads from it.
    if (m_skin.GetSkinningPass() && m_skin.IsInitialised() && !m_skin.GetSkinJobs().empty())
    {
        RHI::CommandList skinCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        skinCL.gfx = &m_gfx;
        m_skin.GetSkinningPass()->Execute(skinCL);
        if (m_afterimageCapturePass && m_afterimageSystem &&
            !m_afterimageSystem->GetCaptureJobs().empty())
        {
            m_afterimageCapturePass->Execute(skinCL);
        }
        // skinCL is submitted by EndFrame in allocation order (before GBuffer CLs)
    }

    // ---- Phase 0.5: Particle sim CS (emit+update) before render passes; trailing UAV barrier.
    if (m_particleSimPass && m_particleSystem)
    {
        // Feed mesh-shape sampling; no-op for non-mesh emitters.
        m_particleSimPass->SetMeshDescriptorBinding(
            &m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer(),
            m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());

        RHI::CommandList pCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        pCL.gfx = &m_gfx;
        m_particleSimPass->Execute(pCL);
    }

    // ---- Phase 0.6: Trail update compute -----------------------------------
    if (m_trailUpdatePass && m_trailSystem)
    {
        RHI::CommandList tCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        tCL.gfx = &m_gfx;
        m_trailUpdatePass->Execute(tCL);
    }

    // ---- Phase 0.7: Tracer emit+age CS (same UAV-barrier pattern as ParticleSimPass).
    if (m_tracerSimPass && m_tracerSystem)
    {
        RHI::CommandList trCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        trCL.gfx = &m_gfx;
        m_tracerSimPass->Execute(trCL);
    }

    // ---- Phase 0.8: Beam tube CS — fills shared PVF UAVs; trailing UAV barrier for SRV reads.
    if (m_beamSimPass && m_beamSystem)
    {
        RHI::CommandList bCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        bCL.gfx = &m_gfx;
        m_beamSimPass->Execute(bCL);
    }

    // Particle render frame data: derive cam right/up from view matrix rows (row-major).
    if (m_particleRenderPass)
    {
        const auto& vm = m_view.viewMatrix;
        DirectX::XMFLOAT3 camRight = { vm._11, vm._21, vm._31 };
        DirectX::XMFLOAT3 camUp    = { vm._12, vm._22, vm._32 };
        m_particleRenderPass->SetFrameData(
            m_view.viewProjMatrix, camRight, camUp, 1.0f);
    }

    // ---- Trail render pass frame data --------------------------------------
    if (m_trailRenderPass)
    {
        m_trailRenderPass->SetFrameData(m_view.viewProjMatrix, m_view.cameraForward);
    }

    // ---- Tracer render pass frame data -------------------------------------
    if (m_tracerRenderPass)
    {
        m_tracerRenderPass->SetFrameData(
            m_view.viewProjMatrix,
            m_camera.position,
            m_globalTimeSec,
            m_camera.nearZ,
            m_camera.farZ);
    }
}

// ---------------------------------------------------------------------------
RHI::CommandList Renderer::Render()
{
    EnsureWorkers();

    // ---- Frame bindings (read-only for all workers this frame) -------------
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    Render_BindFrameResources(frameSlot);

    // Pre-capture SRV handles on main thread before workers kick (graph won't recompile mid-frame).
    const bool needPostCapture = m_taaPass || m_xegtaoPass;
    const RHI::Texture* taaDepthTex    = needPostCapture ? m_graph.GetPhysicalTexture(m_depthHandle)    : nullptr;
    const RHI::Texture* taaSurfaceTex  = m_taaPass       ? m_graph.GetPhysicalTexture(m_surfaceHandle)  : nullptr;
    // Velocity feeds BOTH TAA and XeGTAO temporal reprojection — capture it
    // whenever either is active (not TAA-only) so XeGTAO gets real motion
    // vectors even when TAA is disabled.
    const RHI::Texture* taaVelocityTex = needPostCapture  ? m_graph.GetPhysicalTexture(m_velocityHandle) : nullptr;
    const RHI::Texture* normalTex      = m_xegtaoPass    ? m_graph.GetPhysicalTexture(m_normalHandle)   : nullptr;
    const uint64_t      depthSrv       = taaDepthTex    ? m_gfx.GetTextureSRVGpuHandle(*taaDepthTex)    : 0;
    const uint64_t      surfaceSrv     = taaSurfaceTex  ? m_gfx.GetTextureSRVGpuHandle(*taaSurfaceTex)  : 0;
    const uint64_t      velocitySrv    = taaVelocityTex ? m_gfx.GetTextureSRVGpuHandle(*taaVelocityTex) : 0;
    const uint64_t      normalSrv      = normalTex      ? m_gfx.GetTextureSRVGpuHandle(*normalTex)      : 0;
    // Stencil-plane view of the depth buffer — TAA uses it to identify
    // pixels tagged by OutlinePass and weaken their history blend weight.
    // Zero when the depth format isn't D24_S8 — TAA disables the feature.
    const uint64_t      stencilSrv     = taaDepthTex    ? m_gfx.GetTextureStencilSRVGpuHandle(*taaDepthTex) : 0;

    // ---- Compute prepass: skinning / particle / trail / tracer / beam CS. ----
    Render_ComputePrepass();

    // ---- Phase 1: Open shadow CL and build its per-frame context -----------
    RHI::CommandList shadowCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    shadowCL.gfx = &m_gfx;

    RG::RenderContext shadowCtx;
    shadowCtx.SetDimensions(m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());
    shadowCtx.SetDrawList(&m_drawPackets);
    shadowCtx.SetBuffer("InstanceBuffer",  m_instanceBuffer[frameSlot]);
    shadowCtx.SetBuffer("MeshDescriptors", m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer());
    // Material buffer + bindless texture table required by Shadow.ps ALPHA_TEST path.
    shadowCtx.SetBuffer("MaterialBuffer",  m_materialBuffer[frameSlot]);
    shadowCtx.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());
    shadowCL.ctx = &shadowCtx;

    // ---- Phase 2a: Kick Worker 0 (shadow) ----------------------------------
    m_renderWorkers[0].Kick([this, shadowCL]() mutable
    {
        if (m_shadowPass)
        {
            uint32_t r = m_gfx.BeginGPUTimestamp(shadowCL, "ShadowPass");
            m_shadowPass->Execute(shadowCL);
            m_gfx.EndGPUTimestamp(shadowCL, r);
        }
    });

    // ---- Phase 2a.5: Cluster lighting CS — must run when lights exist OR when DecalPass needs AABBs.
    // Hoisted out of the if-scope so the DDGI compute-queue CL can declare
    // a cross-queue dependency on it (DDGI's trace CS reads the cluster
    // GPULight buffer that ClusterPass produces).
    RHI::CommandList clusterCL{};
    if (m_clusterPass)
    {
        clusterCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        clusterCL.gfx = &m_gfx;
        uint32_t rc = m_gfx.BeginGPUTimestamp(clusterCL, "ClusterPass");
        m_clusterPass->Execute(clusterCL);

        // Piggyback decal cluster cull on this CL; apply CS in graph reads via CL chain.
        if (m_decalPass)
            m_decalPass->DispatchCull(clusterCL);

        m_gfx.EndGPUTimestamp(clusterCL, rc);
    }

    // GBufferPass indirect-draw state; ExecuteIndirect requires bindless textures.
    if (m_gbufferPass)
    {
        if (m_useIndirectDraw && m_indirectArgBuffer.IsValid() && !m_indirectGroups.empty())
            m_gbufferPass->SetIndirectDraw(&m_indirectArgUpload[frameSlot], m_indirectGroups);
        else
            m_gbufferPass->ClearIndirectDraw();
    }

    // ---- Phase 2a.75: GPU Frustum Culling (optional) -------------------------
    if (m_gpuCullingEnabled && m_cullingPass && m_indirectArgBuffer.IsValid())
    {
        RHI::CommandList cullCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        cullCL.gfx = &m_gfx;
        uint32_t cullRegion = m_gfx.BeginGPUTimestamp(cullCL, "GpuCulling");

        // Set frustum planes from current view
        float frustumData[6][4];
        for (int p = 0; p < 6; ++p)
        {
            frustumData[p][0] = m_view.frustum[p].normal.x;
            frustumData[p][1] = m_view.frustum[p].normal.y;
            frustumData[p][2] = m_view.frustum[p].normal.z;
            frustumData[p][3] = m_view.frustum[p].distance;
        }
        m_cullingPass->SetFrustumPlanes(frustumData);
        m_cullingPass->SetViewProj(m_view.viewProjMatrixNoJitter);
        m_cullingPass->SetInstanceCount(m_indirectDrawCount);

        // Clear draw count to 0 (write 0 to upload staging, copy to default)
        if (m_drawCountMapped[frameSlot])
        {
            *static_cast<uint32_t*>(m_drawCountMapped[frameSlot]) = 0;
            m_gfx.CopyBuffer(m_drawCountUpload[frameSlot], m_drawCountBuffer, sizeof(uint32_t), cullCL);
        }

        m_cullingPass->Execute(cullCL,
            m_instanceBuffer[frameSlot],
            m_meshMgr.GetDescriptorHeap().GetMeshAABBBuffer(),
            m_indirectArgBuffer,
            m_drawCountBuffer);
        m_gfx.EndGPUTimestamp(cullCL, cullRegion);
    }

    // ---- Phase 2b.5: DDGI (TLAS + trace + relight) — dependency of color graph; atlases SRV-ready.
    // Optional ddgiLogTick logs the failing gate at ~1 Hz for offline troubleshooting.
    RHI::CommandList ddgiCL{};
    static uint32_t s_ddgiLogCounter = 0;
    //const bool ddgiLogTick = (++s_ddgiLogCounter % 60u) == 0;
    const bool ddgiLogTick = false;

    uint32_t ddgiRegion = ~0u;
    RHI::CommandList demoteCL{};
    if (m_ddgiReady && m_ddgiPass && m_ddgiMgr.GetActiveVolumeCount() > 0 && m_lastWorld)
    {
        auto& dx12 = static_cast<GraphicsDX12&>(m_gfx);

        // Tiny graphics-queue CL that demotes DDGI atlases + SH/probeData
        // buffers from PIXEL|NON_PIXEL_SHADER_RESOURCE (set by the previous
        // frame's promoteCL, or by CreateBuffer's initial transition on
        // frame 1) → NON_PIXEL_SHADER_RESOURCE only, which is the only SR
        // state a compute CL can transition to/from. Without this step the
        // first compute-queue DDGI barrier with StateBefore=NPSR-only would
        // trip the D3D12 debug layer (recorded state contains PSR=0x80,
        // which is invalid on compute). ddgiCL waits on demoteCL via fence.
        demoteCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        if (demoteCL.IsValid())
        {
            demoteCL.gfx = &m_gfx;
            uint32_t dr = m_gfx.BeginGPUTimestamp(demoteCL, "DDGI.Demote");
            m_ddgiMgr.DemoteForComputeQueue(m_gfx, demoteCL);
            m_gfx.EndGPUTimestamp(demoteCL, dr);
        }

        // DDGI on the COMPUTE queue — probe update is purely compute work
        // (CS dispatches + DXR DispatchRays / RayQuery), so it can run in
        // parallel with the graphics-queue Shadow + GBuffer + SkyIBL +
        // Decal sequence. LightingPass picks up the SH probe buffer + depth
        // atlas via SRV; the cross-queue handoff is done via:
        //   0. demote→DDGI fence (demote graphics CL puts buffers in NPSR-only)
        //   1. cluster→DDGI fence (DDGI's trace reads ClusterPass's lights buf)
        //   2. DDGI→promote fence (promote graphics CL flips atlases NPSR→NPSR|PSR)
        //   3. promote→LightingPass fence (RenderGraph::SetExternalWait)
        ddgiCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::COMPUTE);
        if (ddgiCL.IsValid())
        {
            if (demoteCL.IsValid())
                m_gfx.AddCommandListDependency(ddgiCL, demoteCL);
            // Cluster GPULight buffer is produced on the graphics queue by
            // ClusterPass. DDGI's trace CS samples it at root slot 11 — wait
            // for that write to land before the trace dispatches.
            if (clusterCL.IsValid())
                m_gfx.AddCommandListDependency(ddgiCL, clusterCL);
            ddgiRegion = m_gfx.BeginGPUTimestamp(ddgiCL, "DDGI");
            ID3D12GraphicsCommandList*  base = dx12.GetNativeCommandList(ddgiCL);
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd4;
            if (base && SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&cmd4))))
            {
                // Build / refit DDGI TLAS from current static geometry.
                D3D12_GPU_VIRTUAL_ADDRESS tlasVA =
                    m_ddgiSceneAS.BuildOrRefit(dx12, cmd4.Get(), *m_lastWorld,
                                               m_meshLib, m_meshSys, &m_meshMgr);
                if (ddgiLogTick)
                {
                    LOG_INFO("DDGI: gate-pass volumes=%u tlasVA=0x%llX skyRadSrv=%llu",
                             m_ddgiMgr.GetActiveVolumeCount(),
                             (unsigned long long)tlasVA,
                             (unsigned long long)m_skyRadianceSrvForDDGI);
                }
                if (tlasVA == 0 && ddgiLogTick)
                    LOG_WARNING("DDGI: TLAS build returned 0 — no static geometry visible to DDGI this frame (checked MeshLibRef + procedural-MeshHandle pools).");
                if (tlasVA != 0)
                {
                    // Bind the descriptor heap so DispatchRays can access SRVs/UAVs.
                    dx12.BindDescriptorHeaps(ddgiCL);

                    // GPU VA of g_DDGIInstances buffer (closest-hit reads albedo + bindless slots).
                    D3D12_GPU_VIRTUAL_ADDRESS matVA = 0;
                    if (const RHI::GPUBuffer* mb = m_ddgiSceneAS.GetInstanceBuffer(dx12))
                        if (ID3D12Resource* mr = dx12.GetBufferResource(*mb))
                            matVA = mr->GetGPUVirtualAddress();

                    // Same bindless table as rasterizer; closest-hit reads vb/ibBindless for face normals.
                    const uint64_t bindlessTable =
                        m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle();

                    // DDGIPass manages atlas SRV↔UAV internally; we leave it SRV for downstream.
                    const uint64_t lightsSrv = m_clusterPass
                        ? m_clusterPass->GetLightsSRVHandle() : 0ull;
                    for (uint32_t s = 0; s < DDGI::kMaxVolumes; ++s)
                    {
                        if (m_ddgiMgr.GetProbeCount(s) == 0) continue;

                        m_ddgiPass->Execute(m_gfx, ddgiCL, m_ddgiMgr, s,
                                            tlasVA, m_skyRadianceSrvForDDGI, matVA,
                                            bindlessTable, lightsSrv,
                                            /*onComputeQueue=*/true);

                        // Leave atlases in SRV state on the compute queue.
                        // The promoteCL below (graphics queue) flips them
                        // NPSR-only → NPSR|PSR so LightingPass's PS read sees
                        // them in the full SR mask.
                        m_ddgiMgr.TransitionVolumeAtlases(m_gfx, ddgiCL, s,
                            DDGI::DDGIVolumeManager::AtlasState::SRV,
                            /*onComputeQueue=*/true);
                    }
                }
            }
            m_gfx.EndGPUTimestamp(ddgiCL, ddgiRegion);
        }
    }

    // ---- Phase 2b.6: DDGI atlas promote (graphics queue).
    // DDGI ran on the COMPUTE queue, which leaves the SH probe buffer + depth
    // atlas in NON_PIXEL_SHADER_RESOURCE (the only SR state legal on compute).
    // LightingPass's PS read needs the full PIXEL|NON_PIXEL mask, which can
    // only be issued on a graphics CL. promoteCL waits on the DDGI fence
    // (cross-queue) and emits the per-volume state transitions; LightingPass
    // then waits on promoteCL via SetExternalWait below.
    RHI::CommandList promoteCL{};
    if (ddgiCL.IsValid())
    {
        promoteCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        if (promoteCL.IsValid())
        {
            promoteCL.gfx = &m_gfx;
            m_gfx.AddCommandListDependency(promoteCL, ddgiCL);
            uint32_t pr = m_gfx.BeginGPUTimestamp(promoteCL, "DDGI.Promote");
            m_ddgiMgr.PromoteAtlasesForGraphicsQueue(m_gfx, promoteCL);
            m_gfx.EndGPUTimestamp(promoteCL, pr);

            // LightingPass is the first graph pass that reads the DDGI SH
            // buffer (its PS samples g_DDGIProbeSH at t29). Surgical wait —
            // GBuffer / Terrain / SkyIBL / SpotShadow / Decal run in parallel
            // with the compute-queue DDGI work.
            m_graph.SetExternalWait("LightingPass", promoteCL);
        }
    }

    // ---- Phase 2b: Main-thread color passes (concurrent with Worker 0).
    RHI::CommandList colorLastCL = m_graph.Execute(m_gfx, m_clearColor);

    if (!ddgiCL.IsValid() && ddgiLogTick)
    {
        // Identify which gate tripped; throttled to ~1 Hz by ddgiLogTick.
        if (!m_ddgiReady)
            LOG_INFO("DDGI: skipped — m_ddgiReady=false (DXR unsupported or pass init failed)");
        else if (!m_ddgiPass)
            LOG_INFO("DDGI: skipped — m_ddgiPass null");
        else if (m_ddgiMgr.GetActiveVolumeCount() == 0)
            LOG_INFO("DDGI: skipped — 0 active volumes (add a DDGIVolumeComponent to an entity)");
        else if (!m_lastWorld)
            LOG_INFO("DDGI: skipped — m_lastWorld null (BeginFrame not called yet?)");
    }

    // ---- Phase 3: Wait for Worker 0 ----------------------------------------
    m_renderWorkers[0].doneSem.acquire();

    // ---- Phase 4: Wire shadow → color GPU dependency -----------------------
    if (colorLastCL.IsValid() && shadowCL.IsValid())
        m_gfx.AddCommandListDependency(colorLastCL, shadowCL);

    // ---- Phase 4.2: Pop one probe off the bake queue; no-op when steady-state.
    ProcessProbeBakeQueue(colorLastCL);

    // ---- Phase 4.5: Hi-Z mip chain generation (for next frame's occlusion culling) ---
    if (m_gpuCullingEnabled && m_hiZPass)
    {
        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        if (depthTex)
        {
            m_hiZPass->EnsureTexture(m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());

            RHI::CommandList hizCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
            hizCL.gfx = &m_gfx;
            uint32_t hizRegion = m_gfx.BeginGPUTimestamp(hizCL, "HiZ");

            // Transition depth using graph-tracked state (not hardcoded).
            RHI::ResourceState depthState = m_graph.GetTextureState(m_depthHandle);
            m_gfx.PushBarrier(
                RHI::GPUBarrier::Image(depthTex,
                    depthState,
                    RHI::ResourceState::SHADER_RESOURCE), hizCL);

            uint64_t depthSrv = m_gfx.GetTextureSRVGpuHandle(*depthTex);
            m_hiZPass->Execute(hizCL, depthSrv);

            // Transition back and update graph's state tracking.
            m_gfx.PushBarrier(
                RHI::GPUBarrier::Image(depthTex,
                    RHI::ResourceState::SHADER_RESOURCE,
                    RHI::ResourceState::DEPTHSTENCIL), hizCL);
            m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::DEPTHSTENCIL);

            m_gfx.EndGPUTimestamp(hizCL, hizRegion);

            if (colorLastCL.IsValid())
                m_gfx.AddCommandListDependency(hizCL, colorLastCL);
        }
    }

    // ---- Phase 4.6 + 4.7: Hi-Z SSR chain.
    // Trace / resolve / temporal / upsample / composite are all driven by
    // SSRSubsystem. Renderer is responsible only for handing it the GBuffer
    // handles + jittered camera state; barrier dance + per-pass barriers live
    // inside the subsystem.
    if (m_ssrEnabled && m_ssrSubsystem && m_viewMode != ViewMode::Wireframe)
    {
        SSRSubsystem::FrameContext ctx{};
        ctx.graph                = &m_graph;
        ctx.albedoHandle         = m_albedoHandle;
        ctx.normalHandle         = m_normalHandle;
        ctx.surfaceHandle        = m_surfaceHandle;
        ctx.depthHandle          = m_depthHandle;
        ctx.velocityHandle       = m_velocityHandle;
        ctx.viewProj             = m_view.viewProjMatrix;
        ctx.prevViewProjJittered = m_taaJitter.GetPrevViewProjJittered();
        ctx.cameraPos            = m_camera.position;
        ctx.nearZ                = m_camera.nearZ;
        ctx.farZ                 = m_camera.farZ;
        ctx.brdfLutSrv           = m_brdfLutSrv;
        m_ssrSubsystem->Render(m_gfx, ctx, colorLastCL);
    }

    // ---- Phase 4.8: Debug wireframe — MUST run after SSR composite or it gets stomped.
    if (m_debugWirePass && m_debugWirePass->enabled && colorLastCL.IsValid())
    {
        RHI::CommandList wireCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        wireCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(wireCL, colorLastCL);

        uint32_t wireRegion = m_gfx.BeginGPUTimestamp(wireCL, "DebugWire");
        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        m_debugWirePass->Execute(wireCL, depthTex, m_perObjectCB.CurrentBuffer(m_gfx));
        m_gfx.EndGPUTimestamp(wireCL, wireRegion);

        colorLastCL = wireCL;
    }

    // DDGI probe debug spheres: PS samples irradiance atlas via octahedral mapping.
    // Only runs when at least one DDGIVolumeComponent has debugDraw=true.
    if (m_ddgiProbeDebugPass && m_ddgiProbeDebugPass->enabled
        && colorLastCL.IsValid() && m_lastWorld)
    {
        RHI::CommandList dbgCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        dbgCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(dbgCL, colorLastCL);

        uint32_t dbgRegion = m_gfx.BeginGPUTimestamp(dbgCL, "DDGIProbeDebug");
        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        m_ddgiProbeDebugPass->Execute(m_gfx, dbgCL, depthTex,
                                      m_perObjectCB.CurrentBuffer(m_gfx), m_ddgiMgr, *m_lastWorld);
        m_gfx.EndGPUTimestamp(dbgCL, dbgRegion);

        colorLastCL = dbgCL;
    }

    // ---- Phase 5: Post-processing (preComputeCL → computeCL → restoreCL) ---
    if (m_taaPass || m_autoExposurePass || m_bloomPass || m_toneMapPass || m_xegtaoPass)
    {
        // preComputeCL (GRAPHICS): transition HDR + depth for compute reads.
        RHI::CommandList preComputeCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        preComputeCL.gfx = &m_gfx;
        if (colorLastCL.IsValid())
            m_gfx.AddCommandListDependency(preComputeCL, colorLastCL);

        // Rebuild ToneMap texture on main thread first; Worker 2 then sees no size change.
        if (m_toneMapPass) m_toneMapPass->EnsureTexture(m_vpWidth, m_vpHeight);

        m_gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE_COMPUTE, preComputeCL);
        if (m_toneMapPass) m_toneMapPass->PrepareForCompute(preComputeCL);
        if (taaDepthTex)
        {
            RHI::ResourceState curDepthState = m_graph.GetTextureState(m_depthHandle);
            m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                taaDepthTex,
                curDepthState,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE), preComputeCL);
            m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        }
        // Transition normal buffer for XeGTAO compute read.
        if (normalTex && m_xegtaoPass)
        {
            RHI::ResourceState curNormalState = m_graph.GetTextureState(m_normalHandle);
            if (curNormalState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
            {
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    normalTex,
                    curNormalState,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), preComputeCL);
                m_graph.SetTextureState(m_normalHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            }
        }
        // Transition velocity for TAA / XeGTAO compute reads. Velocity now
        // carries an SRV (GBuffer3_Velocity isSRV), but the graph leaves it in
        // RENDERTARGET after GBufferPass (and SSR's fromSR restores it there),
        // so it must be made compute-readable here — mirrors the normal buffer.
        if (taaVelocityTex && (m_taaPass || m_xegtaoPass))
        {
            RHI::ResourceState curVelState = m_graph.GetTextureState(m_velocityHandle);
            if (curVelState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
            {
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    taaVelocityTex,
                    curVelState,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), preComputeCL);
                m_graph.SetTextureState(m_velocityHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            }
        }
        // computeCL (COMPUTE): TAA → AutoExposure → Bloom → ToneMap; cross-queue fenced.
        RHI::CommandList computeCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::COMPUTE);
        computeCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(computeCL, preComputeCL);

        // Worker 2 records the compute dispatches while main thread can do other work.
        m_renderWorkers[2].Kick([this, computeCL, depthSrv, surfaceSrv, velocitySrv, normalSrv, stencilSrv]() mutable
        {
            if (m_taaPass)
            {
                XMFLOAT4X4 prevVP;
                XMStoreFloat4x4(&prevVP,
                    XMMatrixTranspose(XMLoadFloat4x4(&m_taaJitter.GetPrevViewProjNoJitter())));

                m_taaPass->SetHdrSrvHandle(m_gfx.GetHdrSceneSrvGpuHandle());
                m_taaPass->SetDepthSrvHandle(depthSrv);
                m_taaPass->SetGBufferSrvHandle(surfaceSrv);
                m_taaPass->SetVelocitySrvHandle(velocitySrv);
                m_taaPass->SetOutlineStencilSrvHandle(stencilSrv);
                m_taaPass->SetViewportSize(m_vpWidth, m_vpHeight);
                m_taaPass->SetFrameData(m_taaJitter.GetInvViewProj(), prevVP,
                                        m_taaPass->historyWeight,
                                        /*hasHistory=*/true, m_deltaTime);
                m_taaPass->SetJitter(m_taaJitter.GetJitterX(), m_taaJitter.GetJitterY());
                uint32_t r = m_gfx.BeginGPUTimestamp(computeCL, "TAA");
                m_taaPass->Execute(computeCL);
                m_gfx.EndGPUTimestamp(computeCL, r);
                m_taaJitter.AdvanceToNextFrame();
            }

            // XeGTAO: compute screen-space AO (result used next frame by LightingPass).
            if (m_xegtaoPass && m_ssaoEnabled && depthSrv)
            {
                m_xegtaoPass->SetDepthSrvHandle(depthSrv);
                m_xegtaoPass->SetNormalSrvHandle(normalSrv);
                // Velocity feeds AO-only temporal reprojection; falls back to "no history" if 0.
                m_xegtaoPass->SetVelocitySrvHandle(velocitySrv);
                m_xegtaoPass->SetViewportSize(m_vpWidth, m_vpHeight);
                // Pass un-jittered projection so TAA jitter doesn't drift reconstruction.
                static uint32_t s_gtaoFrame = 0;
                m_xegtaoPass->SetProjectionMatrix(
                    m_view.projMatrixNoJitter, m_view.viewMatrix, s_gtaoFrame++);
                uint32_t r = m_gfx.BeginGPUTimestamp(computeCL, "XeGTAO");
                m_xegtaoPass->Execute(computeCL);
                m_gfx.EndGPUTimestamp(computeCL, r);
            }

            // FXAA pass: spatial AA. Reads either TAA's resolved buffer (if TAA also
            // active → FXAA+TAA mode) or the raw HDR scene (FXAA-only mode). Output
            // becomes the resolvedSrv for downstream Bloom / AutoExposure / ToneMap.
            if (m_fxaaPass && m_fxaaPass->IsEnabled())
            {
                const uint64_t fxaaInputSrv = (m_taaPass && m_taaPass->IsEnabled())
                    ? m_taaPass->GetResolvedSrvHandle()
                    : m_gfx.GetHdrSceneSrvGpuHandle();
                m_fxaaPass->SetInputSrvHandle(fxaaInputSrv);
                m_fxaaPass->SetViewportSize(m_vpWidth, m_vpHeight);
                uint32_t r = m_gfx.BeginGPUTimestamp(computeCL, "FXAA");
                m_fxaaPass->Execute(computeCL);
                m_gfx.EndGPUTimestamp(computeCL, r);
            }

            // Route resolvedSrv to FXAA > TAA > raw HDR (priority order).
            uint64_t resolvedSrv = m_gfx.GetHdrSceneSrvGpuHandle();
            if (m_taaPass && m_taaPass->IsEnabled())
                resolvedSrv = m_taaPass->GetResolvedSrvHandle();
            if (m_fxaaPass && m_fxaaPass->IsEnabled())
                resolvedSrv = m_fxaaPass->GetResolvedSrvHandle();

            // ---- Lens flare per-frame setup (CPU project sun → screen UV) ----
            // Reads LightCB.lightDir (= -sunDirWS) and lightColor written earlier
            // this frame, projects a far point in the sun direction with the
            // un-jittered view-proj, and feeds the result to the pass before
            // the post-process stack runs (the stack invokes LensFlareEffect
            // between Bloom and Tonemapping).
            if (m_lensFlarePass && depthSrv)
            {
                using namespace DirectX;
                XMFLOAT3 sunDirWS{ 0.f, 1.f, 0.f };
                XMFLOAT3 sunColor{ 1.f, 1.f, 1.f };
                if (m_lightCB.Current(m_gfx))
                {
                    auto* lb = m_lightCB.Current(m_gfx);
                    sunDirWS = { -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
                    sunColor = {  lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
                }

                const XMFLOAT3& fwd = m_view.cameraForward;
                const float dotFwd  = sunDirWS.x * fwd.x + sunDirWS.y * fwd.y
                                    + sunDirWS.z * fwd.z;
                bool sunBehind = (dotFwd <= 0.05f);

                XMFLOAT2 sunUV{ 0.5f, 0.5f };
                if (!sunBehind)
                {
                    const XMFLOAT3& cp = m_view.cameraPosition;
                    XMVECTOR wp = XMVectorSet(
                        cp.x + sunDirWS.x * 1.0e5f,
                        cp.y + sunDirWS.y * 1.0e5f,
                        cp.z + sunDirWS.z * 1.0e5f,
                        1.0f);
                    XMMATRIX vp = XMLoadFloat4x4(&m_view.viewProjMatrixNoJitter);
                    XMVECTOR clip = XMVector4Transform(wp, vp);
                    XMFLOAT4 c; XMStoreFloat4(&c, clip);
                    if (c.w > 1.0e-4f)
                    {
                        sunUV.x =  c.x / c.w * 0.5f + 0.5f;
                        sunUV.y = -c.y / c.w * 0.5f + 0.5f;
                    }
                    else
                    {
                        sunBehind = true;
                    }
                }

                // Disable when the sun is below the horizon. Two paths:
                //   - TOD active and TODOutput flagged moon active → LightCB
                //     tracks the moon (moonDir.y > 0 at night), so the dir.y
                //     check below would *not* fire. Read TODOutput to catch.
                //   - TOD off → directional light is whatever the user
                //     authored. Gate on sunDirWS.y so a manually down-pointing
                //     sun also turns flare off.
                bool sunBelowHorizon = false;
                if (m_lastWorld) {
                    if (const auto* todOut = TODUtil::FindOutput(*m_lastWorld);
                        todOut && todOut->isMoonActive)
                        sunBelowHorizon = true;
                }
                if (sunDirWS.y < 0.02f)   // ~1° above horizon
                    sunBelowHorizon = true;

                m_lensFlarePass->SetEnabled(!sunBelowHorizon);
                m_lensFlarePass->SetSun(sunUV, sunBehind, sunColor);
                m_lensFlarePass->SetDepthSrvHandle(depthSrv);
                m_lensFlarePass->SetDepthSourceSize(m_vpWidth, m_vpHeight);
                m_lensFlarePass->SetViewportSize(m_vpWidth, m_vpHeight);
            }

            // Stack drives CAS → AutoExposure → Bloom → LensFlare → Tonemap.
            if (m_postProcessStack)
            {
                PostProcess::Context ppCtx{};
                ppCtx.cl             = computeCL;
                ppCtx.gfx            = &m_gfx;
                ppCtx.viewportWidth  = m_vpWidth;
                ppCtx.viewportHeight = m_vpHeight;
                ppCtx.deltaTime      = m_deltaTime;
                ppCtx.cameraPos      = m_view.cameraPosition;
                ppCtx.world          = m_lastWorld;   // for EntityVolumeSource
                ppCtx.hdrSrv         = resolvedSrv;
                uint32_t r = m_gfx.BeginGPUTimestamp(computeCL, "PostProcessStack");
                m_postProcessStack->Execute(ppCtx);
                m_gfx.EndGPUTimestamp(computeCL, r);
            }

            // GlassShatterPass: composites shards in-place into Tonemap output; restores entry state.
            if (m_glassShatterPass && m_glassShatterPass->IsActive() && m_toneMapPass)
            {
                uint32_t r = m_gfx.BeginGPUTimestamp(computeCL, "GlassShatter");
                m_glassShatterPass->Execute(
                    computeCL,
                    m_toneMapPass->GetFinalOutputTexture(),
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                    m_vpWidth, m_vpHeight,
                    m_deltaTime);
                m_gfx.EndGPUTimestamp(computeCL, r);
            }
        });

        // ---- Phase 6: Wait for Worker 2 ------------------------------------
        m_renderWorkers[2].doneSem.acquire();

        // ---- Phase 7: restoreCL (GRAPHICS) — depth + display transitions; cross-queue fenced.
        RHI::CommandList restoreCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        restoreCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(restoreCL, computeCL);

        if (taaDepthTex)
        {
            RHI::ResourceState curState = m_graph.GetTextureState(m_depthHandle);
            m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                taaDepthTex,
                curState,
                RHI::ResourceState::DEPTHSTENCIL), restoreCL);
            m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::DEPTHSTENCIL);
        }
        // Restore normal buffer state for next frame's GBuffer pass.
        if (normalTex && m_xegtaoPass)
        {
            RHI::ResourceState curNormalState = m_graph.GetTextureState(m_normalHandle);
            if (curNormalState != RHI::ResourceState::RENDERTARGET)
            {
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    normalTex,
                    curNormalState,
                    RHI::ResourceState::RENDERTARGET), restoreCL);
                m_graph.SetTextureState(m_normalHandle, RHI::ResourceState::RENDERTARGET);
            }
        }
        if (m_toneMapPass) m_toneMapPass->TransitionForDisplay(restoreCL);

        // HDR editor viewport: post-process Phase 5 left it in SHADER_RESOURCE_COMPUTE
        // (NPSR-only, set at line ~1929 for compute reads). The next frame's first
        // RG color pass calls SetRenderTargetToHdr which lazily transitions back
        // to RT, but with multi-threaded CL recording the tracker can desync from
        // actual GPU state — leading to "ClearRTV on resource in NPSR" validator
        // errors against `GraphicsDX12.HdrEditorViewport`. Restore explicitly here
        // so every frame ends with HDR known to be in RT state. Idempotent: the
        // helper no-ops when the tracker already says RT.
        m_gfx.SetHdrTextureState(RHI::ResourceState::RENDERTARGET, restoreCL);

        // UI overlay into LDR tonemap output (SR→RT→SR internally). WorldUI billboards first.
        if (m_worldUIPass && m_worldUIPass->enabled && m_toneMapPass && m_lastWorld)
        {
            const RHI::ResourceState entry = m_toneMapPass->GetFinalOutputState();
            uint32_t r = m_gfx.BeginGPUTimestamp(restoreCL, "WorldUI");
            m_worldUIPass->Execute(restoreCL,
                                    *m_lastWorld,
                                    m_view.viewProjMatrixNoJitter,
                                    m_view.viewMatrix,
                                    m_toneMapPass->GetFinalOutputTexture(),
                                    entry,
                                    m_vpWidth, m_vpHeight);
            m_gfx.EndGPUTimestamp(restoreCL, r);
            m_toneMapPass->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
        }

        if (m_uiPass && m_uiPass->enabled && m_toneMapPass
            && !m_uiPass->GetDrawList().IsEmpty())
        {
            // Pass actual tracked state so the barrier matches (resize paths can leave UAV/SR_COMPUTE).
            const RHI::ResourceState entry = m_toneMapPass->GetFinalOutputState();
            uint32_t r = m_gfx.BeginGPUTimestamp(restoreCL, "UI");
            m_uiPass->Execute(restoreCL,
                              m_toneMapPass->GetFinalOutputTexture(),
                              entry,
                              m_vpWidth, m_vpHeight);
            m_gfx.EndGPUTimestamp(restoreCL, r);
            m_toneMapPass->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
        }
        // Always clear drawlist — widgets re-emit each frame; prevents disabled-UI accumulation.
        if (m_uiPass) m_uiPass->GetDrawList().Clear();

        return restoreCL;
    }

    return colorLastCL.IsValid() ? colorLastCL : shadowCL;
}

void Renderer::InitMeshManager()
{
    m_meshMgr.Init(m_gfx);
    m_meshMgr.InitPrimitives();
    // Billboard quad deferred to InitSkinningSystems — same lifetime as its pass.
}

// ---------------------------------------------------------------------------
const ShaderReflect::Reflection*
Renderer::GetCustomShaderReflection(int customShaderID) const
{
    if (customShaderID <= 0 || !m_gbufferPass) return nullptr;
    return m_gbufferPass->GetShaderLibrary()
        .GetDynamicReflection(static_cast<uint32_t>(customShaderID));
}

std::string
Renderer::GetCustomShaderError(int customShaderID) const
{
    if (customShaderID <= 0 || !m_gbufferPass) return {};
    return m_gbufferPass->GetShaderLibrary()
        .GetDynamicCompileError(static_cast<uint32_t>(customShaderID));
}

bool Renderer::CaptureViewportToPNG(const char* path)
{
    if (!m_toneMapPass) return false;
    const RHI::Texture* tex = m_toneMapPass->GetFinalOutputTexture();
    if (!tex || !tex->IsValid()) return false;
    // Tonemap sits in SHADER_RESOURCE post-composite; capture roundtrips through COPY_SOURCE.
    return m_gfx.CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, path);
}

// ---------------------------------------------------------------------------
void Renderer::UploadFrameData(FrameIndex /*frame*/, uint32_t vpW, uint32_t vpH)
{
    if (vpW == 0 || vpH == 0)  return;
    auto* pvCB = m_perObjectCB.Current(m_gfx);
    if (!pvCB)                  return;

    m_vpWidth  = vpW;
    m_vpHeight = vpH;

    // Build view from current camera pose (position + forward come from the
    // camera entity's GlobalTransform — see App::Run / Renderer::SetCamera).
    const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&m_camera.forward));
    const XMVECTOR pos     = XMLoadFloat3(&m_camera.position);
    const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    const float aspect = static_cast<float>(vpW) / static_cast<float>(vpH);
    XMMATRIX view    = XMMatrixLookToLH(pos, forward, up);
    // Reversed-Z: near/far swapped (NDC z=1 near, 0 far) + GREATER_EQUAL test → packs D32 precision at far.
    XMMATRIX projBase = XMMatrixPerspectiveFovLH(m_camera.fov, aspect, m_camera.farZ, m_camera.nearZ);

    // Save unjittered VP for TAA reprojection (becomes prevViewProj next frame).
    XMMATRIX viewProjNoJitter = XMMatrixMultiply(view, projBase);

    // Halton subpixel jitter (±0.5 px, 16 positions → 1px box filter). Skipped when TAA off.
    const bool taaEnabled = (m_taaPass && m_taaPass->IsEnabled());
    m_taaJitter.Advance(taaEnabled);
    XMMATRIX proj             = m_taaJitter.ApplyJitterToProjection(projBase, vpW, vpH);
    XMMATRIX viewProjJittered = XMMatrixMultiply(view, proj);

    PerViewCB cb{};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                    XMMatrixTranspose(viewProjJittered));
    // Prev unjittered VP (transposed for row-vector HLSL).
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
                    XMMatrixTranspose(XMLoadFloat4x4(&m_taaJitter.GetPrevViewProjNoJitter())));
    // Current UNJITTERED VP for velocity (real motion only, not TAA jitter).
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.curViewProjNoJitter),
                    XMMatrixTranspose(viewProjNoJitter));
    *pvCB = cb;

    XMStoreFloat4x4(&m_view.viewMatrix,              view);
    XMStoreFloat4x4(&m_view.projMatrix,              proj);              // jittered
    XMStoreFloat4x4(&m_view.viewProjMatrix,          viewProjJittered);  // jittered
    XMStoreFloat4x4(&m_view.projMatrixNoJitter,      projBase);          // un-jittered (for ImGuizmo)
    XMStoreFloat4x4(&m_view.viewProjMatrixNoJitter,  viewProjNoJitter);  // un-jittered (for ImGuizmo)
    XMStoreFloat3(&m_view.cameraPosition,   pos);
    XMStoreFloat3(&m_view.cameraForward,    forward);
    m_view.nearZ = m_camera.nearZ;
    m_view.farZ  = m_camera.farZ;

    // BoundingFrustum extraction needs standard-Z (CreateFromMatrix hard-codes near→0, far→1).
    // Render projection is reversed-Z; build a std-Z copy here only for the extractor.
    {
        const float aspect = static_cast<float>(vpW) / static_cast<float>(vpH);
        XMMATRIX stdProj = XMMatrixPerspectiveFovLH(
            m_camera.fov, aspect, m_camera.nearZ, m_camera.farZ);
        DirectX::BoundingFrustum::CreateFromMatrix(m_view.boundingFrustum, stdProj);

        // Transform from view-space to world-space.
        XMMATRIX invView = XMMatrixInverse(nullptr, view);
        m_view.boundingFrustum.Transform(m_view.boundingFrustum, invView);

        // Inflate slightly to avoid edge-case culling of objects at frustum boundary.
        m_view.boundingFrustum.Near   *= 0.9f;
        m_view.boundingFrustum.Far    *= 1.1f;
        m_view.boundingFrustum.TopSlope    *= 1.05f;
        m_view.boundingFrustum.BottomSlope *= 1.05f;
        m_view.boundingFrustum.LeftSlope   *= 1.05f;
        m_view.boundingFrustum.RightSlope  *= 1.05f;
    }

    // Legacy plane extraction (kept for reference).
    m_view.frustum = ExtractFrustumPlanes(m_view.viewProjMatrixNoJitter);

    // Update outline pass with current near/far for depth linearization
    if (m_outlinePass)
    {
        m_outlinePass->nearZ = m_camera.nearZ;
        m_outlinePass->farZ  = m_camera.farZ;
    }

    // LightCB camera + invViewProj (transposed for HLSL row-vector mul); cache for TAACB.
    if (m_lightCB.Current(m_gfx))
    {
        auto* lb = m_lightCB.Current(m_gfx);
        lb->cameraPos[0] = m_camera.position.x;
        lb->cameraPos[1] = m_camera.position.y;
        lb->cameraPos[2] = m_camera.position.z;
        lb->viewMode     = (uint32_t)m_viewMode;   // global Lit/Unlit/Wireframe switch

        XMMATRIX invVP = XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjJittered));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->invViewProj), invVP);

        m_taaJitter.CommitFrame(viewProjNoJitter, viewProjJittered, invVP);
    }

    // ---- Compute cascade shadow matrices ------------------------------------
    if (m_shadowSystem && m_lightCB.Current(m_gfx))
    {
        auto* lb = m_lightCB.Current(m_gfx);

        ShadowSystem::FrameInput fi{};
        fi.view           = view;
        fi.cameraForward  = m_view.cameraForward;
        fi.lightDir       = { lb->lightDir[0], lb->lightDir[1], lb->lightDir[2] };
        fi.nearZ          = m_camera.nearZ;
        fi.farZ           = m_camera.farZ;
        fi.fov            = m_camera.fov;
        fi.aspect         = (m_vpHeight > 0)
                            ? static_cast<float>(m_vpWidth) / static_cast<float>(m_vpHeight)
                            : 1.0f;
        fi.shadowMapSize  = ShadowPass::kShadowMapSize;

        m_shadowSystem->Update(fi);

        if (m_shadowSystem->HasValidLight())
        {
            for (int i = 0; i < ShadowSystem::kCascadeCount; ++i)
            {
                std::memcpy(lb->shadowMatrix[i],
                            &m_shadowSystem->CascadeMatricesTransposed()[i],
                            sizeof(DirectX::XMFLOAT4X4));
                lb->cascadeSplits[i] = m_shadowSystem->CascadeSplits()[i];
            }
            lb->shadowBias         = ShadowSystem::ShadowBias();
            lb->shadowStrength     = ShadowSystem::ShadowStrength();
            lb->cameraForward[0]   = m_view.cameraForward.x;
            lb->cameraForward[1]   = m_view.cameraForward.y;
            lb->cameraForward[2]   = m_view.cameraForward.z;
            lb->shadowMapTexelSize = 1.0f / static_cast<float>(ShadowPass::kShadowMapSize);
            lb->shadowBlendRange   = m_shadowSystem->ShadowBlendRange();
            lb->shadowFrameIndex   = m_shadowSystem->ShadowFrameIndex();

            const float* texelWorld = m_shadowSystem->CascadeTexelWorldSize();
            for (int i = 0; i < ShadowSystem::kCascadeCount; ++i)
                lb->cascadeTexelWorldSize[i] = texelWorld[i];
            lb->shadowNormalOffset = ShadowSystem::ShadowNormalOffset();
        }
    }
}

// ---------------------------------------------------------------------------
void Renderer::CreateConstantBuffers()
{
    // ---- PerViewCB (triple-buffered) ----------------------------------------
    if (!m_perObjectCB.Create(m_gfx, "Renderer.PerViewCB"))
    {
        LOG_ERROR("Renderer: PerViewCB creation failed");
        return;
    }
    {
        m_vpWidth  = static_cast<uint32_t>(m_gfx.GetWidth());
        m_vpHeight = static_cast<uint32_t>(m_gfx.GetHeight());
        UploadFrameData(0, m_vpWidth, m_vpHeight);
    }

    // ---- TerrainParams CB (triple-buffered) --------------------------------
    if (!m_terrainCB.Create(m_gfx, "Renderer.TerrainCB"))
        LOG_ERROR("Renderer: TerrainCB creation failed");

    // ---- InstanceBuffer (GPUInstanceData per instance, UPLOAD heap, triple-buffered)
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxInstances) * sizeof(GPUInstanceData);
        desc.stride     = sizeof(GPUInstanceData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (!m_gfx.CreateBuffer(desc, m_instanceBuffer[i]))
            {
                LOG_ERROR("Renderer: InstanceBuffer[%u] creation failed", i);
                return;
            }
            m_instanceBufferMapped[i] = m_gfx.MapBuffer(m_instanceBuffer[i]);
        }
    }

    // ---- ExecuteIndirect buffers --------------------------------------------
    {
        const uint64_t argSize = static_cast<uint64_t>(kMaxInstances) * sizeof(IndirectDrawCommand);

        // DEFAULT heap: GPU-side indirect arg buffer (single — GPU-only, no CPU race)
        RHI::GPUBufferDesc desc;
        desc.size       = argSize;
        desc.stride     = sizeof(IndirectDrawCommand);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bind_flags = RHI::BindFlag::UNORDERED_ACCESS; // GPU culling will write here
        m_gfx.CreateBuffer(desc, m_indirectArgBuffer);

        // UPLOAD heap: CPU staging (triple-buffered)
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_indirectArgUpload[i]))
                m_indirectArgMapped[i] = m_gfx.MapBuffer(m_indirectArgUpload[i]);
        }

        // Draw count buffer (4 bytes, DEFAULT — GPU-only)
        desc.size       = sizeof(uint32_t);
        desc.stride     = sizeof(uint32_t);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bind_flags = RHI::BindFlag::UNORDERED_ACCESS;
        m_gfx.CreateBuffer(desc, m_drawCountBuffer);

        // Draw count staging (UPLOAD, triple-buffered)
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_drawCountUpload[i]))
                m_drawCountMapped[i] = m_gfx.MapBuffer(m_drawCountUpload[i]);
        }

        LOG_INFO("Renderer: ExecuteIndirect buffers ready (max %u commands)", kMaxInstances);
    }

    // ---- LightCB (triple-buffered) ------------------------------------------
    if (!m_lightCB.Create(m_gfx, "Renderer.LightCB"))
    {
        LOG_ERROR("Renderer: LightCB creation failed");
        return;
    }
    {
        auto norm3 = [](float x, float y, float z, float o[3]) {
            float l = std::sqrt(x*x + y*y + z*z);
            o[0] = x/l; o[1] = y/l; o[2] = z/l;
        };
        LightCB lb{};
        norm3(1.f, -2.f, 0.5f, lb.lightDir);
        lb.lightColor[0] = 1.f;  lb.lightColor[1] = 0.92f; lb.lightColor[2] = 0.82f;
        lb.cameraPos[0]  = m_camera.position.x;
        lb.cameraPos[1]  = m_camera.position.y;
        lb.cameraPos[2]  = m_camera.position.z;
        // Seed every slot so the first kFrameSlots frames don't read junk before
        // BuildScene_UploadLights has overwritten the slot the GPU is reading.
        for (uint32_t i = 0; i < kFrameSlots; ++i)
            if (m_lightCB.mapped[i]) *m_lightCB.mapped[i] = lb;
    }

    // ---- SpotShadow VP matrix buffer (triple-buffered) ----------------------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(SpotShadowPass::kMaxCasters)
                        * sizeof(DirectX::XMFLOAT4X4);
        desc.stride     = sizeof(DirectX::XMFLOAT4X4);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        DirectX::XMFLOAT4X4 ident;
        DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_spotShadowVPBuffer[i]))
            {
                m_spotShadowVPMapped[i] = m_gfx.MapBuffer(m_spotShadowVPBuffer[i]);
                m_spotShadowVPSrv[i]    = m_gfx.GetBufferSRVGpuHandle(m_spotShadowVPBuffer[i]);
                if (m_spotShadowVPMapped[i])
                {
                    auto* dst = static_cast<DirectX::XMFLOAT4X4*>(m_spotShadowVPMapped[i]);
                    for (uint32_t k = 0; k < SpotShadowPass::kMaxCasters; ++k)
                        dst[k] = ident;
                }
            }
        }
    }

    // ---- MaterialBuffer (StructuredBuffer<MaterialGPUData>, UPLOAD heap) ----
    // Triple-buffered: matBuf is rewritten from scratch every frame in
    // BuildDrawListAndUploadInstances (matIdx resets to 0 each frame). Without
    // ringing it, CPU writes for frame N+1 race GPU reads for frame N → flicker
    // in motion.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxMaterials) * sizeof(Resource::MaterialGPUData);
        desc.stride     = sizeof(Resource::MaterialGPUData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t s = 0; s < kFrameSlots; ++s)
        {
            if (!m_gfx.CreateBuffer(desc, m_materialBuffer[s]))
            {
                LOG_ERROR("Renderer: MaterialBuffer slot %u creation failed", s);
                return;
            }
            m_materialBufferMapped[s] = m_gfx.MapBuffer(m_materialBuffer[s]);

            // Pre-fill EVERY slot with default PBR values so any of the first
            // kFrameSlots frames reads valid data even before the first scene
            // gather has written its visible materials.
            if (m_materialBufferMapped[s])
            {
                auto* data = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped[s]);
                for (uint32_t i = 0; i < kMaxMaterials; ++i)
                {
                    data[i] = {};
                    data[i].roughnessMin = 0.0f;  data[i].roughnessMax = 0.5f;
                    data[i].metalnessMin = 0.0f;  data[i].metalnessMax = 0.0f;
                    data[i].reflectance  = 0.5f;  // F0 = 0.16 * 0.5² = 0.04
                    data[i].baseColor[0] = data[i].baseColor[1] =
                    data[i].baseColor[2] = data[i].baseColor[3] = 1.0f;
                    for (auto& id : data[i].textureHandleIds) id = -1;
                }
            }
        }
    }

    // Reflection probe pool — non-fatal on partial init.
    m_probeMgr.Init(m_gfx);

    // DDGI: per-volume resources lazy via BuildScene_UpdateDDGI; needs DXR for the trace pass.
    // No DXR → skip pass init entirely; LightingPass's ddgiVolumeCount==0 fallback handles it.
    if (m_ddgiMgr.Init(m_gfx))
    {
        auto& dx12 = static_cast<GraphicsDX12&>(m_gfx);
        if (dx12.SupportsDXR())
        {
            m_ddgiPass = std::make_unique<DDGIPass>();
            m_ddgiReady = m_ddgiPass->Init(m_gfx);
            if (!m_ddgiReady)
            {
                m_ddgiPass.reset();
                LOG_INFO("Renderer: DDGI pass init failed — DDGI disabled");
            }
        }
        else
        {
            LOG_INFO("Renderer: DXR not supported — DDGI disabled (Sky IBL fallback only)");
        }

        // Probe debug viz works without DXR; spheres render dim but visible (+0.02 ambient floor).
        m_ddgiProbeDebugPass = std::make_unique<DDGIProbeDebugPass>();
        if (!m_ddgiProbeDebugPass->Init(m_gfx))
        {
            m_ddgiProbeDebugPass.reset();
            LOG_INFO("Renderer: DDGI probe debug pass init failed");
        }
    }
}


// ---------------------------------------------------------------------------
void Renderer::RequestPick(float pixelX, float pixelY)
{
    if (m_pickingPass)
        m_pickingPass->RequestPick(static_cast<int>(pixelX), static_cast<int>(pixelY));
}

bool Renderer::ResolvePick(Entity& outEntity)
{
    if (!m_pickingPass || !m_pickingPass->IsPickPending()) return false;
    m_gfx.FlushAndWait();
    const uint32_t slotValue = m_pickingPass->ReadPickResult();
    m_pickingPass->ClearPickPending();
    if (slotValue == 0)
    {
        outEntity = NullEntity;
        return true;
    }
    const uint32_t slot = slotValue - 1;
    outEntity = (slot < kMaxInstances) ? m_instanceSlotToEntity[slot] : NullEntity;
    return true;
}


// ---------------------------------------------------------------------------
// Skinning system initialisation — called once from Compile()
// ---------------------------------------------------------------------------
void Renderer::InitSkinningSystems()
{
    // Pose/vertex rings, ECS systems, SkinningPass — all owned by m_skin.
    m_skin.Init(m_gfx, m_meshMgr);

    // Phase E/F per-material rings — first init hook that runs after CBV/SRV heaps are ready.
    m_customMatCbvRing.Init(m_gfx);
    m_customMatSrvRing.Init(m_gfx);

    // ---- Particle system (GPU pool, emit + update compute, billboard render) --
    m_particleSystem = std::make_unique<ParticleSystem>();
    m_particleSystem->Init(m_gfx);

    m_particleSystem->SetResourceSystems(m_texSys, m_resMgr);

    m_particleSimPass = std::make_unique<ParticleSimPass>();
    m_particleSimPass->Init(m_gfx);
    m_particleSimPass->SetSystem(m_particleSystem.get());

    // ParticleRenderPass added to graph in Compile(); SetSystem is pure data-store, post-Init fine.
    if (m_particleRenderPass)
        m_particleRenderPass->SetSystem(m_particleSystem.get());

    // ---- Trail system (GPU ribbon trails) ---------------------------------
    m_trailSystem = std::make_unique<TrailSystem>();
    m_trailSystem->Init(m_gfx);

    m_trailUpdatePass = std::make_unique<TrailUpdatePass>();
    m_trailUpdatePass->Init(m_gfx);
    m_trailUpdatePass->SetSystem(m_trailSystem.get());

    if (m_trailRenderPass)
        m_trailRenderPass->SetSystem(m_trailSystem.get());

    // ---- Tracer system (cylindrical-billboard thin-laser pool) ------------
    m_tracerSystem = std::make_unique<TracerSystem>();
    m_tracerSystem->Init(m_gfx);

    m_tracerSimPass = std::make_unique<TracerSimPass>();
    m_tracerSimPass->Init(m_gfx);
    m_tracerSimPass->SetSystem(m_tracerSystem.get());

    if (m_tracerRenderPass)
        m_tracerRenderPass->SetSystem(m_tracerSystem.get());

    // ---- Beam system (procedural tube CS generator) -----------------------
    m_beamSystem = std::make_unique<BeamSystem>();
    m_beamSystem->Init(m_gfx, m_meshMgr.GetDescriptorHeap());

    m_beamSimPass = std::make_unique<BeamSimPass>();
    m_beamSimPass->Init(m_gfx);
    m_beamSimPass->SetSystem(m_beamSystem.get());

    // ---- Afterimage system (post-skinning vertex snapshot pool) -----------
    m_afterimageSystem = std::make_unique<AfterimageSystem>();
    m_afterimageSystem->Init(m_gfx, m_meshMgr.GetDescriptorHeap());

    m_afterimageCapturePass = std::make_unique<AfterimageCapturePass>();
    m_afterimageCapturePass->Init(m_gfx);
    m_afterimageCapturePass->SetSystem(m_afterimageSystem.get());

    // ---- ClusterPass (clustered deferred lighting) ----
    m_clusterPass = std::make_unique<ClusterPass>();
    m_clusterPass->Init(m_gfx);

    // Probe cluster SRVs persistent post-ClusterPass::Init — one-shot wire to LightingPass.
    if (m_lightingPass)
    {
        m_lightingPass->SetReflectionProbeCluster(
            m_clusterPass->GetProbeGridSRVHandle(),
            m_clusterPass->GetProbeIndexSRVHandle());
    }

    // DecalPass reuses ClusterPass cluster AABB SRV; persistent for program lifetime.
    if (m_decalPass)
        m_decalPass->SetClusterAABBSRV(m_clusterPass->GetClusterAABBSRVHandle());

    // ---- CullingPass (GPU frustum culling) ----
    m_cullingPass = std::make_unique<CullingPass>();
    m_cullingPass->Init(m_gfx);

    // ---- HiZPass (hierarchical-Z mip chain for occlusion culling) ----
    m_hiZPass = std::make_unique<HiZPass>();
    m_hiZPass->Init(m_gfx);

    // SSR pipeline lives behind a single owning subsystem; see Graphics/SSR/SSRSubsystem.h.
    m_ssrSubsystem = std::make_unique<SSRSubsystem>();
    m_ssrSubsystem->Init(m_gfx);

    // Lighting reads UPSAMPLE output (1-frame latent) to dampen its specIBL by (1 - ssrConf).
    if (m_lightingPass)
    {
        uint64_t srv = m_ssrSubsystem->OnResize(m_gfx.GetRenderWidth(),
                                                m_gfx.GetRenderHeight());
        m_lightingPass->SetSSRResult(srv);
    }

    // ---- DebugWirePass (AABB + frustum wireframe) ----
    m_debugWirePass = std::make_unique<DebugWirePass>();
    m_debugWirePass->Init(m_gfx);

    // ---- UIPass (HUD / menus) — renders on the LDR tonemap output ----
    m_uiPass = std::make_unique<UIPass>();
    m_uiPass->Init(m_gfx);

    // ---- WorldUIBillboardPass (world-space UI: HP bars / names) ----
    m_worldUIPass = std::make_unique<WorldUIBillboardPass>();
    m_worldUIPass->Init(m_gfx);

    m_meshMgr.InitBillboardQuad();

    // m_skin.Init() flips the IsInitialised() flag; nothing more to do here.
    LOG_SUCCESS("Renderer: post-compile systems initialised");
}


