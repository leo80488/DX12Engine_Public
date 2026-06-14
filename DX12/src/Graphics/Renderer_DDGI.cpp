#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderPass/ShadowPass.h"

// render passes
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"

// ECS components + systems
#include "ECS/ReflectionProbeComponent.h"

// STL / DirectXMath
#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
using TerrainParamsCB = RendererDetail::TerrainParamsCB;
static_assert(sizeof(TerrainParamsCB) == 176,
    "TerrainParamsCB layout drift — sync Terrain.{ms,as,ps,shadow.ms,shadow.as}.hlsl + Renderer.h");

// Renderer_DDGI.cpp — split out of Renderer.cpp (one TU per Renderer subsystem).
// All members belong to class Renderer (declared in Graphics/Renderer.h).
// The include block mirrors Renderer.cpp so every cluster keeps compiling;
// trim per-TU later if desired.
// DDGI + reflection-probe upload / bake / export / per-frame tick.
// Pack ReflectionProbeComponents into GPU StructuredBuffer; FIFO slice assignment (kMaxReflectionProbes).
void Renderer::BuildScene_UploadProbes(World& world)
{
    using namespace Reflection;

    uint32_t activeCount = 0;
    m_probeMgr.SetActiveProbeCount(0);
    auto* lb = m_lightCB.Current(m_gfx);

    auto publishCount = [&]() {
        m_probeMgr.SetActiveProbeCount(activeCount);
        if (lb) lb->reflectionProbeCount = activeCount;
    };

    auto* dst = m_probeMgr.GetUploadPointer(m_gfx);
    if (!dst) { publishCount(); return; }

    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool)                     { publishCount(); return; }
    const auto& probeEnts = probePool->Entities();
    if (probeEnts.empty())              { publishCount(); return; }

    auto& probeData = probePool->Data();

    // Disk-load amortization — see the bakedCubemapPath restore block below.
    constexpr uint32_t kMaxProbeDiskLoadsPerFrame = 2;
    uint32_t diskLoadsThisFrame = 0;

    bool overflowed = false;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        Entity e = probeEnts[i];
        if (!world.IsAlive(e)) continue;

        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) continue;  // probe needs a transform to live in world space

        if (activeCount >= kMaxReflectionProbes) { overflowed = true; break; }

        ReflectionProbeComponent& comp = probeData[i];

        const uint32_t slice = activeCount;
        comp.cubemapSlice = slice;

        // Restore a previously-exported cubemap straight off disk before
        // falling back to the (expensive 6-face + prefilter) bake queue.
        // Attempted at most once per probe — cacheLoadAttempted guards both a
        // missing file and a successful load — and budgeted to a couple of
        // disk loads per frame so a probe-heavy scene doesn't hitch hard on
        // the first frame after a world load (each load does a FlushAndWait).
        if (!comp.IsBaked() && !comp.realtime &&
            !comp.bakedCubemapPath.empty() && !comp.cacheLoadAttempted &&
            diskLoadsThisFrame < kMaxProbeDiskLoadsPerFrame &&
            m_probeMgr.GetArrayTexture().IsValid())
        {
            comp.cacheLoadAttempted = true;
            ++diskLoadsThisFrame;
            auto& dx12 = static_cast<GraphicsDX12&>(m_gfx);
            if (dx12.LoadITEXIntoTextureCube(m_probeMgr.GetArrayTexture(), slice,
                                             RHI::ResourceState::SHADER_RESOURCE,
                                             comp.bakedCubemapPath.c_str()))
                comp.SetBaked(true);   // valid content now → skip the bake enqueue below
            else
                LOG_WARNING("Renderer: probe cubemap '%s' load failed — falling back to bake",
                            comp.bakedCubemapPath.c_str());
        }

        const DirectX::XMFLOAT3 pos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };

        DirectX::XMFLOAT3 inner = comp.innerExtents;
        DirectX::XMFLOAT3 outer = comp.outerExtents;
        outer.x = std::max(outer.x, inner.x);
        outer.y = std::max(outer.y, inner.y);
        outer.z = std::max(outer.z, inner.z);

        GPUReflectionProbe& gpu = dst[slice];
        gpu.position        = pos;
        gpu.influenceRadius = std::sqrt(outer.x*outer.x + outer.y*outer.y + outer.z*outer.z);
        gpu.boxMin          = { pos.x - outer.x, pos.y - outer.y, pos.z - outer.z };
        gpu.boxMax          = { pos.x + outer.x, pos.y + outer.y, pos.z + outer.z };
        gpu.cubemapSlice    = slice;
        gpu.flags           = comp.IsBaked() ? GPU_PROBE_FLAG_BAKED : 0u;
        gpu.innerExtents = {
            std::min(inner.x, outer.x),
            std::min(inner.y, outer.y),
            std::min(inner.z, outer.z) };
        gpu.intensity = std::max(0.0f, comp.intensity);

        if (!comp.IsBaked() || comp.NeedsRebake())
            m_probeMgr.EnqueueBake(slice);

        if (comp.realtime && comp.IsBaked() && comp.tickIntervalFrames > 0)
        {
            const uint64_t interval = comp.tickIntervalFrames;
            if (m_currentFrame >= comp.lastBakedFrame + interval)
            {
                comp.RequestRebake();
                m_probeMgr.EnqueueBake(slice);
                comp.lastBakedFrame = m_currentFrame;
            }
        }

        ++activeCount;
    }

    if (overflowed)
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            LOG_WARNING("Renderer: more reflection probes in scene than kMaxReflectionProbes=%u — extras ignored",
                        kMaxReflectionProbes);
            s_warned = true;
        }
    }

    publishCount();

    if (m_clusterPass)
        m_clusterPass->SetProbes(m_probeMgr.GetBufferSrv(m_gfx), activeCount);

    // Probe StructuredBuffer is ring-allocated — rebind the current frame's
    // SRV to the consumers that bake it into their root tables (Lighting,
    // Transparent). The array SRV stays static; only the buffer SRV rotates.
    const uint64_t probeArraySrv  = m_probeMgr.GetArraySrv();
    const uint64_t probeBufferSrv = m_probeMgr.GetBufferSrv(m_gfx);
    if (m_lightingPass)
        m_lightingPass->SetReflectionProbes(probeArraySrv, probeBufferSrv);
    if (m_transparentPass)
        m_transparentPass->SetReflectionProbes(probeArraySrv, probeBufferSrv);
}

