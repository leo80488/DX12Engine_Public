#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"

// render passes
#include "RenderGraph/RenderPass/GrassPass.h"
#include "RenderGraph/RenderPass/WaterPass.h"

// ECS components
#include "ECS/GrassComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

// Renderer_GrassWater.cpp — Grass + Water per-frame sync (one TU per Renderer
// subsystem, mirrors Renderer_Terrain.cpp). Both systems piggyback on the
// terrain heightmap that BuildScene_SyncTerrain refreshed earlier in
// BuildRenderScene — these MUST run after it so TerrainComponent's mutable
// heightmapSRV / heightmapHandle fields are current.

namespace
{
    // First live component of a pool, or nullptr (singleton-by-convention).
    template <typename T>
    T* FirstLive(World& world)
    {
        auto* pool = world.GetPool<T>();
        if (!pool || pool->Size() == 0) return nullptr;
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
            if (ents[i] != NullEntity)
                return &data[i];
        return nullptr;
    }

    // Heightmap texel size (1/width) — same fallback the terrain CB uses.
    float HeightmapTexel(Resource::TextureSystem* texSys,
                         const TerrainComponent*  tc)
    {
        uint32_t w = 1024;
        if (texSys && tc
            && tc->heightmapHandle != Resource::kInvalidTextureHandle
            && texSys->IsReady(tc->heightmapHandle))
        {
            if (const RHI::Texture* tex = texSys->GetTexture(tc->heightmapHandle))
                if (tex->IsValid() && tex->desc.width > 0)
                    w = tex->desc.width;
        }
        return 1.0f / static_cast<float>(w);
    }
}

