#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"

// render passes
#include "RenderGraph/RenderPass/TerrainPass.h"

// ECS components + systems
#include "ECS/TerrainComponent.h"

// resource system
#include "Resource/HeightField.h"

// STL / DirectXMath
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
using TerrainParamsCB = RendererDetail::TerrainParamsCB;
using TerrainLayerGPU = RendererDetail::TerrainLayerGPU;
static_assert(sizeof(TerrainParamsCB) == 176,
    "TerrainParamsCB layout drift — sync Terrain.{ms,as,ps,shadow.ms,shadow.as}.hlsl + Renderer.h");
static_assert(sizeof(TerrainLayerGPU) == 48,
    "TerrainLayerGPU stride drift — sync StructuredBuffer<TerrainLayerGPU> in Terrain.ps.hlsl");

// Renderer_Terrain.cpp — split out of Renderer.cpp (one TU per Renderer subsystem).
// All members belong to class Renderer (declared in Graphics/Renderer.h).
// The include block mirrors Renderer.cpp so every cluster keeps compiling;
// trim per-TU later if desired.
// Terrain heightmap sync + TerrainParamsCB upload.
// Single-tile terrain sync: TextureSystem paths, TerrainCB upload, arm TerrainPass.
void Renderer::BuildScene_SyncTerrain(World& world)
{
    if (!m_terrainPass || !m_terrainCB.Current(m_gfx)) return;

    auto* pTerrain = world.GetPool<TerrainComponent>();
    if (!pTerrain || pTerrain->Size() == 0)
    {
        m_terrainPass->SetActiveTile({});
        return;
    }

    // First TerrainComponent only (single-tile); multi-tile/quadtree later.
    const auto& ents = pTerrain->Entities();
    auto&       data = pTerrain->Data();

    Entity            activeEntity = NullEntity;
    TerrainComponent* activeTC     = nullptr;
    for (size_t i = 0; i < ents.size(); ++i)
    {
        if (ents[i] == NullEntity) continue;
        activeEntity = ents[i];
        activeTC     = &data[i];
        break;
    }
    if (!activeTC)
    {
        m_terrainPass->SetActiveTile({});
        return;
    }

    // Texture sync helper: acquire/release/promote, writes SRV + optional bindless idx.
    uint64_t heightmapSRV = 0;
    uint64_t splatmapSRV  = 0;
    // Per-layer bindless indices are written straight onto each layer's mutable
    // fields below (l.albedoBindlessIdx / normalBindlessIdx / armBindlessIdx)
    // and read back when packing the TerrainLayerGPU StructuredBuffer.

    if (m_texSys && m_resMgr)
    {
        auto [it, _] = m_terrainTexCache.try_emplace(activeEntity);
        TerrainTexCache& cache = it->second;

        // path-change → acquire/release; tex-ready → promote. outBindless optional (skip for table-bound).
        auto syncSlot = [&](const std::string& path,
                            TerrainTexSlot&    slot,
                            int32_t*           outBindless,
                            uint64_t*          outSrv) -> void
        {
            if (path != slot.path)
            {
                if (slot.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(slot.handle, m_gfx);

                slot.path   = path;
                slot.handle = path.empty()
                    ? Resource::kInvalidTextureHandle
                    : m_texSys->Acquire(path, *m_resMgr, m_gfx);
                if (outBindless) *outBindless = -1;
                if (outSrv)      *outSrv      = 0;
            }

            if (slot.handle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(slot.handle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(slot.handle))
                {
                    if (tex->IsValid())
                    {
                        if (outSrv)      *outSrv      = m_gfx.GetTextureSRVGpuHandle(*tex);
                        if (outBindless) *outBindless = static_cast<int32_t>(tex->handle_id);
                    }
                }
            }
        };

        // Heightmap (root[10] descriptor table).
        // Path-change → invalidate the CPU-side HeightField below.
        const std::string priorHeightPath = cache.heightmap.path;
        syncSlot(activeTC->heightmapPath, cache.heightmap, nullptr, &heightmapSRV);
        activeTC->heightmapHandle = cache.heightmap.handle;
        activeTC->heightmapSRV    = heightmapSRV;
        if (priorHeightPath != cache.heightmap.path)
            activeTC->heightField.reset();

        // CPU HeightField for collision: decode raw R16_UNORM once per heightmap path.
        if (!activeTC->heightField
            && activeTC->heightmapHandle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(activeTC->heightmapHandle))
        {
            Resource::Handle rmHandle =
                m_texSys->GetResourceManagerHandle(activeTC->heightmapHandle);
            if (const auto* texRes = m_resMgr->Get<Resource::TextureResource>(rmHandle))
            {
                const DirectX::TexMetadata&  meta = texRes->GetMetadata();
                const DirectX::ScratchImage& img  = texRes->GetImage();
                if (meta.format == DXGI_FORMAT_R16_UNORM
                    && meta.width > 0 && meta.height > 0)
                {
                    auto hf = std::make_shared<Resource::HeightField>();
                    hf->width  = static_cast<uint32_t>(meta.width);
                    hf->height = static_cast<uint32_t>(meta.height);
                    hf->samples.resize(static_cast<size_t>(hf->width) *
                                       static_cast<size_t>(hf->height));

                    // Mip 0 / slice 0 = full-res for collision; respect rowPitch (DXTex may pad).
                    if (const DirectX::Image* mip0 = img.GetImage(0, 0, 0))
                    {
                        const size_t rowBytes = static_cast<size_t>(hf->width) * sizeof(uint16_t);
                        for (uint32_t y = 0; y < hf->height; ++y)
                        {
                            const uint8_t* src = mip0->pixels + static_cast<size_t>(y) * mip0->rowPitch;
                            std::memcpy(hf->samples.data() + static_cast<size_t>(y) * hf->width,
                                        src, rowBytes);
                        }
                    }

                    // Actual data range — drives the height-range re-anchoring
                    // below so heightScale edits don't translate the tile.
                    {
                        uint16_t mn = 0xFFFF, mx = 0;
                        for (uint16_t s : hf->samples)
                        {
                            mn = std::min(mn, s);
                            mx = std::max(mx, s);
                        }
                        constexpr float kInv65535 = 1.0f / 65535.0f;
                        hf->dataMin01 = static_cast<float>(mn) * kInv65535;
                        hf->dataMax01 = static_cast<float>(mx) * kInv65535;
                    }

                    hf->baseY       = activeTC->worldCenter.y;
                    hf->heightScale = activeTC->heightScale;
                    LOG_INFO("Terrain: HeightField populated (%ux%u, baseY=%.1f, scale=%.1f, data range [%.3f, %.3f])",
                             hf->width, hf->height, hf->baseY, hf->heightScale,
                             hf->dataMin01, hf->dataMax01);
                    activeTC->heightField = std::move(hf);
                }
                else
                {
                    LOG_WARNING("Terrain: heightmap '%s' format=%u — expected R16_UNORM. Re-import to enable CPU collision.",
                                activeTC->heightmapPath.c_str(),
                                static_cast<unsigned>(meta.format));
                }
            }
        }
        // (CPU HeightField world-Y mapping is synced below, after the
        //  height-range re-anchoring computes the effective pair.)

        // Splatmap (root[11] descriptor table).
        syncSlot(activeTC->splatmapPath, cache.splatmap, nullptr, &splatmapSRV);
        activeTC->splatmapHandle = cache.splatmap.handle;
        activeTC->splatmapSRV    = splatmapSRV;

        // Variable-count layers × 4 bindless maps (albedo, normal, ARM, disp).
        // Keep the per-entity texture cache sized to the layer list; release
        // the slots of any layers that were removed in the editor before
        // shrinking so their TextureHandles don't leak until OnWorldClear.
        const size_t nLayers = activeTC->layers.size();
        if (cache.layers.size() > nLayers)
        {
            for (size_t li = nLayers; li < cache.layers.size(); ++li)
            {
                auto relSlot = [&](TerrainTexSlot& s) {
                    if (s.handle != Resource::kInvalidTextureHandle)
                        m_texSys->Release(s.handle, m_gfx);
                };
                relSlot(cache.layers[li].albedo);
                relSlot(cache.layers[li].normal);
                relSlot(cache.layers[li].arm);
                relSlot(cache.layers[li].disp);
            }
        }
        cache.layers.resize(nLayers);

        for (size_t li = 0; li < nLayers; ++li)
        {
            auto& l    = activeTC->layers[li];
            auto& slot = cache.layers[li];
            // Write bindless indices straight onto the layer's mutable fields;
            // syncSlot leaves them untouched while a texture is still loading,
            // so a resolved index persists across frames.
            syncSlot(l.albedoPath, slot.albedo, &l.albedoBindlessIdx, nullptr);
            syncSlot(l.normalPath, slot.normal, &l.normalBindlessIdx, nullptr);
            syncSlot(l.armPath,    slot.arm,    &l.armBindlessIdx,    nullptr);
            syncSlot(l.dispPath,   slot.disp,   &l.dispBindlessIdx,   nullptr);

            l.albedoHandle = slot.albedo.handle;
            l.normalHandle = slot.normal.handle;
            l.armHandle    = slot.arm.handle;
            l.dispHandle   = slot.disp.handle;
        }

        // First-resident-per-layer debug print to disambiguate load-failure vs shader bug.
        static int s_lastLoggedAlbedoIdx[Renderer::kMaxTerrainLayers] = {};
        static bool s_loggedInit = false;
        if (!s_loggedInit) { for (auto& v : s_lastLoggedAlbedoIdx) v = -2; s_loggedInit = true; }
        for (size_t li = 0; li < std::min<size_t>(nLayers, Renderer::kMaxTerrainLayers); ++li)
        {
            const int32_t cur = activeTC->layers[li].albedoBindlessIdx;
            if (cur != s_lastLoggedAlbedoIdx[li])
            {
                LOG_INFO("Terrain layer[%zu] albedo bindlessIdx=%d (path='%s')",
                         li, cur, activeTC->layers[li].albedoPath.c_str());
                s_lastLoggedAlbedoIdx[li] = cur;
            }
        }
    }

    // ---- Height-range re-anchoring ------------------------------------------
    // Heightmaps rarely use the full [0,1] encodable range (this project's
    // HeightMap sits in ~[0.29, 0.48]). With the raw mapping
    //   Y = worldCenter.y + h * heightScale
    // the lowest valley sits at worldCenter.y + dataMin * heightScale, so a
    // heightScale edit TRANSLATES the whole tile vertically. Re-anchor so:
    //   valley floor (dataMin) → worldCenter.y           (pinned, scale-invariant)
    //   highest peak (dataMax) → worldCenter.y + heightScale
    // i.e. heightScale becomes the TRUE total relief. Implemented purely by
    // feeding adjusted (baseY, scale) into every consumer — shaders unchanged:
    //   effScale = heightScale / (dataMax - dataMin)
    //   effBase  = worldCenter.y - dataMin * effScale
    // Falls back to the raw mapping until the CPU HeightField is decoded
    // (R16_UNORM only) — a one-time settle at load. Note the range is global
    // to the heightmap; tiles sampling a sub-rect via heightmapUVScale pin
    // against the whole map's minimum (conservative).
    {
        float effBaseY  = activeTC->worldCenter.y;
        float effScale  = activeTC->heightScale;
        if (const auto& hf = activeTC->heightField; hf && hf->IsValid())
        {
            const float range = hf->dataMax01 - hf->dataMin01;
            if (range > 1e-5f)
            {
                effScale = activeTC->heightScale / range;
                effBaseY = activeTC->worldCenter.y - hf->dataMin01 * effScale;
            }
            else
            {
                effScale = 0.0f;   // flat data → flat tile at the pivot
                effBaseY = activeTC->worldCenter.y;
            }
        }
        activeTC->effBaseY       = effBaseY;
        activeTC->effHeightScale = effScale;

        // Keep CPU collision on the SAME re-anchored surface (live edits flow
        // every frame without re-decoding).
        if (activeTC->heightField)
        {
            activeTC->heightField->baseY       = effBaseY;
            activeTC->heightField->heightScale = effScale;
        }
    }

    // ---- Upload TerrainCB. CB stores bottom-left corner (XZ) so MS uses origin + gridXY * step.
    {
        TerrainParamsCB cb{};
        const float halfSize = activeTC->worldSize * 0.5f;
        cb.worldOriginX       = activeTC->worldCenter.x - halfSize;
        cb.worldOriginY       = activeTC->worldCenter.z - halfSize;  // .y is world Z
        cb.worldSize          = activeTC->worldSize;
        cb.heightScale        = activeTC->effHeightScale;
        cb.worldCenterY       = activeTC->effBaseY;
        cb.heightmapUVOffsetX = activeTC->heightmapUVOffset.x;
        cb.heightmapUVOffsetY = activeTC->heightmapUVOffset.y;
        cb.heightmapUVScaleX  = activeTC->heightmapUVScale.x;
        cb.heightmapUVScaleY  = activeTC->heightmapUVScale.y;

        // Heightmap width drives texel size; fall back to 1024 pre-load for analytic-normal gradient.
        uint32_t hmWidth = 1024;
        if (m_texSys && activeTC->heightmapHandle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(activeTC->heightmapHandle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(activeTC->heightmapHandle))
                if (tex->IsValid() && tex->desc.width > 0)
                    hmWidth = tex->desc.width;
        }
        cb.heightmapTexel = 1.0f / static_cast<float>(hmWidth);
        cb.hasHeightmap   = (heightmapSRV != 0) ? 1u : 0u;
        cb.hasSplatmap    = (splatmapSRV  != 0) ? 1u : 0u;

        // Per-layer material data lives in the StructuredBuffer (packed below);
        // the CB only carries the count the PS loops over.
        cb.layerCount = std::min<uint32_t>(
            static_cast<uint32_t>(activeTC->layers.size()), Renderer::kMaxTerrainLayers);

        // Height-correlated blend (per-layer disp). Off by default → plain
        // linear blend, so existing terrains are pixel-identical until opted in.
        cb.heightBlendEnable   = activeTC->heightBlendEnabled ? 1u : 0u;
        cb.heightBlendStrength = activeTC->heightBlendStrength;
        cb.heightBlendRange    = (activeTC->heightBlendRange > 1e-4f)
                               ? activeTC->heightBlendRange : 1e-4f;

        cb.tilesPerSide       = activeTC->tilesPerSide ? activeTC->tilesPerSide : 1u;
        cb.enableFrustumCull  = 1u;     // colour pass uses camera frustum

        // Frustum planes (unjittered VP). dot(n,P) + d ≥ 0 = inside; order = L,R,B,T,N,F.
        const auto& fp = m_view.frustum;
        for (int p = 0; p < 6; ++p)
        {
            cb.frustumPlanes[p][0] = fp[p].normal.x;
            cb.frustumPlanes[p][1] = fp[p].normal.y;
            cb.frustumPlanes[p][2] = fp[p].normal.z;
            cb.frustumPlanes[p][3] = fp[p].distance;
        }

        if (auto* dst = m_terrainCB.Current(m_gfx))
            std::memcpy(dst, &cb, sizeof(cb));
    }

    // ---- Upload the per-layer material table (StructuredBuffer<TerrainLayerGPU>).
    // One element per layer; the terrain PS loops over cb.layerCount. Bindless
    // indices come from each layer's mutable fields synced above (-1 = not yet
    // GPU-resident → PS skips that map). fadeHeight/fadeSlope are clamped > 0
    // to keep the smoothstep falloff well-defined.
    const uint32_t frameSlot      = m_gfx.GetFrameIndex();
    uint64_t       layerBufferSRV = (frameSlot < kFrameSlots) ? m_terrainLayerSrv[frameSlot] : 0;
    if (frameSlot < kFrameSlots)
    {
        const uint32_t layerCount = std::min<uint32_t>(
            static_cast<uint32_t>(activeTC->layers.size()), kMaxTerrainLayers);
        if (auto* dst = static_cast<TerrainLayerGPU*>(m_terrainLayerMapped[frameSlot]))
        {
            for (uint32_t li = 0; li < layerCount; ++li)
            {
                const auto& l = activeTC->layers[li];
                TerrainLayerGPU g{};
                g.albedoIdx    = l.albedoBindlessIdx;
                g.normalIdx    = l.normalBindlessIdx;
                g.armIdx       = l.armBindlessIdx;
                g.tilingScale  = l.tilingScale;
                g.minHeight    = l.minHeight;
                g.maxHeight    = l.maxHeight;
                g.fadeHeight   = (l.fadeHeight   > 1e-3f) ? l.fadeHeight   : 1e-3f;
                g.minSlopeDeg  = l.minSlopeDeg;
                g.maxSlopeDeg  = l.maxSlopeDeg;
                g.fadeSlopeDeg = (l.fadeSlopeDeg > 1e-3f) ? l.fadeSlopeDeg : 1e-3f;
                g.dispIdx      = l.dispBindlessIdx;
                dst[li] = g;
            }
        }
    }

    // Arm the pass; pass self-skips without heightmapSRV. Splatmap optional (PS slope-debug fallback).
    TerrainPass::TileBindings tb;
    tb.heightmapSRV      = heightmapSRV;
    tb.splatmapSRV       = splatmapSRV;
    tb.layerBufferSRV    = layerBufferSRV;
    // 1 AS group per 32 sub-tiles → tilesPerSide² / 32 dispatches; AS culls + DispatchMesh's survivors.
    {
        constexpr uint32_t kASGroupSize = 32;   // must match Terrain.as.hlsl
        const uint32_t n            = activeTC->tilesPerSide ? activeTC->tilesPerSide : 1u;
        const uint32_t totalSubTiles = n * n;
        tb.dispatchAsGroupCount = (totalSubTiles + kASGroupSize - 1u) / kASGroupSize;
    }
    m_terrainPass->SetActiveTile(tb);
}