// Bake queue API; ProcessProbeBakeQueue consumes one entry/frame (amortizes 6-face + prefilter cost).
void Renderer::BakeProbe(uint32_t cubeSlice)
{
    m_probeMgr.EnqueueBake(cubeSlice);
}

void Renderer::BakeAllProbes()
{
    if (!m_lastWorld) return;
    auto* probePool = m_lastWorld->GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        ReflectionProbeComponent& comp = probeData[i];
        if (comp.cubemapSlice == ReflectionProbeComponent::kInvalidSlice) continue;
        comp.RequestRebake();
        m_probeMgr.EnqueueBake(comp.cubemapSlice);
    }
}

// Export baked (non-realtime) probe cubemaps to .itex beside the .iscene so
// the next LoadScene restores them off disk instead of re-baking. Call right
// before Resource::SaveScene — it mutates ReflectionProbeComponent::
// bakedCubemapPath, which the component serializer then persists.
void Renderer::ExportBakedProbeCubemaps(World& world, const std::string& worldPath)
{
    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();
    if (probeEnts.empty())                       return;
    if (!m_probeMgr.GetArrayTexture().IsValid()) return;

    auto& dx12 = static_cast<GraphicsDX12&>(m_gfx);

    namespace fs = std::filesystem;
    const fs::path    worldP(worldPath);
    const std::string stem     = worldP.stem().string();
    const fs::path    probeDir = worldP.parent_path() / (stem + "_probes");

    // New probes get file indices above the highest already-assigned one, so
    // re-saves keep stable filenames and never clobber each other.
    uint32_t nextIdx = 0;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        const std::string& p = probeData[i].bakedCubemapPath;
        if (p.empty()) continue;
        const std::string fn = fs::path(p).stem().string(); // e.g. "probe_3"
        if (fn.rfind("probe_", 0) == 0)
        {
            uint32_t n = 0;
            if (sscanf_s(fn.c_str() + 6, "%u", &n) == 1)
                nextIdx = std::max(nextIdx, n + 1);
        }
    }

    uint32_t exported = 0, cleared = 0;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        if (!world.IsAlive(probeEnts[i])) continue;
        ReflectionProbeComponent& comp = probeData[i];

        // Realtime probes re-bake on an interval; un-baked / unslotted probes
        // have no valid cubemap content. Either way: nothing to persist —
        // drop any stale path so the next load doesn't restore garbage.
        if (comp.realtime ||
            !comp.IsBaked() ||
            comp.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
        {
            if (!comp.bakedCubemapPath.empty()) { comp.bakedCubemapPath.clear(); ++cleared; }
            continue;
        }

        // Assign a stable path on first export; reuse it on subsequent saves.
        // Stored relative to the working dir when possible so the scene stays
        // portable (matches how other asset paths are persisted).
        if (comp.bakedCubemapPath.empty())
        {
            const fs::path file = probeDir / ("probe_" + std::to_string(nextIdx++) + ".itex");
            std::error_code ec;
            const fs::path rel = fs::relative(file, fs::current_path(), ec);
            comp.bakedCubemapPath =
                (!ec && !rel.empty()) ? rel.generic_string() : file.generic_string();
        }

        if (dx12.SaveTextureCubeToITEX(m_probeMgr.GetArrayTexture(),
                                       comp.cubemapSlice,
                                       RHI::ResourceState::SHADER_RESOURCE,
                                       comp.bakedCubemapPath.c_str()))
        {
            ++exported;
        }
        else
        {
            // Don't persist a path pointing at a file that wasn't written —
            // the probe will simply re-bake on the next load instead.
            LOG_WARNING("Renderer: probe cubemap export failed (entity %u) — will re-bake on load",
                        probeEnts[i]);
            comp.bakedCubemapPath.clear();
        }
    }

    if (exported || cleared)
        LOG_INFO("Renderer::ExportBakedProbeCubemaps — exported %u, cleared %u ('%s')",
                 exported, cleared, probeDir.generic_string().c_str());
}

