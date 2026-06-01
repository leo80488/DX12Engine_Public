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
static_assert(sizeof(TerrainParamsCB) == 336,
    "TerrainParamsCB layout drift — sync Terrain.{ms,ps,as}.hlsl + Renderer.h");

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
    int32_t  layerAlbedoIdx[4] = { -1, -1, -1, -1 };
    int32_t  layerNormalIdx[4] = { -1, -1, -1, -1 };
    int32_t  layerARMIdx   [4] = { -1, -1, -1, -1 };
    int32_t  layerDispIdx  [4] = { -1, -1, -1, -1 };

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

                    hf->baseY       = activeTC->worldCenter.y;
                    hf->heightScale = activeTC->heightScale;
                    LOG_INFO("Terrain: HeightField populated (%ux%u, baseY=%.1f, scale=%.1f)",
                             hf->width, hf->height, hf->baseY, hf->heightScale);
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
        // Sync world-Y every frame so live edits flow to collision without re-decoding.
        if (activeTC->heightField)
        {
            activeTC->heightField->baseY       = activeTC->worldCenter.y;
            activeTC->heightField->heightScale = activeTC->heightScale;
        }

        // Splatmap (root[11] descriptor table).
        syncSlot(activeTC->splatmapPath, cache.splatmap, nullptr, &splatmapSRV);
        activeTC->splatmapHandle = cache.splatmap.handle;
        activeTC->splatmapSRV    = splatmapSRV;

        // 4 layers × 4 bindless maps (albedo, normal, ARM, disp).
        for (int li = 0; li < 4; ++li)
        {
            auto& l    = activeTC->layers[li];
            auto& slot = cache.layers[li];
            syncSlot(l.albedoPath, slot.albedo, &layerAlbedoIdx[li], nullptr);
            syncSlot(l.normalPath, slot.normal, &layerNormalIdx[li], nullptr);
            syncSlot(l.armPath,    slot.arm,    &layerARMIdx[li],    nullptr);
            syncSlot(l.dispPath,   slot.disp,   &layerDispIdx[li],   nullptr);

            l.albedoHandle      = slot.albedo.handle;
            l.normalHandle      = slot.normal.handle;
            l.armHandle         = slot.arm.handle;
            l.dispHandle        = slot.disp.handle;
            l.albedoBindlessIdx = layerAlbedoIdx[li];
            l.normalBindlessIdx = layerNormalIdx[li];
            l.armBindlessIdx    = layerARMIdx[li];
            l.dispBindlessIdx   = layerDispIdx[li];
        }

        // First-resident-per-layer debug print to disambiguate load-failure vs shader bug.
        static int s_lastLoggedAlbedoIdx[4] = { -2, -2, -2, -2 };
        for (int li = 0; li < 4; ++li)
        {
            const int32_t cur = layerAlbedoIdx[li];
            if (cur != s_lastLoggedAlbedoIdx[li])
            {
                LOG_INFO("Terrain layer[%d] albedo bindlessIdx=%d (path='%s')",
                         li, cur,
                         activeTC->layers[li].albedoPath.c_str());
                s_lastLoggedAlbedoIdx[li] = cur;
            }
        }
    }

    // ---- Upload TerrainCB. CB stores bottom-left corner (XZ) so MS uses origin + gridXY * step.
    {
        TerrainParamsCB cb{};
        const float halfSize = activeTC->worldSize * 0.5f;
        cb.worldOriginX       = activeTC->worldCenter.x - halfSize;
        cb.worldOriginY       = activeTC->worldCenter.z - halfSize;  // .y is world Z
        cb.worldSize          = activeTC->worldSize;
        cb.heightScale        = activeTC->heightScale;
        cb.worldCenterY       = activeTC->worldCenter.y;
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

        for (int li = 0; li < 4; ++li)
        {
            const auto& l            = activeTC->layers[li];
            cb.layerBindlessIdx[li]  = layerAlbedoIdx[li];
            cb.layerTilingScale[li]  = l.tilingScale;
            cb.layerNormalIdx[li]    = layerNormalIdx[li];
            cb.layerARMIdx[li]       = layerARMIdx[li];
            cb.layerDispIdx[li]      = layerDispIdx[li];
            cb.layerMinHeight   [li] = l.minHeight;
            cb.layerMaxHeight   [li] = l.maxHeight;
            cb.layerFadeHeight  [li] = (l.fadeHeight   > 1e-3f) ? l.fadeHeight   : 1e-3f;
            cb.layerMinSlopeDeg [li] = l.minSlopeDeg;
            cb.layerMaxSlopeDeg [li] = l.maxSlopeDeg;
            cb.layerFadeSlopeDeg[li] = (l.fadeSlopeDeg > 1e-3f) ? l.fadeSlopeDeg : 1e-3f;
        }

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

    // Arm the pass; pass self-skips without heightmapSRV. Splatmap optional (PS slope-debug fallback).
    TerrainPass::TileBindings tb;
    tb.heightmapSRV      = heightmapSRV;
    tb.splatmapSRV       = splatmapSRV;
    // 1 AS group per 32 sub-tiles → tilesPerSide² / 32 dispatches; AS culls + DispatchMesh's survivors.
    {
        constexpr uint32_t kASGroupSize = 32;   // must match Terrain.as.hlsl
        const uint32_t n            = activeTC->tilesPerSide ? activeTC->tilesPerSide : 1u;
        const uint32_t totalSubTiles = n * n;
        tb.dispatchAsGroupCount = (totalSubTiles + kASGroupSize - 1u) / kASGroupSize;
    }
    m_terrainPass->SetActiveTile(tb);
}