// ---------------------------------------------------------------------------
// Grass: gather the first GrassComponent + the first TerrainComponent's
// heightmap mapping, fill GrassCBData, arm GrassPass.
// ---------------------------------------------------------------------------
void Renderer::BuildScene_SyncGrass(World& world)
{
    if (!m_grassPass) return;

    GrassComponent* gc = FirstLive<GrassComponent>(world);
    if (!gc || !gc->enabled || gc->density <= 0.0f)
    {
        m_grassPass->SetActiveField({}, {});
        return;
    }

    const TerrainComponent* tc = FirstLive<TerrainComponent>(world);
    const uint64_t heightmapSRV = tc ? tc->heightmapSRV : 0;

    GrassCBData cb{};

    const float half  = gc->worldSize * 0.5f;
    cb.grassOriginX   = gc->worldCenter.x - half;
    cb.grassOriginZ   = gc->worldCenter.z - half;
    cb.grassSize      = std::max(gc->worldSize, 1.0f);
    // Re-anchored mapping computed by SyncTerrain this frame (runs right
    // before us) — keeps grass roots on the exact surface the terrain
    // rasterizes regardless of the heightmap's data range.
    cb.baseY          = tc ? tc->effBaseY        : gc->worldCenter.y;
    cb.heightScale    = tc ? tc->effHeightScale  : 0.0f;
    cb.hmTexel        = HeightmapTexel(m_texSys, tc);
    cb.hasHeightmap   = (heightmapSRV != 0) ? 1u : 0u;

    const uint32_t patches = std::clamp(gc->patchesPerSide, 1u, 1024u);
    cb.patchesPerSide = patches;

    if (tc)
    {
        const float tHalf = tc->worldSize * 0.5f;
        cb.hmUVOffsetX    = tc->heightmapUVOffset.x;
        cb.hmUVOffsetY    = tc->heightmapUVOffset.y;
        cb.hmUVScaleX     = tc->heightmapUVScale.x;
        cb.hmUVScaleY     = tc->heightmapUVScale.y;
        cb.terrainOriginX = tc->worldCenter.x - tHalf;
        cb.terrainOriginZ = tc->worldCenter.z - tHalf;
        cb.terrainSize    = std::max(tc->worldSize, 1.0f);
    }
    else
    {
        cb.hmUVScaleX     = cb.hmUVScaleY = 1.0f;
        cb.terrainOriginX = cb.grassOriginX;
        cb.terrainOriginZ = cb.grassOriginZ;
        cb.terrainSize    = cb.grassSize;
    }

    cb.cameraPos[0] = m_view.cameraPosition.x;
    cb.cameraPos[1] = m_view.cameraPosition.y;
    cb.cameraPos[2] = m_view.cameraPosition.z;
    cb.time         = m_globalTimeSec;
    cb.prevTime     = m_globalTimeSec - m_deltaTime;

    cb.lod0Dist = std::max(gc->lod0Dist, 1.0f);
    cb.lod1Dist = std::max(gc->lod1Dist, cb.lod0Dist + 1.0f);
    cb.cullDist = std::max(gc->cullDist, cb.lod1Dist + 1.0f);
    cb.density  = gc->density;

    cb.bladeHeight    = std::max(gc->bladeHeight, 0.01f);
    cb.bladeHeightVar = std::clamp(gc->bladeHeightVar, 0.0f, 0.95f);
    cb.bladeWidth     = std::max(gc->bladeWidth, 0.002f);
    cb.tiltMax        = gc->tiltMaxDeg * (XM_PI / 180.0f);
    cb.bendAmount     = std::clamp(gc->bendAmount, 0.0f, 1.0f);

    // Wind direction normalized on the CPU so the shader never renormalizes.
    {
        float wx = gc->windDir.x, wz = gc->windDir.y;
        const float len = std::sqrt(wx * wx + wz * wz);
        if (len > 1e-4f) { wx /= len; wz /= len; }
        else             { wx = 1.0f; wz = 0.0f; }
        cb.windDirX = wx;
        cb.windDirZ = wz;
    }
    cb.windStrength = gc->windStrength;
    cb.windSpeed    = gc->windSpeed;
    cb.windScale    = std::max(gc->windScale, 1e-4f);

    cb.clumpCellSize = std::max(gc->clumpCellSize, 0.05f);
    cb.clumpBlend    = std::clamp(gc->clumpBlend, 0.0f, 1.0f);

    cb.minWorldY   = gc->minWorldY;
    cb.maxWorldY   = gc->maxWorldY;
    cb.maxSlopeCos = std::cos(std::clamp(gc->maxSlopeDeg, 0.0f, 89.9f)
                              * (XM_PI / 180.0f));

    cb.baseColor[0] = gc->baseColor.x; cb.baseColor[1] = gc->baseColor.y;
    cb.baseColor[2] = gc->baseColor.z; cb.baseColor[3] = 1.0f;
    cb.tipColor[0]  = gc->tipColor.x;  cb.tipColor[1]  = gc->tipColor.y;
    cb.tipColor[2]  = gc->tipColor.z;  cb.tipColor[3]  = 1.0f;

    cb.colorNoiseScale  = std::max(gc->colorNoiseScale, 1e-4f);
    cb.colorNoiseAmount = std::clamp(gc->colorNoiseAmount, 0.0f, 1.0f);
    cb.rootAO       = std::clamp(gc->rootAO, 0.0f, 1.0f);
    cb.normalBlend  = std::clamp(gc->normalBlend, 0.0f, 1.0f);
    cb.viewThicken  = std::clamp(gc->viewThicken, 0.0f, 2.0f);
    cb.farWidthMul  = std::max(gc->farWidthMul, 1.0f);
    cb.roughness    = std::clamp(gc->roughness, 0.02f, 1.0f);
    cb.seed         = gc->seed;

    // Camera frustum planes (same convention as TerrainParamsCB).
    const auto& fp = m_view.frustum;
    for (int p = 0; p < 6; ++p)
    {
        cb.frustumPlanes[p][0] = fp[p].normal.x;
        cb.frustumPlanes[p][1] = fp[p].normal.y;
        cb.frustumPlanes[p][2] = fp[p].normal.z;
        cb.frustumPlanes[p][3] = fp[p].distance;
    }

    GrassPass::FieldBindings fb;
    fb.heightmapSRV = heightmapSRV;
    {
        constexpr uint32_t kASGroupSize = 32;   // must match Grass.as.hlsl
        const uint32_t totalPatches = patches * patches;
        fb.dispatchAsGroupCount = (totalPatches + kASGroupSize - 1u) / kASGroupSize;
    }
    m_grassPass->SetActiveField(fb, cb);
}