// CPU-side DDGI bookkeeping: collect volumes, alloc/resize, refresh CBs, publish to LightCB.
// GPU dispatches (TLAS build + trace + relight) live in Render().
void Renderer::BuildScene_UpdateDDGI(World& world)
{
    auto* lb = m_lightCB.Current(m_gfx);

    // Singleton-entity settings override; else Renderer's defaults remain in effect.
    if (auto* settingsPool = world.GetPool<IndirectLightingSettingsComponent>())
    {
        const auto& sEnts = settingsPool->Entities();
        if (!sEnts.empty()) m_ddgiSettings = settingsPool->Data().front();
    }

    // No volume pool → 0 volumes this frame; LightCB still needs the master toggles.
    // EnsurePool for runtime component: World serialization deliberately skips
    // DDGIVolumeRuntimeComponent (GPU handles, rebuilt on demand). After a fresh
    // load, no entity holds one yet, so the runtime pool would not exist and the
    // auto-promote loop below would never run — DDGI silently disabled until the
    // user manually adds a volume in the editor. EnsurePool creates the empty pool
    // so the loop can attach the runtime component on first tick.
    auto* volPool = world.GetPool<DDGIVolumeComponent>();
    auto* runPool = world.EnsurePool<DDGIVolumeRuntimeComponent>();
    uint32_t activeCount    = 0;

    // Parallel arrays for the manager's Tick + the GC pass below. Declared out
    // here (not inside the volPool block) so the GC still runs when this world
    // has no DDGIVolume pool at all — see the FreeUnclaimedSlots note below.
    const DDGIVolumeComponent*  vols[DDGI::kMaxVolumes] = {};
    DDGIVolumeRuntimeComponent* runs[DDGI::kMaxVolumes] = {};

    static uint32_t s_ddgiScanLogCounter = 0;
    //const bool ddgiScanLogTick = (++s_ddgiScanLogCounter % 60u) == 0;
    const bool ddgiScanLogTick = false;

    if (ddgiScanLogTick)
    {
        size_t volEntCount = (volPool ? volPool->Entities().size() : 0);
        LOG_INFO("DDGI scan: volPool=%s entities=%zu",
                 volPool ? "ok" : "null", volEntCount);
    }

    if (volPool && runPool)
    {
        auto& volEnts = volPool->Entities();
        auto& volData = volPool->Data();

        // Promote runtime component for newly-added volumes.
        for (size_t i = 0; i < volEnts.size(); ++i)
        {
            Entity e = volEnts[i];
            if (!world.IsAlive(e)) continue;
            if (!runPool->Get(e))
                world.AddComponent<DDGIVolumeRuntimeComponent>(e, {});
        }

        // alloc/resize on first sight or probe-count change.
        for (size_t i = 0; i < volEnts.size() && activeCount < DDGI::kMaxVolumes; ++i)
        {
            Entity e = volEnts[i];
            if (!world.IsAlive(e)) continue;
            DDGIVolumeRuntimeComponent* rt = runPool->Get(e);
            if (!rt) continue;

            DDGIVolumeComponent& vc = volData[i];
            if (!m_ddgiMgr.AllocateOrUpdate(m_gfx, vc, *rt))
                continue;

            vols[activeCount] = &vc;
            runs[activeCount] = rt;
            activeCount++;
        }

        // Trace CS picks random point/spot light per ray from cluster buffer;
        // 0 → sky-miss + multi-bounce only.
        const uint32_t ddgiLightCount =
            m_clusterPass ? m_clusterPass->GetLightCount() : 0u;

        // Directional sun — read back the FINAL LightCB sun. The light gather
        // wrote the authored directional into LightCB.lightDir/lightColor; then
        // SyncSkyboxIBL's TOD override (run earlier this frame, inside
        // BuildScene_UploadLights) replaced it with the active day/night body
        // when Time-of-Day is enabled. This is the SAME single sun the deferred
        // Lighting.ps applies, so feeding it to the trace CS makes DDGI's
        // directional GI track both the authored light AND the TOD cycle — even
        // when the sun is not a tagged SunLightTag entity (the cluster buffer
        // alone never sees that override).
        XMFLOAT3 sunDir{ 0.0f, -1.0f, 0.0f };
        XMFLOAT3 sunCol{ 0.0f,  0.0f, 0.0f };
        if (const auto* lb = m_lightCB.Current(m_gfx))
        {
            sunDir = { lb->lightDir[0],   lb->lightDir[1],   lb->lightDir[2]   };
            sunCol = { lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
        }
        m_ddgiMgr.Tick(m_gfx, vols, runs, activeCount, ddgiLightCount, sunDir, sunCol);
    }

    // GC orphaned slots — ALWAYS, even when volPool==null (the new world has no
    // DDGIVolume component pool at all). Otherwise a scene with zero volumes
    // leaves the PREVIOUS world's volumes allocated and m_activeCount stale, so
    // the DDGI execution gate (GetActiveVolumeCount() > 0, in Render()) keeps
    // passing and the GPU keeps tracing/relighting probes for a world that no
    // longer has DDGI — wasted frame time that never goes away on scene switch.
    // With activeCount==0 this frees every allocated slot and zeroes the count.
    // DestroyVolume uses deferred release, so freeing in the build phase is safe.
    // (Also the original purpose: drop slots whose probe-count layout changed.)
    uint32_t claimed[DDGI::kMaxVolumes] = {};
    for (uint32_t i = 0; i < activeCount; ++i)
        claimed[i] = runs[i] ? runs[i]->volumeSlot : 0xFFFFFFFFu;
    m_ddgiMgr.FreeUnclaimedSlots(m_gfx, claimed, activeCount);

    // LightCB integration knobs. ddgiVolumeCount==0 → shader keeps Sky IBL.
    if (lb)
    {
        lb->ddgiVolumeCount         = activeCount;
        lb->ddgiEnabled             = (m_ddgiSettings.ddgiEnabled && activeCount > 0) ? 1u : 0u;
        lb->ddgiDiffuseScale        = m_ddgiSettings.ddgiDiffuseScale;
        lb->skyIBLDiffuseScale      = m_ddgiSettings.skyIBLDiffuseScale;
        lb->ddgiAONearFieldStrength = m_ddgiSettings.ddgiAONearFieldStrength;
    }

    // SRV table bases for multi-volume; shader indexes by its loop counter (slot offset).
    if (m_lightingPass)
    {
        if (activeCount > 0)
        {
            m_lightingPass->SetDDGI(
                m_ddgiMgr.GetVolumeBufferSrv(m_gfx),
                m_ddgiMgr.GetProbeSHTableGpu(),
                m_ddgiMgr.GetDepthTableGpu(),
                m_ddgiMgr.GetProbeDataTableGpu());
        }
        else
        {
            // ddgiVolumeCount==0 prevents reads; clearing routes to placeholder handle.
            m_lightingPass->SetDDGI(0, 0, 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
void Renderer::ProcessProbeBakeQueue(RHI::CommandList colorLastCL)
{
    if (m_probeMgr.BakeQueueEmpty())                return;
    if (!m_lastWorld)                               return;
    if (!m_probeMgr.GetArrayTexture().IsValid())    return;
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    if (!m_materialBuffer[frameSlot].IsValid())     return;

    const uint32_t cubeSlice = m_probeMgr.PeekBake();
    m_probeMgr.PopBake();

    // Resolve slice → entity → component (linear scan; N ≤ 64).
    auto* probePool = m_lastWorld->GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();

    Entity probeEntity = NullEntity;
    ReflectionProbeComponent* probeComp = nullptr;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        if (probeData[i].cubemapSlice == cubeSlice)
        {
            probeEntity = probeEnts[i];
            probeComp   = &probeData[i];
            break;
        }
    }
    if (!probeComp) return;  // probe disappeared since enqueue

    const GlobalTransform* gt = m_lastWorld->GetComponent<GlobalTransform>(probeEntity);
    if (!gt) return;

    // Build the bake context — pull lighting state from SkyIBLPass when present.
    ReflectionProbeCapturePass::BakeContext ctx{};
    ctx.probeArray       = &m_probeMgr.GetArrayTexture();
    ctx.probePos         = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
    if (m_skyIBLPass)
    {
        ctx.sunDir      = m_skyIBLPass->GetSunDir();
        ctx.sunColor    = m_skyIBLPass->GetSunColor();
        ctx.skySHSrv    = m_skyIBLPass->GetSHSrvHandle();
        ctx.iblStrength = m_skyIBLPass->GetIBLStrength();
    }
    ctx.ambientScale = m_ddgiSettings.reflectionProbeBakeAmbient;

    // CSM sun shadowing for the bake — frame-global cascades from ShadowSystem.
    // The shadow array is always allocated (even with no light); shadowStrength
    // gates whether the capture PS samples it. Cascades are built for the main
    // camera, so probes within the camera's cascade range bake shadowed.
    if (m_shadowPass && m_shadowSystem && m_shadowSystem->HasValidLight())
    {
        ctx.shadowArraySrv = m_shadowPass->GetShadowArrayGpuHandle();
        const DirectX::XMFLOAT4X4* casc = m_shadowSystem->CascadeMatricesTransposed();
        const float*               spl  = m_shadowSystem->CascadeSplits();
        for (int c = 0; c < ShadowSystem::kCascadeCount && c < 4; ++c)
            ctx.cascadeVP[c] = casc[c];
        ctx.cascadeSplits  = { spl[0], spl[1], spl[2], spl[3] };
        ctx.camPos         = m_camera.position;
        ctx.camFwd         = m_view.cameraForward;
        ctx.shadowStrength = ShadowSystem::ShadowStrength();
    }
    ctx.materialBufSrv   = m_gfx.GetBufferSRVGpuHandle(m_materialBuffer[frameSlot]);
    ctx.bindlessTexTable = m_gfx.GetBindlessTextureTableGpuHandle();
    ctx.bindlessBufTable = m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle();
    ctx.instanceBuffer   = &m_instanceBuffer[m_gfx.GetFrameIndex()];
    ctx.meshDescBuffer   = &m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer();

    // Probe-specific cull via BVH range query → DrawList copies filtered by influence AABB.
    // Filtered vectors live on this stack until BakeProbe returns (synchronous record).
    SceneBVH::AABB influenceAabb;
    influenceAabb.min = {
        ctx.probePos.x - probeComp->outerExtents.x,
        ctx.probePos.y - probeComp->outerExtents.y,
        ctx.probePos.z - probeComp->outerExtents.z };
    influenceAabb.max = {
        ctx.probePos.x + probeComp->outerExtents.x,
        ctx.probePos.y + probeComp->outerExtents.y,
        ctx.probePos.z + probeComp->outerExtents.z };

    std::vector<Entity> bvhVisible;
    bvhVisible.reserve(256);
    m_sceneBVH.QueryAABB(influenceAabb, bvhVisible);
    std::unordered_set<Entity> visibleSet(bvhVisible.begin(), bvhVisible.end());

    std::vector<DrawPacket> probeOpaque, probeShadow, probeTransparent;
    auto filterList = [&](DrawList src, std::vector<DrawPacket>& dst)
    {
        dst.reserve(src.size());
        for (const DrawPacket& dp : src)
        {
            // Keep packet if ANY instance sits in influence AABB; off-AABB instances still rasterize.
            const uint32_t endSlot = (std::min)(
                dp.instanceOffset + dp.instanceCount,
                kMaxInstances);
            bool keep = false;
            for (uint32_t i = dp.instanceOffset; i < endSlot; ++i)
            {
                if (visibleSet.count(m_instanceSlotToEntity[i])) { keep = true; break; }
            }
            if (keep) dst.push_back(dp);
        }
    };
    filterList(GetDrawList(DrawFilter::Opaque),      probeOpaque);
    filterList(GetDrawList(DrawFilter::Shadow),      probeShadow);
    filterList(GetDrawList(DrawFilter::Transparent), probeTransparent);

    ctx.opaqueDraws      = DrawList(probeOpaque.data(),      probeOpaque.size());
    ctx.shadowDraws      = DrawList(probeShadow.data(),      probeShadow.size());
    ctx.transparentDraws = DrawList(probeTransparent.data(), probeTransparent.size());
    ctx.defaultWhiteSrv    = m_gbufferPass ? m_gbufferPass->GetDefaultWhiteSrvHandle()      : 0;
    ctx.defaultFlatNormSrv = m_gbufferPass ? m_gbufferPass->GetDefaultFlatNormalSrvHandle() : 0;
    // Skybox capture: reuse SkyboxPass cube VB + active sky cubemap. Either 0 → sky draw skipped.
    if (m_skyboxPass)
        ctx.skyCubeVB = &m_skyboxPass->GetCubeVB();
    if (m_skyIBLPass)
        ctx.skyCubemapSrv = m_skyIBLPass->ResolveSkyboxSrvHandle(/*staticFallback=*/0);

    // Dedicated graphics CL sequenced after main graph (mirrors HiZ Phase 4.5).
    RHI::CommandList bakeCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    bakeCL.gfx = &m_gfx;
    if (colorLastCL.IsValid())
        m_gfx.AddCommandListDependency(bakeCL, colorLastCL);

    uint32_t bakeRegion = m_gfx.BeginGPUTimestamp(bakeCL, "ProbeBake");
    m_probeMgr.GetCapturePass().BakeProbe(bakeCL, cubeSlice, ctx);
    m_gfx.EndGPUTimestamp(bakeCL, bakeRegion);

    probeComp->SetBaked(true);
    probeComp->ClearRebakeRequest();
    // Seed realtime tick so freshly-baked realtime probes wait a full interval.
    probeComp->lastBakedFrame = m_currentFrame;
}