// ---------------------------------------------------------------------------
// Water: gather the first WaterComponent (+ terrain mapping for the analytic
// depth), fill WaterCBData, arm WaterPass. The sky-cube SRV is pushed
// separately from SyncSkyboxIBL (single source of truth with SkyboxPass).
// ---------------------------------------------------------------------------
void Renderer::BuildScene_SyncWater(World& world)
{
    if (!m_waterPass) return;

    WaterComponent* wc = FirstLive<WaterComponent>(world);
    if (!wc || !wc->enabled || wc->worldSize <= 0.0f)
    {
        m_waterPass->SetActiveWater({}, {});
        return;
    }

    const TerrainComponent* tc = FirstLive<TerrainComponent>(world);
    const uint64_t heightmapSRV = tc ? tc->heightmapSRV : 0;

    // ---- Flow-normal map sync (acquire/release on path change, promote on
    //      ready) — same pattern as BuildScene_SyncTerrain's syncSlot.
    uint64_t normalASRV = 0;
    uint64_t normalBSRV = 0;
    if (m_texSys && m_resMgr)
    {
        auto syncSlot = [&](const std::string& path,
                            TerrainTexSlot&    slot,
                            uint64_t*          outSrv)
        {
            if (path != slot.path)
            {
                if (slot.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(slot.handle, m_gfx);
                slot.path   = path;
                slot.handle = path.empty()
                    ? Resource::kInvalidTextureHandle
                    : m_texSys->Acquire(path, *m_resMgr, m_gfx);
            }
            if (slot.handle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(slot.handle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(slot.handle))
                    if (tex->IsValid())
                        *outSrv = m_gfx.GetTextureSRVGpuHandle(*tex);
            }
        };
        syncSlot(wc->normalMapAPath, m_waterNormalA, &normalASRV);
        syncSlot(wc->normalMapBPath, m_waterNormalB, &normalBSRV);
        wc->normalMapAHandle = m_waterNormalA.handle;
        wc->normalMapBHandle = m_waterNormalB.handle;
        wc->normalMapASRV    = normalASRV;
        wc->normalMapBSRV    = normalBSRV;
    }

    WaterCBData cb{};

    const float half = wc->worldSize * 0.5f;
    cb.waterOriginX = wc->worldCenter.x - half;
    cb.waterOriginZ = wc->worldCenter.z - half;
    cb.waterSize    = wc->worldSize;
    cb.waterHeight  = wc->worldCenter.y;

    cb.deepColor[0]    = wc->deepColor.x;    cb.deepColor[1]    = wc->deepColor.y;
    cb.deepColor[2]    = wc->deepColor.z;    cb.deepColor[3]    = 1.0f;
    cb.shallowColor[0] = wc->shallowColor.x; cb.shallowColor[1] = wc->shallowColor.y;
    cb.shallowColor[2] = wc->shallowColor.z; cb.shallowColor[3] = 1.0f;

    {
        float fx = wc->flowDir.x, fz = wc->flowDir.y;
        const float len = std::sqrt(fx * fx + fz * fz);
        if (len > 1e-4f) { fx /= len; fz /= len; }
        else             { fx = 1.0f; fz = 0.0f; }
        cb.flowDirX = fx;
        cb.flowDirZ = fz;
    }
    cb.flowSpeed      = wc->flowSpeed;
    cb.normalTiling   = std::max(wc->normalTiling, 1e-3f);
    cb.normalStrength = std::max(wc->normalStrength, 0.0f);
    cb.absorbDist     = std::max(wc->absorbDist, 0.01f);
    cb.shoreFade      = std::max(wc->shoreFade, 0.01f);
    cb.fresnelF0      = std::clamp(wc->fresnelF0, 0.0f, 1.0f);
    cb.specPower      = std::max(wc->specPower, 1.0f);
    cb.reflStrength   = std::max(wc->reflStrength, 0.0f);
    cb.time           = m_globalTimeSec;
    cb.gridQuads      = std::clamp(wc->gridQuads, 1u, 512u);
    // GBuffer roughness for the SSR cone — derived from the Blinn glint
    // power (Blinn → Beckmann-ish): rough = sqrt(2 / (power + 2)).
    // specPower 260 → ~0.087: near-mirror, stays under the SSR roughness
    // cutoff so the trace always runs on water.
    cb.surfaceRoughness = std::clamp(
        std::sqrt(2.0f / (cb.specPower + 2.0f)), 0.02f, 0.30f);

    if (tc)
    {
        const float tHalf     = tc->worldSize * 0.5f;
        cb.terrainOriginX     = tc->worldCenter.x - tHalf;
        cb.terrainOriginZ     = tc->worldCenter.z - tHalf;
        cb.terrainSize        = std::max(tc->worldSize, 1.0f);
        // Re-anchored mapping (see SyncTerrain) — the analytic water depth
        // must measure against the same surface the terrain renders.
        cb.terrainBaseY       = tc->effBaseY;
        cb.terrainHeightScale = tc->effHeightScale;
        cb.hasTerrain         = (heightmapSRV != 0) ? 1u : 0u;
        cb.hmTexel            = HeightmapTexel(m_texSys, tc);
        cb.hmUVOffsetX        = tc->heightmapUVOffset.x;
        cb.hmUVOffsetY        = tc->heightmapUVOffset.y;
        cb.hmUVScaleX         = tc->heightmapUVScale.x;
        cb.hmUVScaleY         = tc->heightmapUVScale.y;
    }
    else
    {
        cb.terrainSize = 1.0f;
        cb.hasTerrain  = 0u;
        cb.hmTexel     = 1.0f / 1024.0f;
        cb.hmUVScaleX  = cb.hmUVScaleY = 1.0f;
    }

    WaterPass::WaterBindings wb;
    wb.heightmapSRV = heightmapSRV;
    wb.normalASRV   = normalASRV;   // 0 → pass-owned flat-normal fallback
    wb.normalBSRV   = normalBSRV;
    wb.active       = true;
    m_waterPass->SetActiveWater(wb, cb);
}
