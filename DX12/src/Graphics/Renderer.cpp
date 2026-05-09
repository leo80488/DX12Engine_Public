#include "Graphics/Renderer.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "ECS/BillboardComponent.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "ECS/TerrainComponent.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/GlassShatterPass.h"
#include "PostProcess/PostProcessStack.h"
#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/VolumeSystem.h"
#include "PostProcess/EntityVolumeSource.h"
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
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "RenderGraph/RenderPass/TracerPasses.h"
#include "Graphics/BeamSystem.h"
#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "ECS/BeamComponent.h"
#include "ECS/ParticleComponent.h"
#include "RenderGraph/RenderPass/TrailPasses.h"
#include "Graphics/TrailSystem.h"
#include "ECS/TrailComponent.h"
#include "RenderGraph/RenderPass/CullingPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/SSRPass.h"
#include "RenderGraph/RenderPass/SSRDepthHierarchyPass.h"
#include "RenderGraph/RenderPass/SceneColorPyramidPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GPUInstanceData.h"
#include <unordered_set>
#include "Graphics/IndirectDrawCommand.h"
#include "Resource/ProceduralMesh.h"
#include "Resource/MaterialSystem.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/MaterialReflectionSync.h"
#include "System/Log.h"
#include "System/TaskSystem.h"
#include "System/EventBus.h"
#include <atomic>
#include <mutex>
#include <condition_variable>

#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <execution>

#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors. Layouts MUST stay in sync with the matching HLSL.
namespace
{
    // viewProj is JITTERED for SV_POSITION; curViewProjNoJitter feeds velocity.
    struct alignas(16) PerViewCB
    {
        float viewProj[16];            // current, jittered (rasterization)
        float prevViewProj[16];        // previous frame, unjittered (velocity)
        float curViewProjNoJitter[16]; // current, unjittered (velocity numerator)
    };
    static constexpr uint64_t kPerViewCBSize = (sizeof(PerViewCB) + 255u) & ~255ull;

    // Mirror of light_cb.hlsli — do NOT reorder fields without touching both.
    struct alignas(16) LightCB
    {
        float    lightDir[3];    float pad0;
        float    lightColor[3];  float pad1;
        float    ambient[3];     float pad2;
        float    cameraPos[3];   float pad3;
        float    invViewProj[16];
        uint32_t iblRadianceMips;
        float    iblStrength;
        uint32_t iblUseSH;                   // 0=irradiance cube, 1=gSkySH
        float    aerialMaxDistKm;            // 0 = AP disabled
        // CSM: cascades 0..2 standard, cascade 3 ultra-far for terrain self-shadow.
        float    shadowMatrix[4][16];        // 256B — per-cascade light VP, transposed
        float    cascadeSplits[4];
        float    cascadeTexelWorldSize[4];
        float    shadowBias;
        float    shadowStrength;             // 0 disabled, 1 full
        float    shadowMapTexelSize;
        float    shadowBlendRange;
        uint32_t shadowFrameIndex;           // PCF dither rotation
        float    shadowNormalOffset;
        float    _shadowPad0;
        float    _shadowPad1;
        float    cameraForward[3];           // unit camera forward
        float    _shadowPad2;
        float    viewMatrix[16];             // clustered lighting (world→view)
        float    clusterNearZ;
        float    clusterFarZ;
        uint32_t clusterLightCount;
        float    nprMinBrightness;
        uint32_t reflectionProbeCount;       // gReflectionProbes valid count; 0 = sky cube fallback
        float    reflectionPad[3];
        uint32_t ddgiVolumeCount;            // 0..4; 0 = SkyIBL diffuse fallback
        uint32_t ddgiEnabled;
        float    ddgiDiffuseScale;
        float    skyIBLDiffuseScale;
        float    ddgiAONearFieldStrength;    // see light_cb.hlsli — DDGI-path AO attenuation
        float    _ddgiPad0;
        float    _ddgiPad1;
        float    _ddgiPad2;
    };
    static constexpr uint64_t kLightCBSize = (sizeof(LightCB) + 255u) & ~255ull;

    struct alignas(16) ShadowPerViewCB { float shadowViewProj[16]; };
    static constexpr uint64_t kShadowCBSize = (sizeof(ShadowPerViewCB) + 255u) & ~255ull;

    // Mirror of Terrain.{ms,ps,as}.hlsl cbuffer TerrainCB; each block is 16B aligned.
    struct alignas(16) TerrainParamsCB
    {
        // Block 0: world tile placement
        float    worldOriginX, worldOriginY;
        float    worldSize;
        float    heightScale;
        // Block 1: heightmap UV remap
        float    heightmapUVOffsetX, heightmapUVOffsetY;
        float    heightmapUVScaleX,  heightmapUVScaleY;
        // Block 2: heightmap meta + splatmap flag + center Y
        float    heightmapTexel;
        uint32_t hasHeightmap;       // 0 ⇒ flat
        uint32_t hasSplatmap;        // 0 ⇒ slope debug colour
        float    worldCenterY;
        // Blocks 3..7: per-layer bindless slots + tiling (-1 ⇒ disabled)
        int32_t  layerBindlessIdx[4];
        float    layerTilingScale[4];
        int32_t  layerNormalIdx[4];
        int32_t  layerARMIdx[4];
        int32_t  layerDispIdx[4];
        // Block 8: multi-tile + AS cull flag
        uint32_t tilesPerSide;
        uint32_t enableFrustumCull;
        float    _pad8a, _pad8b;
        // Blocks 9..14: per-layer auto-blend params (height + slope ranges, PS only)
        float    layerMinHeight   [4];
        float    layerMaxHeight   [4];
        float    layerFadeHeight  [4];
        float    layerMinSlopeDeg [4];
        float    layerMaxSlopeDeg [4];
        float    layerFadeSlopeDeg[4];
        // Blocks 15..20: 6 world-space frustum planes (a*x + b*y + c*z + d ≥ 0).
        // AS-only; ShadowPass overwrites per-cascade for shadow culling.
        float    frustumPlanes[6][4];
    };
    static_assert(sizeof(TerrainParamsCB) == 336, "TerrainParamsCB layout drift — sync Terrain.{ms,ps,as}.hlsl + this struct");
    static constexpr uint64_t kTerrainCBSize = (sizeof(TerrainParamsCB) + 255u) & ~255ull;

    // Lazy linear-color cache refresh — 14 powf calls only when sRGB colors changed.
    static void RefreshLinearCache(const MaterialComponent& mc)
    {
        auto srgbToLinear = [](float c) { return std::powf(std::max(c, 0.f), 2.2f); };
        mc._linearBaseColor     = { srgbToLinear(mc.baseColor.x),
                                    srgbToLinear(mc.baseColor.y),
                                    srgbToLinear(mc.baseColor.z),
                                    mc.baseColor.w };
        mc._linearSpecularColor = { srgbToLinear(mc.specularColor.x),
                                    srgbToLinear(mc.specularColor.y),
                                    srgbToLinear(mc.specularColor.z),
                                    mc.specularColor.w };
        mc._linearEmissiveColor = { srgbToLinear(mc.emissiveColor.x),
                                    srgbToLinear(mc.emissiveColor.y),
                                    srgbToLinear(mc.emissiveColor.z),
                                    mc.emissiveColor.w };
        mc._linearNprDiffuseRamp = { srgbToLinear(mc.nprDiffuseRampColor.x),
                                     srgbToLinear(mc.nprDiffuseRampColor.y),
                                     srgbToLinear(mc.nprDiffuseRampColor.z) };
        mc._linearNprShadowRamp  = { srgbToLinear(mc.nprShadowRampColor.x),
                                     srgbToLinear(mc.nprShadowRampColor.y),
                                     srgbToLinear(mc.nprShadowRampColor.z) };
        const_cast<MaterialComponent&>(mc)._flags &= ~MaterialComponent::GPU_LINEAR_CACHE_DIRTY;
    }

    // Write a MaterialComponent into one slot of the per-frame material buffer.
    // Walks customParams/customTextures in reflection order — must match shader-side indexing.
    static void WriteMatSlot(Resource::MaterialGPUData* buf, uint32_t slot,
                             const MaterialComponent* mc,
                             const ShaderReflect::Reflection* customRefl = nullptr)
    {
        if (!buf) return;
        Resource::MaterialGPUData& dst = buf[slot];
        if (mc)
        {
            if (mc->_flags & MaterialComponent::GPU_LINEAR_CACHE_DIRTY)
                RefreshLinearCache(*mc);

            // ---- PBR scalars ----
            dst.roughnessMin   = mc->roughnessMin;
            dst.roughnessMax   = mc->roughnessMax;
            dst.metalnessMin   = mc->metalnessMin;
            dst.metalnessMax   = mc->metalnessMax;
            dst.reflectance    = mc->reflectance;
            dst.normalStrength = mc->normalMapStrength;
            dst.saturation     = mc->saturation;
            dst.alphaRef       = mc->alphaRef;

            // ---- PBR colors (linear cache — no powf on the hot path) ----
            dst.baseColor[0]      = mc->_linearBaseColor.x;
            dst.baseColor[1]      = mc->_linearBaseColor.y;
            dst.baseColor[2]      = mc->_linearBaseColor.z;
            dst.baseColor[3]      = mc->_linearBaseColor.w;
            dst.specularColor[0]  = mc->_linearSpecularColor.x;
            dst.specularColor[1]  = mc->_linearSpecularColor.y;
            dst.specularColor[2]  = mc->_linearSpecularColor.z;
            dst.specularColor[3]  = mc->_linearSpecularColor.w;
            dst.emissiveColor[0]  = mc->_linearEmissiveColor.x;
            dst.emissiveColor[1]  = mc->_linearEmissiveColor.y;
            dst.emissiveColor[2]  = mc->_linearEmissiveColor.z;
            dst.emissiveColor[3]  = mc->_linearEmissiveColor.w;

            // nprMinBrightness doubles as the "is-NPR" flag (encoded into normal.w by GBuffer).
            const bool isNPR = (mc->shaderType == MaterialComponent::SHADERTYPE_NPR_RAMP ||
                                mc->shaderType == MaterialComponent::SHADERTYPE_NPR_COLOR);
            dst.shaderType         = static_cast<uint32_t>(mc->shaderType);
            dst.nprMinBrightness   = isNPR ? mc->nprMinBrightness : 0.0f;
            dst.nprShadowThreshold = mc->nprShadowThreshold;
            dst.nprShadowSmooth    = mc->nprShadowSmooth;

            dst.nprRimPower         = mc->nprRimPower;
            dst.nprRimStrength      = mc->nprRimStrength;
            dst.nprRampBlend        = mc->nprRampBlend;
            dst.nprBrightnessClamp  = mc->nprBrightnessClamp;

            dst.nprDiffuseRampColor[0] = mc->_linearNprDiffuseRamp.x;
            dst.nprDiffuseRampColor[1] = mc->_linearNprDiffuseRamp.y;
            dst.nprDiffuseRampColor[2] = mc->_linearNprDiffuseRamp.z;
            dst.nprMaxBrightness       = mc->nprMaxBrightness;

            dst.nprShadowRampColor[0] = mc->_linearNprShadowRamp.x;
            dst.nprShadowRampColor[1] = mc->_linearNprShadowRamp.y;
            dst.nprShadowRampColor[2] = mc->_linearNprShadowRamp.z;
            dst._padNprShadow         = 0.0f;

            dst.nprMidWeight  = mc->nprMidWeight;
            dst.nprVeinWeight = mc->nprVeinWeight;
            dst.nprSSSWeight  = mc->nprSSSWeight;
            dst._padNprSss    = 0.0f;

            dst.paramCount = 14;  // legacy "count of populated engine slots"

            for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT && s < Resource::kMaxTextureSlots; ++s)
                dst.textureHandleIds[s] = mc->textures[s].bindlessIndex;
            dst.textureCount = MaterialComponent::TEXTURESLOT_COUNT;

            // Per-material GPU flag bitmask read inside pixel shaders.
            dst.materialFlags = 0u;
            if (mc->_flags & MaterialComponent::EXCLUDE_FROM_SSAO)
                dst.materialFlags |= (1u << 0);   // MAT_FLAG_EXCLUDE_FROM_SSAO
            if (mc->_flags & MaterialComponent::DISABLE_RECEIVE_SHADOW)
                dst.materialFlags |= (1u << 1);   // MAT_FLAG_DISABLE_RECEIVE_SHADOW

            // Custom-shader params/textures — reflection-order, skipping reserved cbuffers/structs/arrays.
            std::memset(dst.customParams,     0, sizeof(dst.customParams));
            for (int i = 0; i < Resource::kMaxCustomTextures; ++i) dst.customTextureIds[i] = -1;
            dst.customParamCount    = 0;
            dst.customTextureCount  = 0;
            // Shading model is authoritative even when custom PS isn't resolved yet.
            dst.customShadingModel  = mc->useCustomShader
                ? static_cast<uint32_t>(mc->customShadingModel)
                : 0u;

            if (customRefl && mc->useCustomShader)
            {
                uint32_t pi = 0;
                for (const auto& cb : customRefl->cbuffers)
                {
                    if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                    for (const auto& v : cb.vars)
                    {
                        if (pi >= Resource::kMaxCustomParams) break;
                        if (v.type == ShaderReflect::VarType::Struct)    continue;
                        if (v.type == ShaderReflect::VarType::Float4x4)  continue;
                        if (v.elements > 0)                              continue;

                        auto it = mc->customParams.find(v.name);
                        if (it != mc->customParams.end())
                        {
                            const auto& val = it->second;
                            dst.customParams[pi][0] = val[0];
                            dst.customParams[pi][1] = val[1];
                            dst.customParams[pi][2] = val[2];
                            dst.customParams[pi][3] = val[3];
                        }
                        ++pi;
                    }
                    if (pi >= Resource::kMaxCustomParams) break;
                }
                dst.customParamCount = pi;

                uint32_t ti = 0;
                for (const auto& b : customRefl->bindings)
                {
                    if (ti >= Resource::kMaxCustomTextures) break;
                    if (b.type != ShaderReflect::ResourceType::Texture) continue;
                    // Skip engine-bound names so user-visible custom slot indexing stays contiguous.
                    if (MaterialReflectionSync::IsReservedTexture(b.name.c_str())) continue;

                    auto it = mc->customTextures.find(b.name);
                    if (it != mc->customTextures.end())
                        dst.customTextureIds[ti] = it->second.bindlessIndex;
                    ++ti;
                }
                dst.customTextureCount = ti;
            }
        }
        else
        {
            dst = {};
            dst.roughnessMin = 0.0f;  dst.roughnessMax = 0.5f;
            dst.metalnessMin = 0.0f;  dst.metalnessMax = 0.0f;
            dst.reflectance  = 0.5f;  // F0 = 0.16 * 0.5² = 0.04
            dst.baseColor[0] = dst.baseColor[1] =
            dst.baseColor[2] = dst.baseColor[3] = 1.0f;  // white
            dst.paramCount   = 9;
        }
    }

    // Hash of every render-affecting MaterialComponent field — keys batch slot reuse.
    // Missing a field here causes silent shared-slot bugs (edits invisible on duplicate materials).
    static uint32_t HashMatParams(const MaterialComponent* mc)
    {
        if (!mc) return 0;
        uint32_t h = 0;
        auto mix = [&](uint32_t u) {
            h ^= u + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        auto mixF = [&](float f) {
            uint32_t u; std::memcpy(&u, &f, 4); mix(u);
        };

        mixF(mc->roughnessMin);  mixF(mc->roughnessMax);
        mixF(mc->metalnessMin);  mixF(mc->metalnessMax);
        mixF(mc->reflectance);   mixF(mc->normalMapStrength);
        mixF(mc->saturation);    mixF(mc->alphaRef);

        mixF(mc->baseColor.x);     mixF(mc->baseColor.y);
        mixF(mc->baseColor.z);     mixF(mc->baseColor.w);
        mixF(mc->specularColor.x); mixF(mc->specularColor.y);
        mixF(mc->specularColor.z); mixF(mc->specularColor.w);
        mixF(mc->emissiveColor.x); mixF(mc->emissiveColor.y);
        mixF(mc->emissiveColor.z); mixF(mc->emissiveColor.w);

        mix(static_cast<uint32_t>(mc->shaderType));
        mix(mc->_flags);

        // NPR scalars
        mixF(mc->nprMinBrightness);   mixF(mc->nprShadowThreshold);
        mixF(mc->nprShadowSmooth);    mixF(mc->nprRimPower);
        mixF(mc->nprRimStrength);     mixF(mc->nprRampBlend);
        mixF(mc->nprBrightnessClamp); mixF(mc->nprMaxBrightness);
        mixF(mc->nprMidWeight);       mixF(mc->nprVeinWeight);
        mixF(mc->nprSSSWeight);

        // NPR colors
        mixF(mc->nprDiffuseRampColor.x); mixF(mc->nprDiffuseRampColor.y);
        mixF(mc->nprDiffuseRampColor.z);
        mixF(mc->nprShadowRampColor.x);  mixF(mc->nprShadowRampColor.y);
        mixF(mc->nprShadowRampColor.z);

        // Custom-shader path identity (covers ShaderLab-edited materials)
        mix(mc->useCustomShader ? 1u : 0u);
        mix(static_cast<uint32_t>(mc->customShaderID));
        mix(static_cast<uint32_t>(mc->customShadingModel));
        // customParams/customTextures: hash size + per-element bits so inspector edits invalidate.
        mix(static_cast<uint32_t>(mc->customParams.size()));
        for (const auto& [name, val] : mc->customParams)
        {
            mixF(val[0]); mixF(val[1]); mixF(val[2]); mixF(val[3]);
        }
        mix(static_cast<uint32_t>(mc->customTextures.size()));
        for (const auto& [name, tex] : mc->customTextures)
            mix(static_cast<uint32_t>(tex.bindlessIndex));

        // All eight texture bindless indices — partial set lets emissive/ramp/AO swaps re-batch wrongly.
        for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
            mix(static_cast<uint32_t>(mc->textures[i].bindlessIndex));

        return h;
    }

} // namespace

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

    if (m_perObjectCBMapped)    m_gfx.UnmapBuffer(m_perObjectCB);
    if (m_lightCBMapped)        m_gfx.UnmapBuffer(m_lightCB);
    if (m_instanceBufferMapped) m_gfx.UnmapBuffer(m_instanceBuffer);
    if (m_materialBufferMapped) m_gfx.UnmapBuffer(m_materialBuffer);
    if (m_terrainCBMapped)      m_gfx.UnmapBuffer(m_terrainCB);

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
        { RHI::Format::R16G16_FLOAT,        0, 0, false, false, L"GBuffer3_Velocity" });
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
        if (m_shadowPass)
        {
            m_shadowPass->SetTerrainPass(m_terrainPass);
            m_shadowPass->SetTerrainParamsCB(&m_terrainCB);
        }
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
        // Reflection probe SRVs are persistent — one-shot wire after m_probeMgr.Init().
        m_lightingPass->SetReflectionProbes(
            m_probeMgr.GetArraySrv(), m_probeMgr.GetBufferSrv());
        // Cluster probe grid/index handles wired post-ClusterPass::Init (end of Compile).
        m_graph.AddPass(std::move(pass));
    }
    {
        auto pass = std::make_unique<SkyboxPass>(m_depthHandle);
        m_skyboxPass = pass.get();
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
        m_transparentPass->SetReflectionProbes(
            m_probeMgr.GetArraySrv(), m_probeMgr.GetBufferSrv());
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
    m_meshMgr.OnWorldClear();  // clears per-library caches + bumps generation
    // DDGI BLAS cache keyed on MeshLibRef → stale after teardown; drop to force rebuild.
    m_ddgiSceneAS.OnWorldClear();
    m_matTexCache.clear();
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
    if (m_ssrPass && m_lightingPass)
    {
        m_ssrPass->EnsureTexture(renderW, renderH);
        if (m_ssrResolvePass && m_ssrTemporalPass && m_ssrUpsamplePass)
        {
            m_ssrResolvePass->EnsureTextures(renderW, renderH);
            m_ssrTemporalPass->EnsureTextures(renderW, renderH);
            m_ssrUpsamplePass->EnsureTexture(renderW, renderH);
            // Lighting reads upsample output (1-frame latent). Disabled → pass 0 so confidence drains.
            if (m_ssrEnabled)
                m_lightingPass->SetSSRResult(m_ssrUpsamplePass->GetColorSrv());
            else
                m_lightingPass->SetSSRResult(0);
        }
        else
        {
            m_lightingPass->SetSSRResult(m_ssrEnabled ? m_ssrPass->GetResultSrv() : 0);
        }
    }

    // CPU animation systems write SkinningOutputComponent byte offsets for BuildSkinJobs.
    if (m_skin.IsInitialised())
    {
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

        m_skin.GetPoseRingBuffer().BeginFrame(frame);
        // Rotate ring BEFORE any custom-material packing happens this frame.
        m_customMatCbvRing.BeginFrame(frame);
        m_customMatSrvRing.BeginFrame(frame);
        m_skin.GetVertexRing().BeginFrame(frame);
        m_skin.GetAnimationSystem()->Update(world, dt, animActivePtr);
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
void Renderer::ReloadShaders()
{
    // Standalone passes first — these aren't visited by m_graph.ReloadShaders.
    if (m_shadowPass) m_shadowPass->ReloadShaders(m_gfx);
    // DDGI's RTPSO + relight CS aren't in the render graph either — its own
    // ReloadShaders rebuilds the state object from disk on hot edit.
    if (m_ddgiPass)   m_ddgiPass->ReloadShaders(m_gfx);
    if (m_uiPass)         m_uiPass->ReloadShaders();
    if (m_worldUIPass)    m_worldUIPass->ReloadShaders();

    // PP/SSR/culling passes don't have ReloadShaders overrides yet — base no-op is harmless.

    m_graph.ReloadShaders(m_gfx);
}

// ---------------------------------------------------------------------------
RHI::CommandList Renderer::Render()
{
    EnsureWorkers();

    // ---- Frame bindings (read-only for all workers this frame) -------------
    m_graph.BindConstantBuffer("PerView",     m_perObjectCB);
    m_graph.BindConstantBuffer("LightCB",     m_lightCB);
    if (m_terrainCB.IsValid())
        m_graph.BindConstantBuffer("TerrainParams", m_terrainCB);
    if (m_skyboxPass)
        m_graph.BindConstantBuffer("SkyCB", m_skyboxPass->GetSkyCB());
    if (m_volFogPass)
        m_graph.BindConstantBuffer("VolApplyCB", m_volFogPass->GetApplyCB());
    m_graph.BindBuffer("InstanceBuffer",      m_instanceBuffer);
    m_graph.BindBuffer("MeshDescriptors",     m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer());
    m_graph.BindBuffer("MaterialBuffer",      m_materialBuffer);
    m_graph.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr);
    m_graph.SetDrawList(&m_drawPackets);

    if (m_shadowPass && m_lightingPass)
        m_lightingPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());

    // Bind clustered lighting SRVs to LightingPass.
    if (m_clusterPass && m_lightingPass)
        m_lightingPass->SetClusterSRVs(
            m_clusterPass->GetLightsSRVHandle(),
            m_clusterPass->GetLightIndexSRVHandle(),
            m_clusterPass->GetLightGridSRVHandle());

    // Spot shadow atlas + per-slice VP buffer; consumed by both Lighting and VolumetricFog.
    if (m_spotShadowPass && m_lightingPass)
        m_lightingPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv);
    if (m_spotShadowPass && m_volFogPass)
        m_volFogPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv);

    // Bind NPR ramp texture to LightingPass (cached in BuildRenderScene).
    if (m_lightingPass)
        m_lightingPass->SetRampTexture(m_nprRampTexHandle);

    // Bind MaterialBuffer SRV so the Lighting pass can read per-material NPR params.
    if (m_lightingPass && m_materialBuffer.IsValid())
        m_lightingPass->SetMaterialBuffer(m_gfx.GetBufferSRVGpuHandle(m_materialBuffer));

    // Bind previous frame's XeGTAO SSAO texture to LightingPass (one-frame latency).
    if (m_lightingPass && m_xegtaoPass && m_ssaoEnabled)
        m_lightingPass->SetSSAOHandle(m_xegtaoPass->GetAOSrvHandle());
    else if (m_lightingPass)
        m_lightingPass->SetSSAOHandle(0);

    // Feed per-frame state to the volumetric fog pass.
    if (m_volFogPass && m_lightCBMapped)
    {
        auto* lb = static_cast<LightCB*>(m_lightCBMapped);

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
                m_gfx.GetBufferSRVGpuHandle(m_instanceBuffer),
                m_gfx.GetBufferSRVGpuHandle(m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer()),
                m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr);

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

    // Pre-capture SRV handles on main thread before workers kick (graph won't recompile mid-frame).
    const bool needPostCapture = m_taaPass || m_xegtaoPass;
    const RHI::Texture* taaDepthTex    = needPostCapture ? m_graph.GetPhysicalTexture(m_depthHandle)    : nullptr;
    const RHI::Texture* taaSurfaceTex  = m_taaPass       ? m_graph.GetPhysicalTexture(m_surfaceHandle)  : nullptr;
    const RHI::Texture* taaVelocityTex = m_taaPass       ? m_graph.GetPhysicalTexture(m_velocityHandle) : nullptr;
    const RHI::Texture* normalTex      = m_xegtaoPass    ? m_graph.GetPhysicalTexture(m_normalHandle)   : nullptr;
    const uint64_t      depthSrv       = taaDepthTex    ? m_gfx.GetTextureSRVGpuHandle(*taaDepthTex)    : 0;
    const uint64_t      surfaceSrv     = taaSurfaceTex  ? m_gfx.GetTextureSRVGpuHandle(*taaSurfaceTex)  : 0;
    const uint64_t      velocitySrv    = taaVelocityTex ? m_gfx.GetTextureSRVGpuHandle(*taaVelocityTex) : 0;
    const uint64_t      normalSrv      = normalTex      ? m_gfx.GetTextureSRVGpuHandle(*normalTex)      : 0;

    // ---- Phase 0: Skinning CS — writes SkinnedVertexRing UAVs; trailing UAV barrier for GBuffer.
    if (m_skin.GetSkinningPass() && m_skin.IsInitialised() && !m_skin.GetSkinJobs().empty())
    {
        RHI::CommandList skinCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        skinCL.gfx = &m_gfx;
        m_skin.GetSkinningPass()->Execute(skinCL);
        // skinCL is submitted by EndFrame in allocation order (before GBuffer CLs)
    }

    // ---- Phase 0.5: Particle sim CS (emit+update) before render passes; trailing UAV barrier.
    if (m_particleSimPass && m_particleSystem)
    {
        // Feed mesh-shape sampling; no-op for non-mesh emitters.
        m_particleSimPass->SetMeshDescriptorBinding(
            &m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer(),
            m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr);

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

    // ---- Phase 1: Open shadow CL and build its per-frame context -----------
    RHI::CommandList shadowCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    shadowCL.gfx = &m_gfx;

    RG::RenderContext shadowCtx;
    shadowCtx.SetDimensions(m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());
    shadowCtx.SetDrawList(&m_drawPackets);
    shadowCtx.SetBuffer("InstanceBuffer",  m_instanceBuffer);
    shadowCtx.SetBuffer("MeshDescriptors", m_meshMgr.GetDescriptorHeap().GetMeshDescBuffer());
    // Material buffer + bindless texture table required by Shadow.ps ALPHA_TEST path.
    shadowCtx.SetBuffer("MaterialBuffer",  m_materialBuffer);
    shadowCtx.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr);
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
    if (m_clusterPass)
    {
        RHI::CommandList clusterCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
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
            m_gbufferPass->SetIndirectDraw(&m_indirectArgUpload, m_indirectGroups);
        else
            m_gbufferPass->ClearIndirectDraw();
    }

    // ---- Phase 2a.75: GPU Frustum Culling (optional) -------------------------
    if (m_gpuCullingEnabled && m_cullingPass && m_indirectArgBuffer.IsValid())
    {
        RHI::CommandList cullCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        cullCL.gfx = &m_gfx;

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
        if (m_drawCountMapped)
        {
            *static_cast<uint32_t*>(m_drawCountMapped) = 0;
            m_gfx.CopyBuffer(m_drawCountUpload, m_drawCountBuffer, sizeof(uint32_t), cullCL);
        }

        m_cullingPass->Execute(cullCL,
            m_instanceBuffer,
            m_meshMgr.GetDescriptorHeap().GetMeshAABBBuffer(),
            m_indirectArgBuffer,
            m_drawCountBuffer);
    }

    // ---- Phase 2b.5: DDGI (TLAS + trace + relight) — dependency of color graph; atlases SRV-ready.
    // Optional ddgiLogTick logs the failing gate at ~1 Hz for offline troubleshooting.
    RHI::CommandList ddgiCL{};
    static uint32_t s_ddgiLogCounter = 0;
    //const bool ddgiLogTick = (++s_ddgiLogCounter % 60u) == 0;
    const bool ddgiLogTick = false;

    if (m_ddgiReady && m_ddgiPass && m_ddgiMgr.GetActiveVolumeCount() > 0 && m_lastWorld)
    {
        auto& dx12 = static_cast<GraphicsDX12&>(m_gfx);
        ddgiCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        if (ddgiCL.IsValid())
        {
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
                    if (const RHI::GPUBuffer* mb = m_ddgiSceneAS.GetInstanceBuffer())
                        if (ID3D12Resource* mr = dx12.GetBufferResource(*mb))
                            matVA = mr->GetGPUVirtualAddress();

                    // Same bindless table as rasterizer; closest-hit reads vb/ibBindless for face normals.
                    const uint64_t bindlessTable =
                        m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr;

                    // DDGIPass manages atlas SRV↔UAV internally; we leave it SRV for downstream.
                    const uint64_t lightsSrv = m_clusterPass
                        ? m_clusterPass->GetLightsSRVHandle() : 0ull;
                    for (uint32_t s = 0; s < DDGI::kMaxVolumes; ++s)
                    {
                        if (m_ddgiMgr.GetProbeCount(s) == 0) continue;

                        m_ddgiPass->Execute(m_gfx, ddgiCL, m_ddgiMgr, s,
                                            tlasVA, m_skyRadianceSrvForDDGI, matVA,
                                            bindlessTable, lightsSrv);

                        m_ddgiMgr.TransitionVolumeAtlases(m_gfx, ddgiCL, s,
                            DDGI::DDGIVolumeManager::AtlasState::SRV);
                    }
                }
            }
        }
    }

    // ---- Phase 2b: Main-thread color passes (concurrent with Worker 0).
    RHI::CommandList colorLastCL = m_graph.Execute(m_gfx, m_clearColor);

    // DDGI atlas dep — same-queue submission order suffices; invalid CL skips cleanly.
    if (ddgiCL.IsValid() && colorLastCL.IsValid())
        m_gfx.AddCommandListDependency(colorLastCL, ddgiCL);
    else if (ddgiLogTick)
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

            if (colorLastCL.IsValid())
                m_gfx.AddCommandListDependency(hizCL, colorLastCL);
        }
    }

    // ---- Phase 4.6: Hi-Z SSR chain (depth pyramid → trace → snapshot → resolve → temporal → upsample).
    // See SSR_HiZ_Architecture_Prompt.md. Output feeds LightingPass (1-frame latent) + composite.
    if (m_ssrEnabled && m_ssrPass && m_ssrResolvePass && m_ssrDepthHierPass)
    {
        const RHI::Texture* depthTex    = m_graph.GetPhysicalTexture(m_depthHandle);
        const RHI::Texture* normalTex   = m_graph.GetPhysicalTexture(m_normalHandle);
        const RHI::Texture* surfaceTex  = m_graph.GetPhysicalTexture(m_surfaceHandle);
        const RHI::Texture* velocityTex = m_graph.GetPhysicalTexture(m_velocityHandle);
        if (depthTex && normalTex && surfaceTex)
        {
            const uint32_t rw = m_gfx.GetRenderWidth();
            const uint32_t rh = m_gfx.GetRenderHeight();

            m_ssrPass->EnsureTexture(rw, rh);
            m_ssrDepthHierPass->EnsureTexture(rw, rh);
            m_ssrResolvePass->EnsureTextures(rw, rh);

            // Jittered viewProj — matches depth rasterization + LightingPass reconstruct.
            SSRPass::Camera cam{};
            cam.viewProj = m_view.viewProjMatrix;
            {
                using namespace DirectX;
                XMMATRIX vp = XMLoadFloat4x4(&cam.viewProj);
                XMStoreFloat4x4(&cam.invViewProj, XMMatrixInverse(nullptr, vp));
            }
            cam.cameraPos = m_camera.position;
            cam.nearZ     = m_camera.nearZ;
            cam.farZ      = m_camera.farZ;
            m_ssrPass->SetCamera(cam);
            m_ssrPass->SetFrameIndex(m_ssrFrameIndex++);
            {
                // Pyramid mip count = 1 + floor(log2(max(w,h))).
                uint32_t mip = 1, dim = (rw > rh) ? rw : rh;
                while (dim > 1) { dim >>= 1; mip++; }
                m_ssrPass->SetHiZMipCount(mip);
            }
            // Trace tunables live in SSRPass class members; editor SSR Debug window owns them at runtime.

            SSRResolvePass::Camera rcam{};
            rcam.invViewProj = cam.invViewProj;
            rcam.cameraPos   = cam.cameraPos;
            rcam.nearZ       = cam.nearZ;
            rcam.farZ        = cam.farZ;
            m_ssrResolvePass->SetCamera(rcam);
            m_ssrResolvePass->SetFrameIndex(m_ssrFrameIndex);
            // FireflyCap owned by SSRResolvePass class member + editor (see SSRPass comment above).

            SSRTemporalPass::Camera tcam{};
            // Both ends JITTERED — depth was rasterized at jittered NDC, so
            // inverse-VP must be jittered to recover the surface world pos;
            // history was rendered at PREV jittered NDC, so the projection
            // must use prev *jittered* VP to land sampling on the correct
            // history pixel grid. Mixing in non-jittered prev VP (the
            // earlier bug) lit history at ~1 px offset every frame, which
            // the EMA dragged into a multi-pixel ghost trail and a static-
            // frame "reflection misalignment".
            tcam.invViewProj  = cam.invViewProj;
            tcam.prevViewProj = m_taaJitter.GetPrevViewProjJittered();
            tcam.nearZ        = cam.nearZ;
            tcam.farZ         = cam.farZ;
            m_ssrTemporalPass->SetCamera(tcam);

            SSRUpsamplePass::Camera ucam{};
            ucam.invViewProj = cam.invViewProj;
            ucam.nearZ       = cam.nearZ;
            ucam.farZ        = cam.farZ;
            m_ssrUpsamplePass->SetCamera(ucam);

            if (m_sceneColorPyramidPass)
                m_sceneColorPyramidPass->EnsureTexture(rw, rh);
            m_ssrTemporalPass->EnsureTextures(rw, rh);
            m_ssrUpsamplePass->EnsureTexture(rw, rh);

            RHI::CommandList ssrCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
            ssrCL.gfx = &m_gfx;
            if (colorLastCL.IsValid())
                m_gfx.AddCommandListDependency(ssrCL, colorLastCL);

            const RHI::ResourceState depthState0    = m_graph.GetTextureState(m_depthHandle);
            const RHI::ResourceState normalState0   = m_graph.GetTextureState(m_normalHandle);
            const RHI::ResourceState surfaceState0  = m_graph.GetTextureState(m_surfaceHandle);
            const RHI::ResourceState velocityState0 = velocityTex
                ? m_graph.GetTextureState(m_velocityHandle)
                : RHI::ResourceState::SHADER_RESOURCE;

            auto toSR = [&](const RHI::Texture* t, RHI::ResourceState from) {
                if (!t) return;
                if (from != RHI::ResourceState::SHADER_RESOURCE)
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        t, from, RHI::ResourceState::SHADER_RESOURCE), ssrCL);
            };
            toSR(depthTex,    depthState0);
            toSR(normalTex,   normalState0);
            toSR(surfaceTex,  surfaceState0);
            toSR(velocityTex, velocityState0);

            // --- 1. Depth pyramid (lives in UAV across its internal chain) ---
            m_ssrDepthHierPass->Execute(ssrCL,
                m_gfx.GetTextureSRVGpuHandle(*depthTex));
            const RHI::Texture* hierTex = m_ssrDepthHierPass->GetTexture();
            m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                hierTex, RHI::ResourceState::UNORDERED_ACCESS,
                RHI::ResourceState::SHADER_RESOURCE), ssrCL);

            // --- 2. HDR snapshot + scene-color pyramid (BEFORE trace; trace samples pyramid).
            const RHI::Texture* snapTex = m_ssrResolvePass->GetSnapshotTexture();
            m_gfx.SetHdrTextureState(RHI::ResourceState::COPY_SRC, ssrCL);
            m_ssrResolvePass->PreSnapshotCopy(ssrCL);
            m_gfx.CopyHdrSceneTo(*snapTex, ssrCL);
            m_ssrResolvePass->PostSnapshotCopy(ssrCL);

            // Scene-color pyramid: mip 0 = snapshot; mips 1..N Karis-firefly 2×2 reduce.
            // Trace samples cone-footprint mip (roughness × ray len); mirrors stay on mip 0.
            uint64_t pyramidSrv = m_ssrResolvePass->GetSnapshotSrv();   // fallback
            if (m_sceneColorPyramidPass)
            {
                m_sceneColorPyramidPass->Execute(ssrCL,
                    m_ssrResolvePass->GetSnapshotTexture());
                const RHI::Texture* pyrTex = m_sceneColorPyramidPass->GetTexture();
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    pyrTex, RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), ssrCL);
                pyramidSrv = m_sceneColorPyramidPass->GetSrvHandle();
            }

            // --- 3. Trace: write hit colour + rayDirPDF + rayLength -------
            m_ssrPass->Execute(ssrCL,
                m_gfx.GetTextureSRVGpuHandle(*normalTex),
                m_gfx.GetTextureSRVGpuHandle(*surfaceTex),
                m_gfx.GetTextureSRVGpuHandle(*depthTex),
                m_ssrDepthHierPass->GetSrvHandle(),
                pyramidSrv,
                velocityTex ? m_gfx.GetTextureSRVGpuHandle(*velocityTex) : 0);

            // All three trace outputs UAV → SR for resolve to read.
            auto uavToSR = [&](const RHI::Texture* t) {
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    t, RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), ssrCL);
            };
            uavToSR(m_ssrPass->GetResultTexture());
            uavToSR(m_ssrPass->GetRayDirPDFTexture());
            uavToSR(m_ssrPass->GetRayLengthTexture());
            m_ssrPass->SetAllOutputsState(RHI::ResourceState::SHADER_RESOURCE);

            // --- 4. Resolve (spatial BRDF reweight only) ------------------
            m_ssrResolvePass->Execute(ssrCL, rw, rh,
                m_gfx.GetTextureSRVGpuHandle(*normalTex),
                m_gfx.GetTextureSRVGpuHandle(*surfaceTex),
                m_gfx.GetTextureSRVGpuHandle(*depthTex),
                m_ssrPass->GetResultSrv(),
                m_ssrPass->GetRayDirPDFSrv(),
                m_ssrPass->GetRayLengthSrv());

            // --- 5. Temporal (dual-reprojection history blend) ------------
            m_ssrTemporalPass->Execute(ssrCL, rw, rh,
                m_ssrResolvePass->GetColorSrv(),
                m_ssrResolvePass->GetVarianceSrv(),
                m_ssrResolvePass->GetReprojDepthSrv(),
                velocityTex ? m_gfx.GetTextureSRVGpuHandle(*velocityTex) : 0,
                m_gfx.GetTextureSRVGpuHandle(*depthTex));

            // --- 6. Upsample (variance-driven bilateral blur) -------------
            m_ssrUpsamplePass->Execute(ssrCL, rw, rh,
                m_ssrTemporalPass->GetColorSrv(),
                m_ssrTemporalPass->GetVarianceSrv(),
                m_gfx.GetTextureSRVGpuHandle(*depthTex),
                m_gfx.GetTextureSRVGpuHandle(*normalTex),
                m_gfx.GetTextureSRVGpuHandle(*surfaceTex));

            // Return scene-color pyramid to UAV for next frame's mip0 write.
            if (m_sceneColorPyramidPass)
            {
                const RHI::Texture* pyrTex = m_sceneColorPyramidPass->GetTexture();
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    pyrTex, RHI::ResourceState::SHADER_RESOURCE,
                    RHI::ResourceState::UNORDERED_ACCESS), ssrCL);
            }

            // Return the depth pyramid to UAV for next frame's mip0 dispatch.
            m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                hierTex, RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::UNORDERED_ACCESS), ssrCL);

            // HDR → RT so Phase 4.7 composite can flip it to UAV.
            m_gfx.SetHdrTextureState(RHI::ResourceState::RENDERTARGET, ssrCL);

            // Restore graph-tracked states so downstream passes see what they expect.
            auto fromSR = [&](const RHI::Texture* t, RHI::ResourceState to) {
                if (!t) return;
                if (to != RHI::ResourceState::SHADER_RESOURCE)
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        t, RHI::ResourceState::SHADER_RESOURCE, to), ssrCL);
            };
            fromSR(depthTex,    depthState0);
            fromSR(normalTex,   normalState0);
            fromSR(surfaceTex,  surfaceState0);
            fromSR(velocityTex, velocityState0);
        }
    }

    // ---- Phase 4.7: SSR composite -----------------------------------------
    // Resolved SSR + Fresnel × envBRDF → HDR additive; pairs with Lighting (1-ssrConf) dampening.
    if (m_ssrEnabled && m_ssrUpsamplePass && m_ssrCompositePass)
    {
        const RHI::Texture* albedoTex  = m_graph.GetPhysicalTexture(m_albedoHandle);
        const RHI::Texture* normalTex  = m_graph.GetPhysicalTexture(m_normalHandle);
        const RHI::Texture* surfaceTex = m_graph.GetPhysicalTexture(m_surfaceHandle);
        const RHI::Texture* depthTex   = m_graph.GetPhysicalTexture(m_depthHandle);
        const RHI::Texture* ssrTex     = m_ssrUpsamplePass->GetColorTexture();
        const uint64_t      hdrUav     = m_gfx.GetHdrSceneUavGpuHandle();
        if (albedoTex && normalTex && surfaceTex && depthTex && ssrTex && hdrUav)
        {
            SSRCompositePass::Camera cam{};
            cam.cameraPos = m_camera.position;
            {
                using namespace DirectX;
                XMMATRIX vp = XMLoadFloat4x4(&m_view.viewProjMatrix);
                XMStoreFloat4x4(&cam.invViewProj, XMMatrixInverse(nullptr, vp));
            }
            m_ssrCompositePass->SetCamera(cam);

            RHI::CommandList compCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
            compCL.gfx = &m_gfx;
            if (colorLastCL.IsValid())
                m_gfx.AddCommandListDependency(compCL, colorLastCL);

            const RHI::ResourceState albedoState0  = m_graph.GetTextureState(m_albedoHandle);
            const RHI::ResourceState normalState0  = m_graph.GetTextureState(m_normalHandle);
            const RHI::ResourceState surfaceState0 = m_graph.GetTextureState(m_surfaceHandle);
            const RHI::ResourceState depthState0   = m_graph.GetTextureState(m_depthHandle);
            auto toSR = [&](const RHI::Texture* t, RHI::ResourceState from) {
                if (from != RHI::ResourceState::SHADER_RESOURCE)
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        t, from, RHI::ResourceState::SHADER_RESOURCE), compCL);
            };
            toSR(albedoTex,  albedoState0);
            toSR(normalTex,  normalState0);
            toSR(surfaceTex, surfaceState0);
            toSR(depthTex,   depthState0);

            m_gfx.SetHdrTextureState(RHI::ResourceState::UNORDERED_ACCESS, compCL);

            // Debug SRVs for composite modes 3/4/5 (raw trace / confidence / world-L). Already in SR.
            const uint64_t rawTraceSrv = m_ssrPass ? m_ssrPass->GetResultSrv()    : 0;
            const uint64_t rayDirSrv   = m_ssrPass ? m_ssrPass->GetRayDirPDFSrv() : 0;

            // Keep composite's roughness-mask mode in sync with trace's cutoff.
            if (m_ssrPass && m_ssrCompositePass)
                m_ssrCompositePass->SetRoughnessCutoff(m_ssrPass->GetRoughnessCutoff());

            m_ssrCompositePass->Execute(compCL,
                m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight(),
                m_gfx.GetTextureSRVGpuHandle(*albedoTex),
                m_gfx.GetTextureSRVGpuHandle(*normalTex),
                m_gfx.GetTextureSRVGpuHandle(*surfaceTex),
                m_gfx.GetTextureSRVGpuHandle(*depthTex),
                m_gfx.GetTextureSRVGpuHandle(*ssrTex),
                hdrUav,
                m_brdfLutSrv,
                rawTraceSrv, rayDirSrv, /*rayLen*/0, /*variance*/0);

            m_gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE, compCL);

            auto fromSR = [&](const RHI::Texture* t, RHI::ResourceState to) {
                if (to != RHI::ResourceState::SHADER_RESOURCE)
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        t, RHI::ResourceState::SHADER_RESOURCE, to), compCL);
            };
            fromSR(albedoTex,  albedoState0);
            fromSR(normalTex,  normalState0);
            fromSR(surfaceTex, surfaceState0);
            fromSR(depthTex,   depthState0);
        }
    }

    // ---- Phase 4.8: Debug wireframe — MUST run after SSR composite or it gets stomped.
    if (m_debugWirePass && m_debugWirePass->enabled && colorLastCL.IsValid())
    {
        RHI::CommandList wireCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
        wireCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(wireCL, colorLastCL);

        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        m_debugWirePass->Execute(wireCL, depthTex, m_perObjectCB);

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

        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        m_ddgiProbeDebugPass->Execute(m_gfx, dbgCL, depthTex,
                                      m_perObjectCB, m_ddgiMgr, *m_lastWorld);

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

        // computeCL (COMPUTE): TAA → AutoExposure → Bloom → ToneMap; cross-queue fenced.
        RHI::CommandList computeCL = m_gfx.BeginCommandList(RHI::QUEUE_TYPE::COMPUTE);
        computeCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(computeCL, preComputeCL);

        // Worker 2 records the compute dispatches while main thread can do other work.
        m_renderWorkers[2].Kick([this, computeCL, depthSrv, surfaceSrv, velocitySrv, normalSrv]() mutable
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
                m_taaPass->SetViewportSize(m_vpWidth, m_vpHeight);
                m_taaPass->SetFrameData(m_taaJitter.GetInvViewProj(), prevVP,
                                        m_taaPass->tauHistory,
                                        /*hasHistory=*/true, m_deltaTime);
                m_taaPass->SetJitter(m_taaJitter.GetJitterX(), m_taaJitter.GetJitterY());
                m_taaPass->Execute(computeCL);
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
                m_xegtaoPass->Execute(computeCL);
            }

            // Route to TAA's resolved buffer only if TAA actually ran; raw HDR otherwise.
            const uint64_t resolvedSrv = (m_taaPass && m_taaPass->IsEnabled())
                ? m_taaPass->GetResolvedSrvHandle()
                : m_gfx.GetHdrSceneSrvGpuHandle();

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
                if (m_lightCBMapped)
                {
                    auto* lb = static_cast<LightCB*>(m_lightCBMapped);
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
                //   - TOD active and SkyIBLPass switched the active body to the
                //     moon → LightCB.lightDir already tracks the moon, so the
                //     dir.y check below would *not* fire (moonDir.y > 0 at
                //     night). Use IsMoonActive() to catch this case.
                //   - TOD off / no SkyIBLPass → directional light is whatever
                //     the user authored. Gate on sunDirWS.y so a manually
                //     down-pointing sun also turns flare off.
                bool sunBelowHorizon = false;
                if (m_skyIBLPass && m_skyIBLPass->IsTimeOfDayEnabled()
                                 && m_skyIBLPass->IsMoonActive())
                    sunBelowHorizon = true;
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
                m_postProcessStack->Execute(ppCtx);
            }

            // GlassShatterPass: composites shards in-place into Tonemap output; restores entry state.
            if (m_glassShatterPass && m_glassShatterPass->IsActive() && m_toneMapPass)
            {
                m_glassShatterPass->Execute(
                    computeCL,
                    m_toneMapPass->GetFinalOutputTexture(),
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                    m_vpWidth, m_vpHeight,
                    m_deltaTime);
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
            m_worldUIPass->Execute(restoreCL,
                                    *m_lastWorld,
                                    m_view.viewProjMatrixNoJitter,
                                    m_view.viewMatrix,
                                    m_toneMapPass->GetFinalOutputTexture(),
                                    entry,
                                    m_vpWidth, m_vpHeight);
            m_toneMapPass->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
        }

        if (m_uiPass && m_uiPass->enabled && m_toneMapPass
            && !m_uiPass->GetDrawList().IsEmpty())
        {
            // Pass actual tracked state so the barrier matches (resize paths can leave UAV/SR_COMPUTE).
            const RHI::ResourceState entry = m_toneMapPass->GetFinalOutputState();
            m_uiPass->Execute(restoreCL,
                              m_toneMapPass->GetFinalOutputTexture(),
                              entry,
                              m_vpWidth, m_vpHeight);
            m_toneMapPass->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
        }
        // Always clear drawlist — widgets re-emit each frame; prevents disabled-UI accumulation.
        if (m_uiPass) m_uiPass->GetDrawList().Clear();

        return restoreCL;
    }

    return colorLastCL.IsValid() ? colorLastCL : shadowCL;
}

uint64_t Renderer::GetFinalOutputSrvHandle() const
{
    if (m_toneMapPass) return m_toneMapPass->GetFinalOutputSrvHandle();
    return 0;
}

void Renderer::TriggerGlassShatter(float impactU, float impactV)
{
    if (m_glassShatterPass) m_glassShatterPass->Trigger(impactU, impactV);
}

uint64_t Renderer::GetSSRResultSrv() const
{
    return m_ssrPass ? m_ssrPass->GetResultSrv() : 0;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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

void Renderer::SyncMaterialTextures(Entity e, MaterialComponent& mc)
{
    if (!m_texSys || !m_resMgr) return;

    // try_emplace: one lookup whether hit or miss (avoids find-then-insert two-hash).
    auto [cacheIt, _inserted] = m_matTexCache.try_emplace(e);
    auto& cache = cacheIt->second;
    for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
    {
        auto& slot  = mc.textures[s];
        auto& entry = cache[s];

        // Re-acquire when the path changes (or is first seen).
        if (slot.name != entry.path)
        {
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, m_gfx);

            entry.path   = slot.name;
            entry.handle = slot.name.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(slot.name, *m_resMgr, m_gfx);
            slot.gpuHandle = 0; // clear until load completes
        }

        // Promote once async load finishes; gpuHandle==0 gate skips virtuals on subsequent frames.
        if (slot.gpuHandle == 0
            && entry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(entry.handle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
            {
                slot.gpuHandle        = m_gfx.GetTextureSRVGpuHandle(*tex);
                slot.bindlessIndex    = static_cast<int32_t>(tex->handle_id);
                slot.previewGpuHandle = m_gfx.GetTexturePreviewSrvGpuHandle(*tex);
            }
        }
    }
}

// Name-keyed mirror of SyncMaterialTextures for the customTextures map.
void Renderer::SyncMaterialCustomTextures(Entity e, MaterialComponent& mc)
{
    if (!m_texSys || !m_resMgr) return;
    if (mc.customTextures.empty())
    {
        // Free entries left over from a previous shader's textures.
        auto it = m_customMatTexCache.find(e);
        if (it != m_customMatTexCache.end())
        {
            for (auto& [_name, entry] : it->second)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, m_gfx);
            m_customMatTexCache.erase(it);
        }
        return;
    }

    auto [cacheIt, _inserted] = m_customMatTexCache.try_emplace(e);
    auto& cache = cacheIt->second;

    for (auto& [name, slot] : mc.customTextures)
    {
        auto [entIt, _added] = cache.try_emplace(name);
        auto& entry = entIt->second;

        if (slot.name != entry.path)
        {
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, m_gfx);

            entry.path   = slot.name;
            entry.handle = slot.name.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(slot.name, *m_resMgr, m_gfx);
            slot.gpuHandle     = 0;
            slot.bindlessIndex = -1;
        }

        if (slot.gpuHandle == 0
            && entry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(entry.handle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
            {
                slot.gpuHandle        = m_gfx.GetTextureSRVGpuHandle(*tex);
                slot.bindlessIndex    = static_cast<int32_t>(tex->handle_id);
                slot.previewGpuHandle = m_gfx.GetTexturePreviewSrvGpuHandle(*tex);
            }
        }
    }
}

// ---------------------------------------------------------------------------
void Renderer::BuildRenderScene(World& world)
{
    m_drawPackets.clear();

    // Cache ECS pool pointers once — pool->Get(e) is two array loads vs hash lookup.
    auto* pMaterial    = world.GetPool<MaterialComponent>();
    auto* pGlobalXf    = world.GetPool<GlobalTransform>();
    auto* pWorldAabb   = world.GetPool<WorldAabb>();
    auto* pMeshHandle  = world.GetPool<MeshHandle>();
    auto* pMeshLibRef  = world.GetPool<MeshLibRef>();
    auto* pLightData   = world.GetPool<LightData>();
    auto* pVolLight    = world.GetPool<VolumetricLightComponent>();
    auto* pBillboard   = world.GetPool<BillboardComponent>();
    auto* pSkinned     = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut     = world.GetPool<SkinningOutputComponent>();
    auto* pVisibility  = world.GetPool<Visibility>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };

    // Current-frame camera forward for transparent sort (m_view is updated AFTER this function).
    const float _cp = std::cos(m_camera.pitch);
    const float _sp = std::sin(m_camera.pitch);
    const float _cy = std::cos(m_camera.yaw);
    const float _sy = std::sin(m_camera.yaw);
    const XMFLOAT3 camForwardThisFrame = { _sy * _cp, -_sp, _cy * _cp };

    // ---- Sync material textures + NPR ramp scan (merged single pass) --------
    m_nprRampTexHandle = 0;
    if (m_texSys && m_resMgr && pMaterial)
    {
        auto& matEnts = pMaterial->Entities();
        auto& matData = pMaterial->Data();
        const size_t matN = matData.size();
        for (size_t mi = 0; mi < matN; ++mi)
        {
            const Entity e = matEnts[mi];
            if (!world.IsAlive(e)) continue;
            MaterialComponent* mc = &matData[mi];

            // NPR ramp scan (piggyback on material iteration).
            if (m_nprRampTexHandle == 0
                && mc->shaderType == MaterialComponent::SHADERTYPE_NPR_RAMP
                && mc->textures[MaterialComponent::RAMPMAP].gpuHandle != 0)
            {
                m_nprRampTexHandle = mc->textures[MaterialComponent::RAMPMAP].gpuHandle;
            }

            // Fast skip: clean + all slots GPU-ready (or empty) → no sync work.
            if (!mc->IsDirty())
            {
                bool allReady = true;
                for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
                {
                    if (!mc->textures[s].name.empty() && mc->textures[s].gpuHandle == 0)
                    { allReady = false; break; }
                }
                if (allReady) continue;
            }

            SyncMaterialTextures(e, *mc);
            SyncMaterialCustomTextures(e, *mc);
            mc->SetDirty(false);
        }
    }

    // ---- Phase 0.5: BVH + frustum cull. Dense bitmasks (no hashing) — bvhVisibleMask /
    // bvhTestedMask indexed by Entity ID; tested = in BVH (has MeshLibRef + WorldAabb).
    Entity maxEntity = 0;
    for (Entity e : world.GetEntities())
        if (e > maxEntity) maxEntity = e;
    std::vector<uint8_t> bvhVisibleMask(static_cast<size_t>(maxEntity) + 1, 0);
    std::vector<uint8_t> bvhTestedMask (static_cast<size_t>(maxEntity) + 1, 0);
    auto inMask = [](const std::vector<uint8_t>& m, Entity e) -> bool {
        return e < m.size() && m[e] != 0;
    };
    if (m_gpuCullingEnabled)
    {
        std::vector<Entity>              bvhEntities;
        std::vector<SceneBVH::AABB>      bvhAABBs;
        std::vector<SceneBVH::LeafType>  bvhTypes;
        bvhEntities.reserve(1024);
        bvhAABBs.reserve(1024);
        bvhTypes.reserve(1024);

        if (pMeshLibRef)
        {
            auto& mlrEnts = pMeshLibRef->Entities();
            auto& mlrData = pMeshLibRef->Data();
            const size_t mlrN = mlrData.size();
            for (size_t k = 0; k < mlrN; ++k)
            {
                const Entity e = mlrEnts[k];
                if (!world.IsAlive(e)) continue;
                if (!mlrData[k].IsValid()) continue;
                const WorldAabb* aabb = pGet(pWorldAabb, e);
                if (!aabb) continue;

                bvhEntities.push_back(e);
                bvhAABBs.push_back({ aabb->min, aabb->max });

                const MeshSkinnedComponent* skMesh = pGet(pSkinned, e);
                bvhTypes.push_back(skMesh ? SceneBVH::LeafType::Skinned
                                          : SceneBVH::LeafType::Static);
            }
        }

        const uint32_t entityCount = static_cast<uint32_t>(bvhEntities.size());

        if (m_sceneBVH.NeedsRebuild() || m_sceneBVH.GetLeafCount() != entityCount)
        {
            // Full SAH rebuild — first frame or entity count changed.
            m_sceneBVH.Build(bvhEntities.data(), bvhAABBs.data(), bvhTypes.data(), entityCount);
        }
        else
        {
            // Incremental refit: leaf AABBs then bottom-up propagate.
            for (uint32_t i = 0; i < entityCount; ++i)
                m_sceneBVH.RefitLeaf(bvhEntities[i], bvhAABBs[i]);
            m_sceneBVH.RefitInternal();
        }

        std::vector<Entity> visibleEntities;
        visibleEntities.reserve(entityCount);
        m_sceneBVH.FrustumCull(m_view.boundingFrustum, visibleEntities);

        for (Entity e : visibleEntities) if (e < bvhVisibleMask.size()) bvhVisibleMask[e] = 1;
        for (Entity e : bvhEntities)     if (e < bvhTestedMask .size()) bvhTestedMask [e] = 1;
    }

    // ---- Debug wireframes (NOT gated on culling — needs Clear() each frame regardless). ----
    if (m_debugWirePass && m_debugWirePass->enabled)
    {
        m_debugWirePass->Clear();

        if (m_debugWirePass->showAABBs)
        {
            for (Entity e : world.GetEntities())
            {
                if (!world.IsAlive(e)) continue;
                const WorldAabb* wa = world.GetComponent<WorldAabb>(e);
                if (wa && wa->min.x <= wa->max.x)
                    m_debugWirePass->AddAABB(wa->min, wa->max, 0xFF00FF00); // green
            }
        }

        if (m_debugWirePass->showFrustum)
        {
            // Compute frustum corners from inverse viewProj.
            XMMATRIX invVP = XMLoadFloat4x4(&m_view.viewProjMatrixNoJitter);
            invVP = XMMatrixInverse(nullptr, invVP);

            XMFLOAT3 frustumCorners[8];
            static const XMFLOAT3 ndcCorners[8] = {
                {-1, -1, 0}, { 1, -1, 0}, {-1,  1, 0}, { 1,  1, 0}, // near
                {-1, -1, 1}, { 1, -1, 1}, {-1,  1, 1}, { 1,  1, 1}, // far
            };
            for (int i = 0; i < 8; ++i)
            {
                XMVECTOR v = XMVector3TransformCoord(
                    XMLoadFloat3(&ndcCorners[i]), invVP);
                XMStoreFloat3(&frustumCorners[i], v);
            }
            m_debugWirePass->AddFrustum(frustumCorners, 0xFF00FFFF); // cyan
        }

        if (m_debugWirePass->showCapsules)
        {
            for (Entity e : world.GetEntities())
            {
                if (!world.IsAlive(e)) continue;
                const auto* capComp = world.GetComponent<CapsuleColliderComponent>(e);
                if (!capComp || capComp->count == 0) continue;

                // Find skeleton — on this entity or via SkeletonRef.
                Entity skelEntity = e;
                const auto* skelComp = world.GetComponent<SkeletonComponent>(e);
                if (!skelComp)
                {
                    const SkeletonRef* ref = world.GetComponent<SkeletonRef>(e);
                    if (ref && ref->entity != NullEntity)
                    {
                        skelEntity = ref->entity;
                        skelComp = world.GetComponent<SkeletonComponent>(skelEntity);
                    }
                }
                if (!skelComp || skelComp->assetIndex == kInvalidSkeletonIndex) continue;

                const SkeletonAsset& skel = m_skin.GetSkeletonRegistry().Get(skelComp->assetIndex);
                AnimationSystem::LocalPose* poses = m_skin.GetAnimationSystem()
                    ? m_skin.GetAnimationSystem()->GetMutableLocalPose(skelEntity) : nullptr;
                if (!poses) continue;

                // Skeleton entity's world transform (skeleton space → world space).
                const GlobalTransform* gt = world.GetComponent<GlobalTransform>(skelEntity);
                XMMATRIX entityWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();

                // Compute bone world matrix: local pose chain × entity transform.
                auto boneWorldMat = [&](uint32_t bi) -> XMMATRIX {
                    if (bi >= skel.boneCount) return XMMatrixIdentity();
                    uint32_t chain[SkeletonAsset::MAX_BONES];
                    uint32_t depth = 0;
                    int32_t cur = static_cast<int32_t>(bi);
                    while (cur >= 0 && depth < skel.boneCount)
                    { chain[depth++] = static_cast<uint32_t>(cur); cur = skel.parentIndex[cur]; }
                    XMMATRIX w = XMMatrixIdentity();
                    for (uint32_t d = depth; d > 0; --d)
                    {
                        auto& p = poses[chain[d-1]];
                        XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
                        XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
                        XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
                        w = S * R * T * w;
                    }
                    return XMMatrixMultiply(w, entityWorld);
                };

                for (int ci = 0; ci < capComp->count; ++ci)
                {
                    const auto& def = capComp->capsules[ci];
                    if (!def.enabled) continue;
                    XMMATRIX wA = boneWorldMat(def.boneA);
                    XMMATRIX wB = boneWorldMat(def.boneB);
                    XMVECTOR posA = XMVectorAdd(wA.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetA), wA));
                    XMVECTOR posB = XMVectorAdd(wB.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetB), wB));
                    XMFLOAT3 wa, wb;
                    XMStoreFloat3(&wa, posA);
                    XMStoreFloat3(&wb, posB);
                    m_debugWirePass->AddCapsule(wa, wb, def.radius, 0xFFFF8800);
                }
            }
        }

        if (m_debugWirePass->showReflectionProbes)
        {
            // Iterate the probe pool directly (see memory: ECS pool iteration).
            auto* probePool = world.GetPool<ReflectionProbeComponent>();
            if (probePool)
            {
                const auto& ents = probePool->Entities();
                auto&       data = probePool->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    Entity e = ents[i];
                    if (!world.IsAlive(e)) continue;
                    const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                    if (!gt) continue;

                    const ReflectionProbeComponent& comp = data[i];
                    const XMFLOAT3 pos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };

                    // Match renderer upload clamp: outer >= inner per axis.
                    XMFLOAT3 inner = comp.innerExtents;
                    XMFLOAT3 outer = {
                        std::max(comp.outerExtents.x, inner.x),
                        std::max(comp.outerExtents.y, inner.y),
                        std::max(comp.outerExtents.z, inner.z),
                    };

                    const XMFLOAT3 innerMin = { pos.x - inner.x, pos.y - inner.y, pos.z - inner.z };
                    const XMFLOAT3 innerMax = { pos.x + inner.x, pos.y + inner.y, pos.z + inner.z };
                    const XMFLOAT3 outerMin = { pos.x - outer.x, pos.y - outer.y, pos.z - outer.z };
                    const XMFLOAT3 outerMax = { pos.x + outer.x, pos.y + outer.y, pos.z + outer.z };

                    // Inner=magenta full-influence, outer=dim-purple falloff (0xAARRGGBB).
                    m_debugWirePass->AddAABB(innerMin, innerMax, 0xFFFF00FF);
                    m_debugWirePass->AddAABB(outerMin, outerMax, 0xFF802080);
                }
            }
        }

        // ---- DDGI volumes + probe grid ----
        // Yellow volume AABB + green cross per probe — verify placement/spacing visually.
        if (m_debugWirePass->showDDGIVolumes)
        {
            auto* volPool = world.GetPool<DDGIVolumeComponent>();
            if (volPool)
            {
                const auto& ents = volPool->Entities();
                auto&       data = volPool->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    if (!world.IsAlive(ents[i])) continue;
                    const DDGIVolumeComponent& v = data[i];

                    // Volume AABB — yellow.
                    XMFLOAT3 mn{ v.origin.x - v.extent.x,
                                 v.origin.y - v.extent.y,
                                 v.origin.z - v.extent.z };
                    XMFLOAT3 mx{ v.origin.x + v.extent.x,
                                 v.origin.y + v.extent.y,
                                 v.origin.z + v.extent.z };
                    m_debugWirePass->AddAABB(mn, mx, 0xFF00FFFF);

                    // Per-probe crosses gated on debugDraw flag (off by default).
                    if (!v.debugDraw) continue;
                    const float spacingX = (v.probeCountsX > 1)
                        ? (2.0f * v.extent.x) / float(v.probeCountsX - 1) : 0.0f;
                    const float spacingY = (v.probeCountsY > 1)
                        ? (2.0f * v.extent.y) / float(v.probeCountsY - 1) : 0.0f;
                    const float spacingZ = (v.probeCountsZ > 1)
                        ? (2.0f * v.extent.z) / float(v.probeCountsZ - 1) : 0.0f;
                    const float crossSize = std::min({ spacingX, spacingY, spacingZ }) * 0.18f;
                    for (uint32_t z = 0; z < v.probeCountsZ; ++z)
                    for (uint32_t y = 0; y < v.probeCountsY; ++y)
                    for (uint32_t x = 0; x < v.probeCountsX; ++x)
                    {
                        XMFLOAT3 p{
                            mn.x + spacingX * float(x),
                            mn.y + spacingY * float(y),
                            mn.z + spacingZ * float(z) };
                        // RGB-encoded probe coord (R = +x, G = +y, B = +z).
                        const uint8_t r = uint8_t(255.0f * float(x) / std::max(1u, v.probeCountsX - 1));
                        const uint8_t g = uint8_t(255.0f * float(y) / std::max(1u, v.probeCountsY - 1));
                        const uint8_t b = uint8_t(255.0f * float(z) / std::max(1u, v.probeCountsZ - 1));
                        const uint32_t color = 0xFF000000u | (r) | (g << 8) | (b << 16);
                        m_debugWirePass->AddCross(p, crossSize, color);
                    }
                }
            }
        }
    }

    // ---- Phase 1: Collect draw candidates ----------------------------------
    struct DrawCandidate
    {
        Entity     entity;
        uint32_t   meshDescSlot;
        uint32_t   indexCount;
        XMFLOAT4X4 worldMatrix;
        const MaterialComponent* mc;
        uint64_t       texBaseColor  = 0;
        uint64_t       texSurfaceMap = 0;
        uint64_t       texNormalMap  = 0;
        uint32_t       matHash       = 0;
        DrawFilter     filter        = DrawFilter::Opaque;
        PermutationKey perm          = {};
        // World-space depth-sort centroid; MeshLibRef overrides w/ WorldAabb centre to fix off-pivot meshes.
        XMFLOAT3       worldCenter   = { 0.0f, 0.0f, 0.0f };
        float          depth              = 0.0f;  // view-space Z (transparent painter's-algorithm sort)
        float          outlinePixels      = 2.0f;  // per-material outline width (Custom packets only)
        bool           screenSpaceOutline = true;  // enable screen-space edge detection
        uint8_t        stencilRef         = 1;     // 1=PBR, 2=NPR
        uint8_t        shadowCullMode     = 0;     // ShadowCullMode raw; 0=Default back-cull
        uint8_t        castShadow         = 1;     // 0 = material opted out of ShadowPass
        uint32_t       prevPosElementBase = 0xFFFFFFFFu; // TAA: skinned prev pos (0xFFFFFFFF = static)
        uint32_t       customPSID         = 0;     // 0 = default GBuffer PS; non-zero = dynamic id
    };

    // Helper: compute filter + permutation + stencil ref from a MaterialComponent.
    auto applyBlendMode = [&](DrawCandidate& c)
    {
        if (!c.mc) return;
        // Stencil ref: 1=PBR (default), 2=NPR (texture ramp or color ramp), 3=Unlit
        switch (c.mc->shaderType)
        {
        case MaterialComponent::SHADERTYPE_NPR_RAMP:
        case MaterialComponent::SHADERTYPE_NPR_COLOR:
            c.stencilRef = 2; break;
        case MaterialComponent::SHADERTYPE_UNLIT:
            c.stencilRef = 3; break;
        default:
            c.stencilRef = 1; break;
        }
        c.perm.Set(PermutationKey::HAS_NORMALMAP,
                   c.mc->textures[MaterialComponent::NORMALMAP].gpuHandle != 0);
        c.perm.Set(PermutationKey::HAS_EMISSIVE,
                   c.mc->GetEmissiveStrength() > 0.0f);
        c.perm.Set(PermutationKey::DOUBLE_SIDED,
                   (c.mc->_flags & MaterialComponent::DOUBLE_SIDED) != 0);
        c.shadowCullMode = static_cast<uint8_t>(c.mc->shadowCullMode);
        c.castShadow     = c.mc->IsCastingShadow() ? 1 : 0;

        switch (c.mc->userBlendMode)
        {
        case BlendMode::Alpha:
            c.filter = DrawFilter::Transparent;
            c.perm.Set(PermutationKey::ALPHA_BLEND, true);
            // Alpha-blended foliage with alphaRef set still wants alpha-tested shadow casting.
            if (c.mc->IsAlphaTestEnabled())
                c.perm.Set(PermutationKey::ALPHA_TEST, true);
            break;
        case BlendMode::Additive:
            c.filter = DrawFilter::Transparent;
            c.perm.Set(PermutationKey::ADDITIVE_BLEND, true);
            break;
        case BlendMode::Premultiplied:
            c.filter = DrawFilter::Transparent;
            c.perm.Set(PermutationKey::PREMULTIPLIED_BLEND, true);
            if (c.mc->IsAlphaTestEnabled())
                c.perm.Set(PermutationKey::ALPHA_TEST, true);
            break;
        case BlendMode::Multiply:
            c.filter = DrawFilter::Transparent;
            c.perm.Set(PermutationKey::MULTIPLY_BLEND, true);
            break;
        default: // BlendMode::Opaque
            c.filter = DrawFilter::Opaque;
            // IsAlphaTestEnabled(): alphaRef strict enough to discard (< full-opaque threshold).
            c.perm.Set(PermutationKey::ALPHA_TEST, c.mc->IsAlphaTestEnabled());
            break;
        }

        // View-space Z depth: project worldCenter onto cam forward (NOT Euclidean — produces pops).
        // worldCenter set by builder: MeshLibRef uses WorldAabb centroid; primitive uses pivot.
        {
            const float dx = c.worldCenter.x - m_camera.position.x;
            const float dy = c.worldCenter.y - m_camera.position.y;
            const float dz = c.worldCenter.z - m_camera.position.z;
            c.depth = dx * camForwardThisFrame.x
                    + dy * camForwardThisFrame.y
                    + dz * camForwardThisFrame.z;
        }
    };

    // Lazy-resolve customShaderPath → dynamic PS id; stash in MaterialComponent on first hit.
    // Post-resolve, sync custom-param maps to shader reflection (preserves existing user values).
    auto resolveCustomPSID = [this](const MaterialComponent& m) -> uint32_t
    {
        if (!m.useCustomShader || m.customShaderPath.empty()) return 0;
        if (m.customShaderID < 0 && m_gbufferPass)
        {
            const uint32_t id = m_gbufferPass->GetShaderLibrary().RegisterDynamic(
                m.customShaderPath.c_str(), RHI::ShaderStage::PS);
            m.customShaderID =
                (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);

            if (m.customShaderID > 0)
            {
                if (auto* refl = m_gbufferPass->GetShaderLibrary()
                                     .GetDynamicReflection(static_cast<uint32_t>(m.customShaderID)))
                {
                    // const_cast: cached customTextures/customParams are part of the same lazy-resolve bucket.
                    MaterialReflectionSync::Sync(const_cast<MaterialComponent&>(m), *refl);
                }
            }
        }
        return (m.customShaderID > 0) ? static_cast<uint32_t>(m.customShaderID) : 0;
    };

    std::vector<DrawCandidate> candidates;
    // Adaptive reserve: use last frame's count to avoid reallocation.
    static uint32_t s_lastCandidateCount = 256;
    candidates.reserve(s_lastCandidateCount + 64);

    // m_frameLights / m_volumetricLights: adaptive reserve from last frame's count.
    static uint32_t s_lastFrameLightCount = 32;
    static uint32_t s_lastVolLightCount   = 8;
    m_frameLights.clear();
    m_volumetricLights.clear();
    if (m_frameLights.capacity()      < s_lastFrameLightCount + 8)
        m_frameLights.reserve(s_lastFrameLightCount + 8);
    if (m_volumetricLights.capacity() < s_lastVolLightCount + 4)
        m_volumetricLights.reserve(s_lastVolLightCount + 4);
    m_sunVolumetric      = false;
    m_sunVolumetricScale = 1.0f;
    auto* lb = m_lightCBMapped ? static_cast<LightCB*>(m_lightCBMapped) : nullptr;

    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;

        // ---- Inline light collection (avoids separate full-entity loop) ----
        const LightData* ld = pGet(pLightData, e);
        if (ld)
        {
            const GlobalTransform* lgt = pGet(pGlobalXf, e);
            XMFLOAT3 wPos = { 0,0,0 }, wDir = ld->direction;
            if (lgt)
            {
                wPos = { lgt->matrix._41, lgt->matrix._42, lgt->matrix._43 };
                XMVECTOR rd = XMVector3Normalize(
                    XMVector3TransformNormal(XMLoadFloat3(&ld->direction),
                                             XMLoadFloat4x4(&lgt->matrix)));
                XMStoreFloat3(&wDir, rd);
            }
            if (ld->type == LightType::Directional && lb)
            {
                XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&wDir));
                XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(lb->lightDir), dir);
                lb->lightColor[0] = ld->color.x * ld->intensity;
                lb->lightColor[1] = ld->color.y * ld->intensity;
                lb->lightColor[2] = ld->color.z * ld->intensity;
            }
            m_frameLights.emplace_back(ResolvedLight{
                wPos, ld->radius, ld->color, ld->intensity,
                wDir, ld->spotAngle, ld->type,
                ld->castsShadow, 0xFFFFFFFFu });

            // VolumetricLightComponent opts the light into the fog (sun=CSM god-rays, point/spot=froxel shafts).
            const VolumetricLightComponent* vc = pGet(pVolLight, e);
            const bool volumetric = vc && vc->enabled;

            if (volumetric && ld->type == LightType::Directional)
            {
                // Sun path via SetSun() below — one-shot, FIRST volumetric directional wins.
                m_sunVolumetric        = true;
                m_sunVolumetricScale   = vc->intensityScale;
            }
            else if (volumetric && ld->type != LightType::Directional)
            {
                VolumetricFogPass::VolLight vl{};
                vl.position       = wPos;
                vl.radius         = ld->radius;
                vl.color          = ld->color;
                vl.intensity      = ld->intensity * vc->intensityScale;
                vl.direction      = wDir;
                vl.spotAngle      = ld->spotAngle;
                vl.type           = static_cast<uint32_t>(ld->type);
                vl.shadowSliceIdx = 0xFFFFFFFFu; // filled after slice assignment in UploadLights
                m_volumetricLights.push_back(vl);
            }
        }

        // ---- Primitive mesh entity (MeshHandle + Transform) ----------------
        const MeshHandle* mh = pGet(pMeshHandle, e);
        if (mh && mh->IsValid() &&
            mh->gpuMeshID < static_cast<uint32_t>(PrimitiveMeshType::Count))
        {
            // Honour Visibility (explicit + inherited_hidden propagation from TransformSystem).
            if (const Visibility* v = pGet(pVisibility, e);
                v && !v->IsEffectivelyVisible())
                continue;

            const auto& mesh = m_meshMgr.GetPrimitive(mh->gpuMeshID);
            if (mesh.meshDescSlot == RHI::kInvalidBufferIndex) continue;

            // GlobalTransform is the single source of truth.
            const GlobalTransform* gt = pGet(pGlobalXf, e);
            if (!gt) continue;
            const XMFLOAT4X4 worldMtx = gt->matrix;

            DrawCandidate c;
            c.entity       = e;
            c.meshDescSlot = mesh.meshDescSlot;
            c.indexCount   = mesh.indexCount;
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&worldMtx)));
            // Primitives are unit-sized and centred — pivot == centroid for depth sort.
            c.worldCenter = { worldMtx.m[3][0], worldMtx.m[3][1], worldMtx.m[3][2] };
            c.mc = pGet(pMaterial, e);
            if (c.mc)
            {
                c.texBaseColor  = c.mc->textures[MaterialComponent::BASECOLORMAP].gpuHandle;
                c.texSurfaceMap = c.mc->textures[MaterialComponent::SURFACEMAP].gpuHandle;
                c.texNormalMap  = c.mc->textures[MaterialComponent::NORMALMAP].gpuHandle;
                c.matHash       = HashMatParams(c.mc);
                c.customPSID    = resolveCustomPSID(*c.mc);
            }
            applyBlendMode(c);
            candidates.push_back(c);
            // Custom candidate for outlines: push-then-mutate avoids full DrawCandidate memcpy.
            if (c.mc && c.mc->IsOutlineEnabled() && c.filter == DrawFilter::Opaque)
            {
                const float outlinePixels      = c.mc->outlinePixels;
                const bool  screenSpaceOutline = c.mc->IsOutlineScreenSpaceEnabled();
                candidates.push_back(c);
                DrawCandidate& outline = candidates.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
            }
            continue;
        }

        // Scene-mesh entities now live in the MeshLibRef pass below.
    }

    // ---- MeshLibRef entities (P1-P6 path). Phase A serial: gather + meshDesc cache.
    // Phase B parallel: OBB cull + candidate construction (no shared writes).
    if (m_meshLib)
    {
        struct MlrJob {
            Entity                 e;
            const MeshLibRef*      mlr;
            const GlobalTransform* gt;
            uint32_t               meshDescSlot;
            uint32_t               indexCount;
        };
        // Local (NOT thread_local): worker lambdas read by ref; thread_local trips subscript asserts.
        std::vector<MlrJob> mlrJobs;
        if (pMeshLibRef)
        {
            auto& mlrEnts = pMeshLibRef->Entities();
            auto& mlrData = pMeshLibRef->Data();
            const size_t mlrN = mlrData.size();
            mlrJobs.reserve(mlrN);
            for (size_t k = 0; k < mlrN; ++k)
            {
                const Entity e = mlrEnts[k];
                if (!world.IsAlive(e)) continue;
                const MeshLibRef& mlr = mlrData[k];
                if (!mlr.IsValid()) continue;
                const GlobalTransform* gt = pGet(pGlobalXf, e);
                if (!gt) continue;

                // MeshDesc slot: skinned+active → skinning slot; skinned+inactive → meshlib (rest pose);
                // non-skinned → meshlib. BuildSkinJobs patches the skinning slot to SkinnedVertexRing.
                const MeshSkinnedComponent*    skMesh  = pGet(pSkinned, e);
                const SkinningOutputComponent* skinOut = pGet(pSkinOut, e);
                const bool skinningActive =
                    skMesh && skinOut && skinOut->poseByteOffset != ~0u
                    && skMesh->meshDescriptorIdx != RHI::kInvalidBufferIndex;

                uint32_t slot = RHI::kInvalidBufferIndex;
                uint32_t indexCountForDraw = 0;
                if (skinningActive)
                {
                    slot              = skMesh->meshDescriptorIdx;
                    indexCountForDraw = skMesh->indexCount;
                }
                else
                {
                    // Serial: populates MeshManager caches safely.
                    slot = m_meshMgr.RegisterMeshLibMesh(*m_meshLib, mlr);
                    if (slot == RHI::kInvalidBufferIndex) continue;
                    const auto* entry = m_meshLib->GetEntry(mlr.libHandle, mlr.meshId);
                    if (!entry) continue;
                    indexCountForDraw = entry->indexCount;
                }
                mlrJobs.push_back({ e, &mlr, gt, slot, indexCountForDraw });
            }
        }

        // Per-entity work; used from both serial fallback and parallel tasks.
        auto processJob = [&](const MlrJob& j, std::vector<DrawCandidate>& out)
        {
            // Transparent materials skip camera-cull drop (alpha-blend rarely casts shadow).
            const MaterialComponent* mcPre        = pGet(pMaterial, j.e);
            const bool               isTransparent =
                mcPre && mcPre->userBlendMode != BlendMode::Opaque;

            // Frustum cull with shadow-fallback.
            bool isShadowOnly = false;
            if (m_gpuCullingEnabled)
            {
                if (inMask(bvhTestedMask, j.e) && !inMask(bvhVisibleMask, j.e)
                    && !isTransparent)
                {
                    if (!m_shadowFrustum.IsValid()) return;
                    const MaterialComponent* mcSh = pGet(pMaterial, j.e);
                    if (mcSh && !mcSh->IsCastingShadow()) return;
                    const WorldAabb* aabb = pGet(pWorldAabb, j.e);
                    if (!aabb) return;
                    BoundingBox bb;
                    bb.Center  = { (aabb->min.x + aabb->max.x) * 0.5f,
                                   (aabb->min.y + aabb->max.y) * 0.5f,
                                   (aabb->min.z + aabb->max.z) * 0.5f };
                    bb.Extents = { (aabb->max.x - aabb->min.x) * 0.5f,
                                   (aabb->max.y - aabb->min.y) * 0.5f,
                                   (aabb->max.z - aabb->min.z) * 0.5f };
                    if (!m_shadowFrustum.IntersectsAny(bb)) return;
                    isShadowOnly = true;
                }
            }
            else
            {
                const WorldAabb* aabb = pGet(pWorldAabb, j.e);
                if (aabb && !FrustumTestAABB(m_view.boundingFrustum, aabb->min, aabb->max)
                    && !isTransparent)
                {
                    if (!m_shadowFrustum.IsValid()) return;
                    const MaterialComponent* mcSh = pGet(pMaterial, j.e);
                    if (mcSh && !mcSh->IsCastingShadow()) return;
                    BoundingBox bb;
                    bb.Center  = { (aabb->min.x + aabb->max.x) * 0.5f,
                                   (aabb->min.y + aabb->max.y) * 0.5f,
                                   (aabb->min.z + aabb->max.z) * 0.5f };
                    bb.Extents = { (aabb->max.x - aabb->min.x) * 0.5f,
                                   (aabb->max.y - aabb->min.y) * 0.5f,
                                   (aabb->max.z - aabb->min.z) * 0.5f };
                    if (!m_shadowFrustum.IntersectsAny(bb)) return;
                    isShadowOnly = true;
                }
            }

            DrawCandidate c;
            c.entity       = j.e;
            c.meshDescSlot = j.meshDescSlot;
            c.indexCount   = j.indexCount;
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&j.gt->matrix)));
            // Depth-sort centroid: WorldAabb centre when present, else pivot fallback.
            if (const WorldAabb* aabb = pGet(pWorldAabb, j.e))
            {
                c.worldCenter = { (aabb->min.x + aabb->max.x) * 0.5f,
                                  (aabb->min.y + aabb->max.y) * 0.5f,
                                  (aabb->min.z + aabb->max.z) * 0.5f };
            }
            else
            {
                c.worldCenter = { j.gt->matrix.m[3][0],
                                  j.gt->matrix.m[3][1],
                                  j.gt->matrix.m[3][2] };
            }
            c.mc = pGet(pMaterial, j.e);
            if (c.mc)
            {
                c.texBaseColor  = c.mc->textures[MaterialComponent::BASECOLORMAP].gpuHandle;
                c.texSurfaceMap = c.mc->textures[MaterialComponent::SURFACEMAP].gpuHandle;
                c.texNormalMap  = c.mc->textures[MaterialComponent::NORMALMAP].gpuHandle;
                c.matHash       = HashMatParams(c.mc);
            }

            if (isShadowOnly)
            {
                c.filter = DrawFilter::Shadow;
                c.perm   = {};
                if (c.mc && c.mc->IsAlphaTestEnabled())
                    c.perm.Set(PermutationKey::ALPHA_TEST, true);
            }
            else
            {
                applyBlendMode(c);
            }
            out.push_back(c);

            // Outline path: push-then-mutate avoids full DrawCandidate memcpy.
            if (!isShadowOnly && c.mc && c.mc->IsOutlineEnabled() && c.filter == DrawFilter::Opaque)
            {
                const float outlinePixels      = c.mc->outlinePixels;
                const bool  screenSpaceOutline = c.mc->IsOutlineScreenSpaceEnabled();
                out.push_back(c);
                DrawCandidate& outline = out.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
            }
        };

        const uint32_t N = static_cast<uint32_t>(mlrJobs.size());
        // Below this the fork/join overhead exceeds the work savings.
        constexpr uint32_t kParallelThreshold = 128;
        const unsigned numWorkers = TaskSystem::Get().GetWorkerCount();

        if (N >= kParallelThreshold && numWorkers > 1)
        {
            const uint32_t numChunks = (std::min)(static_cast<uint32_t>(numWorkers), N);
            const uint32_t chunkSize = (N + numChunks - 1) / numChunks;

            // Local vector-of-vectors; each worker writes only its own slot.
            std::vector<std::vector<DrawCandidate>> local(numChunks);

            std::atomic<uint32_t>    remaining(numChunks);
            std::mutex               doneMtx;
            std::condition_variable  doneCv;

            for (uint32_t t = 0; t < numChunks; ++t)
            {
                const uint32_t lo = t * chunkSize;
                const uint32_t hi = (std::min)(N, lo + chunkSize);
                TaskSystem::Get().Push(
                    [&, t, lo, hi]()
                    {
                        auto& out = local[t];
                        out.reserve((hi - lo) * 2 + 8);
                        for (uint32_t i = lo; i < hi; ++i)
                            processJob(mlrJobs[i], out);
                        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        {
                            std::lock_guard<std::mutex> lk(doneMtx);
                            doneCv.notify_one();
                        }
                    },
                    TaskSystem::TaskPriority::High);
            }

            std::unique_lock<std::mutex> lk(doneMtx);
            doneCv.wait(lk, [&] { return remaining.load(std::memory_order_acquire) == 0; });

            // Merge via single resize + memcpy (DrawCandidate is trivially copyable).
            size_t total = 0;
            for (uint32_t i = 0; i < numChunks; ++i) total += local[i].size();
            const size_t oldSize = candidates.size();
            candidates.resize(oldSize + total);
            DrawCandidate* dst = candidates.data() + oldSize;
            for (uint32_t i = 0; i < numChunks; ++i)
            {
                const size_t n = local[i].size();
                if (n == 0) continue;
                std::memcpy(dst, local[i].data(), n * sizeof(DrawCandidate));
                dst += n;
            }
        }
        else
        {
            for (uint32_t i = 0; i < N; ++i)
                processJob(mlrJobs[i], candidates);
        }
    }

    // ---- Billboard entities → additional DrawCandidates ----
    // Lazy-load light icon texture (m_texSys not available during Compile/Init)
    if (m_lightIconHandle == Resource::kInvalidTextureHandle && m_texSys && m_resMgr)
    {
        m_lightIconHandle = m_texSys->Acquire(
            "asset/Default_Texture/lightsymbol.itex", *m_resMgr, m_gfx);
    }
    if (m_lightIconSRV == 0 && m_texSys && m_lightIconHandle != Resource::kInvalidTextureHandle)
    {
        if (m_texSys->IsReady(m_lightIconHandle))
        {
            const RHI::Texture* tex = m_texSys->GetTexture(m_lightIconHandle);
            if (tex) m_lightIconSRV = m_gfx.GetTextureSRVGpuHandle(*tex);
        }
    }

    if (m_meshMgr.GetBillboardMeshDescSlot() != RHI::kInvalidBufferIndex && pBillboard)
    {
        XMVECTOR camPos = XMLoadFloat3(&m_view.cameraPosition);

        auto& bbEnts = pBillboard->Entities();
        auto& bbData = pBillboard->Data();
        const size_t bbN = bbData.size();
        for (size_t bi = 0; bi < bbN; ++bi)
        {
            const Entity e = bbEnts[bi];
            if (!world.IsAlive(e)) continue;
            const auto* bb = &bbData[bi];
            const GlobalTransform* gt = pGet(pGlobalXf, e);
            if (!gt) continue;

            XMFLOAT3 pos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
            float size = bb->worldSize;

            // Camera world axes = view matrix columns (orthogonal: inv = transpose).
            float crx = m_view.viewMatrix._11, cry = m_view.viewMatrix._21, crz = m_view.viewMatrix._31;
            float cux = m_view.viewMatrix._12, cuy = m_view.viewMatrix._22, cuz = m_view.viewMatrix._32;
            float cfx = m_view.viewMatrix._13, cfy = m_view.viewMatrix._23, cfz = m_view.viewMatrix._33;

            XMFLOAT4X4 billboardWorld;
            XMStoreFloat4x4(&billboardWorld, XMMatrixIdentity());
            // Row 0 = camera right × size
            billboardWorld._11 = crx * size;
            billboardWorld._12 = cry * size;
            billboardWorld._13 = crz * size;
            // Row 1 = camera up × size
            billboardWorld._21 = cux * size;
            billboardWorld._22 = cuy * size;
            billboardWorld._23 = cuz * size;
            // Row 2 = camera forward
            billboardWorld._31 = cfx;
            billboardWorld._32 = cfy;
            billboardWorld._33 = cfz;
            // Row 3 = position
            billboardWorld._41 = pos.x;
            billboardWorld._42 = pos.y;
            billboardWorld._43 = pos.z;

            DrawCandidate c;
            c.entity       = e;
            c.meshDescSlot = m_meshMgr.GetBillboardMeshDescSlot();
            c.indexCount   = m_meshMgr.GetBillboardQuad().indexCount;

            // Transpose for GPU (row-vector mul); pivot==centroid for billboard depth sort.
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&billboardWorld)));
            c.worldCenter = { pos.x, pos.y, pos.z };

            c.mc = pGet(pMaterial, e);
            if (c.mc)
            {
                c.texBaseColor = c.mc->textures[MaterialComponent::BASECOLORMAP].gpuHandle;
            }
            // Route by BillboardMode
            PermutationKey perm{};
            perm.Set(PermutationKey::BILLBOARD, true);

            // Auto-assign light icon if LightData and no explicit texture; UNLIT branch.
            if (pGet(pLightData, e))
            {
                if (c.texBaseColor == 0 && m_lightIconSRV)
                    c.texBaseColor = m_lightIconSRV;
                perm.Set(PermutationKey::UNLIT, true);
            }

            switch (bb->mode)
            {
            case BillboardMode::Opaque:
                c.filter = DrawFilter::Opaque;
                break;
            case BillboardMode::AlphaClip:
                c.filter = DrawFilter::Opaque;
                perm.Set(PermutationKey::ALPHA_TEST, true);
                break;
            case BillboardMode::Transparent:
                c.filter = DrawFilter::Transparent;
                perm.Set(PermutationKey::ALPHA_BLEND, true);
                break;
            case BillboardMode::Additive:
                c.filter = DrawFilter::Transparent;
                perm.Set(PermutationKey::ADDITIVE_BLEND, true);
                break;
            }
            c.perm = perm;

            // View-space Z depth via current-frame camForward (m_view is 1-frame stale here).
            XMVECTOR ePos    = XMLoadFloat3(&pos);
            XMVECTOR forward = XMLoadFloat3(&camForwardThisFrame);
            c.depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(ePos, camPos), forward));

            candidates.push_back(c);
        }
    }

    // ---- Beam entities → CS-generated tube DrawCandidates. World-space control points.
    // For beam-follows-entity, leave transform non-identity (VS multiplies world matrix).
    if (m_beamSystem)
    {
        auto* pBeam = world.GetPool<BeamComponent>();
        if (pBeam)
        {
            auto& bEnts = pBeam->Entities();
            auto& bData = pBeam->Data();
            const size_t bN = bData.size();
            for (size_t bi = 0; bi < bN; ++bi)
            {
                const Entity e = bEnts[bi];
                if (!world.IsAlive(e)) continue;
                BeamComponent& beam = bData[bi];
                if (beam.controlPoints.size() < 2) continue;

                const GlobalTransform*   gt = pGet(pGlobalXf, e);
                const MaterialComponent* mc = pGet(pMaterial, e);
                if (!gt || !mc) continue;

                // Lazy slot acquisition. Released in OnEntityDestroyed.
                if (beam.beamSlot == 0xFFFFFFFFu)
                {
                    beam.beamSlot = m_beamSystem->Acquire();
                    if (beam.beamSlot == 0xFFFFFFFFu) continue; // pool exhausted
                }

                // BeamControlPoint and BeamControlPointGPU share the 32-byte layout — direct reinterpret.
                static_assert(sizeof(BeamControlPoint) == sizeof(BeamControlPointGPU),
                              "BeamControlPoint vs GPU layout drift");
                m_beamSystem->SetControlPoints(
                    beam.beamSlot,
                    reinterpret_cast<const BeamControlPointGPU*>(beam.controlPoints.data()),
                    static_cast<uint32_t>(beam.controlPoints.size()));

                BeamGenParamsGPU params{};
                params.globalRadiusScale = beam.globalRadiusScale;
                params.wobbleAmplitude   = beam.wobbleAmplitude;
                params.wobbleSpeed       = beam.wobbleSpeed;
                m_beamSystem->SetParams(beam.beamSlot, params);

                const uint32_t meshDescSlot = m_beamSystem->GetMeshDescSlot(beam.beamSlot);
                if (meshDescSlot == 0xFFFFFFFFu) continue;

                DrawCandidate c;
                c.entity       = e;
                c.meshDescSlot = meshDescSlot;
                c.indexCount   = BeamSystem::GetIndexCount();

                // World matrix — transposed for HLSL row-vec convention.
                XMStoreFloat4x4(&c.worldMatrix,
                    XMMatrixTranspose(XMLoadFloat4x4(&gt->matrix)));

                // Centroid: avg control-point positions for transparent depth sort.
                XMFLOAT3 centroid{ 0, 0, 0 };
                for (const auto& cp : beam.controlPoints)
                {
                    centroid.x += cp.position.x;
                    centroid.y += cp.position.y;
                    centroid.z += cp.position.z;
                }
                const float invN = 1.0f / static_cast<float>(beam.controlPoints.size());
                centroid.x *= invN; centroid.y *= invN; centroid.z *= invN;
                c.worldCenter = centroid;

                c.mc            = mc;
                c.texBaseColor  = mc->textures[MaterialComponent::BASECOLORMAP].gpuHandle;
                c.texSurfaceMap = mc->textures[MaterialComponent::SURFACEMAP].gpuHandle;
                c.texNormalMap  = mc->textures[MaterialComponent::NORMALMAP].gpuHandle;

                applyBlendMode(c);
                c.customPSID = resolveCustomPSID(*mc);

                // Depth (view-space Z) for transparent painter's-algorithm sort.
                {
                    const float dx = c.worldCenter.x - m_camera.position.x;
                    const float dy = c.worldCenter.y - m_camera.position.y;
                    const float dz = c.worldCenter.z - m_camera.position.z;
                    c.depth = dx * camForwardThisFrame.x
                            + dy * camForwardThisFrame.y
                            + dz * camForwardThisFrame.z;
                }

                candidates.push_back(c);
            }
        }
    }

    s_lastCandidateCount = static_cast<uint32_t>(candidates.size());
    s_lastFrameLightCount = static_cast<uint32_t>(m_frameLights.size());
    s_lastVolLightCount   = static_cast<uint32_t>(m_volumetricLights.size());

    // ---- Phase 2: Sort. Opaque/Shadow by PSO state (perm→stencil→tex→mat→meshDescSlot);
    // Transparent back-to-front. Sort indices not structs (~30× less memcpy on 3k candidates).
    thread_local std::vector<uint32_t> s_sortIdx;
    s_sortIdx.resize(candidates.size());
    for (uint32_t k = 0; k < candidates.size(); ++k) s_sortIdx[k] = k;
    // par_unseq: MSVC parallel sort kicks in once N>~500; comparator is read-only/thread-safe.
    std::sort(std::execution::par_unseq,
        s_sortIdx.begin(), s_sortIdx.end(),
        [&candidates](uint32_t ia, uint32_t ib)
        {
            const DrawCandidate& a = candidates[ia];
            const DrawCandidate& b = candidates[ib];
            if (a.filter != b.filter)
                return static_cast<uint8_t>(a.filter) < static_cast<uint8_t>(b.filter);
            if (a.filter == DrawFilter::Transparent)
            {
                // Painter's order; entity-id tiebreaker pins equal-depth pairs (prevents flicker).
                if (a.depth != b.depth) return a.depth > b.depth;
                return a.entity < b.entity;
            }
            // PSO state first (most expensive to switch).
            if (a.perm != b.perm)
                return a.perm.bits < b.perm.bits;
            if (a.stencilRef    != b.stencilRef)    return a.stencilRef    < b.stencilRef;
            // Texture descriptors (moderate switch cost).
            if (a.texBaseColor  != b.texBaseColor)  return a.texBaseColor  < b.texBaseColor;
            if (a.texSurfaceMap != b.texSurfaceMap) return a.texSurfaceMap < b.texSurfaceMap;
            if (a.texNormalMap  != b.texNormalMap)  return a.texNormalMap  < b.texNormalMap;
            if (a.matHash       != b.matHash)       return a.matHash       < b.matHash;
            // Mesh descriptor last (just a root constant, cheapest to switch).
            return a.meshDescSlot < b.meshDescSlot;
        });
    const auto& indices = s_sortIdx;

    // ---- Phase 3: Write buffers + emit batched DrawPackets -----------------
    auto* instances = static_cast<GPUInstanceData*>(m_instanceBufferMapped);
    auto* matBuf   = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped);
    uint32_t instIdx = 0;
    // Independent matIdx capped at kMaxMaterials (instIdx caps at kMaxInstances=32k).
    uint32_t matIdx = 0;

    // Phase E/F: per-material CBV VA + tex-table; cleared each frame so unused materials read 0.
    if (m_matCustomCbvVA.size() < kMaxMaterials)
        m_matCustomCbvVA.resize(kMaxMaterials, 0ULL);
    std::fill(m_matCustomCbvVA.begin(), m_matCustomCbvVA.end(), 0ULL);
    if (m_matCustomTexHandle.size() < kMaxMaterials)
        m_matCustomTexHandle.resize(kMaxMaterials, 0ULL);
    std::fill(m_matCustomTexHandle.begin(), m_matCustomTexHandle.end(), 0ULL);

    // O(1) prev-batch material-slot reuse — adjacent same-mat batches share one slot.
    bool     hasPrevMat  = false;
    uint32_t prevMatHash = 0;
    uint64_t prevTexBase = 0, prevTexSurf = 0, prevTexNorm = 0;
    uint32_t prevMatIdx  = 0;

    const int n = static_cast<int>(candidates.size());
    for (int i = 0; i < n && instIdx < kMaxInstances; )
    {
        const DrawCandidate& first = candidates[indices[i]];

        // End of batch: same key. Transparent rarely batches (depth-sorted).
        int j = i + 1;
        while (j < n && (instIdx + static_cast<uint32_t>(j - i)) < kMaxInstances)
        {
            const DrawCandidate& cur = candidates[indices[j]];
            if (cur.filter        != first.filter        ||
                cur.perm          != first.perm          ||
                cur.stencilRef    != first.stencilRef    ||
                cur.meshDescSlot  != first.meshDescSlot  ||
                cur.texBaseColor  != first.texBaseColor  ||
                cur.texSurfaceMap != first.texSurfaceMap ||
                cur.texNormalMap  != first.texNormalMap  ||
                cur.matHash       != first.matHash       ||
                cur.outlinePixels != first.outlinePixels ||
                cur.customPSID   != first.customPSID)
                break;
            ++j;
        }

        const uint32_t batchStart = instIdx;
        const uint32_t batchCount = static_cast<uint32_t>(j - i);

        // Allocate or reuse material slot via O(1) prev-batch compare.
        uint32_t thisMatIdx;
        if (first.mc
            && hasPrevMat
            && first.matHash       == prevMatHash
            && first.texBaseColor  == prevTexBase
            && first.texSurfaceMap == prevTexSurf
            && first.texNormalMap  == prevTexNorm)
        {
            thisMatIdx = prevMatIdx;
        }
        else
        {
            thisMatIdx = (matIdx < kMaxMaterials) ? matIdx++ : 0u;

            // Custom reflection only when custom PS resolved; else slots stay zeroed.
            const ShaderReflect::Reflection* customRefl = nullptr;
            if (first.customPSID > 0 && m_gbufferPass)
                customRefl = m_gbufferPass->GetShaderLibrary()
                                 .GetDynamicReflection(first.customPSID);

            WriteMatSlot(matBuf, thisMatIdx, first.mc, customRefl);

            // Phase E — per-material CBV from reflection. Walks first non-reserved cbuffer;
            // places customParams[name] at reflected byte offset.
            if (first.mc && customRefl && first.mc->useCustomShader)
            {
                const ShaderReflect::CBufferLayout* userCB = nullptr;
                for (const auto& cb : customRefl->cbuffers)
                {
                    if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                    userCB = &cb;
                    break;
                }
                if (userCB && userCB->sizeBytes > 0)
                {
                    auto slice = m_customMatCbvRing.Allocate(userCB->sizeBytes);
                    if (slice.cpu)
                    {
                        // Zero-fill first so cbuffer holes are deterministic.
                        std::memset(slice.cpu, 0, userCB->sizeBytes);
                        for (const auto& v : userCB->vars)
                        {
                            auto it = first.mc->customParams.find(v.name);
                            if (it == first.mc->customParams.end()) continue;
                            // Copy min(v.size, sizeof(val[])) — values stored as float[4]; shader decides.
                            const uint32_t copy = (v.size < sizeof(it->second))
                                ? v.size : static_cast<uint32_t>(sizeof(it->second));
                            std::memcpy(
                                static_cast<uint8_t*>(slice.cpu) + v.offset,
                                it->second.data(),
                                copy);
                        }
                        m_matCustomCbvVA[thisMatIdx] = slice.gpu;
                    }
                }

                // Phase F — per-material texture descriptor table; reflection-ordered, reserved names skipped.
                auto texSlice = m_customMatSrvRing.Allocate(
                    GraphicsDX12::kCustomMatTextureSlots);
                if (texSlice.gpu)
                {
                    // Unset slots leave zero-descriptor (sampling returns black).
                    uint32_t texSlot = 0;
                    for (const auto& b : customRefl->bindings)
                    {
                        if (texSlot >= GraphicsDX12::kCustomMatTextureSlots) break;
                        if (b.type != ShaderReflect::ResourceType::Texture) continue;
                        if (MaterialReflectionSync::IsReservedTexture(b.name.c_str())) continue;

                        const uint64_t dst = texSlice.cpu.ptr +
                            static_cast<uint64_t>(texSlot) * texSlice.descIncBytes;

                        uint64_t src = 0;
                        if (auto it = first.mc->customTextures.find(b.name);
                            it != first.mc->customTextures.end() &&
                            it->second.bindlessIndex >= 0)
                        {
                            RHI::Texture t;
                            t.handle_id = static_cast<uint32_t>(it->second.bindlessIndex);
                            src = m_gfx.GetTextureSRVCpuHandle(t);
                        }

                        if (src != 0)
                            m_gfx.CopyCbvSrvUavDescriptors(dst, src, 1);
                        ++texSlot;
                    }
                    m_matCustomTexHandle[thisMatIdx] = texSlice.gpu;
                }
            }
            if (first.mc)
            {
                hasPrevMat  = true;
                prevMatHash = first.matHash;
                prevTexBase = first.texBaseColor;
                prevTexSurf = first.texSurfaceMap;
                prevTexNorm = first.texNormalMap;
                prevMatIdx  = thisMatIdx;
            }
            else
            {
                hasPrevMat = false;
            }
        }

        // Write per-instance data (transform + mesh/material indices).
        for (int k = i; k < j; ++k, ++instIdx)
        {
            const DrawCandidate& ck = candidates[indices[k]];
            if (instances)
            {
                GPUInstanceData& inst = instances[instIdx];
                inst.world       = ck.worldMatrix;
                inst.meshDescIdx = ck.meshDescSlot;
                inst.materialIdx = thisMatIdx; // material slot in material buffer
                inst.lodLevel    = 0;
                inst.pad         = 0;
            }
            m_instanceSlotToEntity[instIdx] = ck.entity;
        }

        DrawPacket dp;
        dp.meshDescriptorIndex = first.meshDescSlot;
        dp.instanceOffset      = batchStart;
        dp.instanceCount       = batchCount;
        dp.vertexOrIndexCount  = first.indexCount;
        dp.materialIndex       = thisMatIdx;
        dp.permutation         = first.perm;
        dp.filter              = first.filter;
        dp.sortKey             = DrawPacket::MakeSortKey(
                                    static_cast<uint16_t>(first.perm.bits), 0, 0);
        dp.texBaseColor        = first.texBaseColor;
        dp.texSurfaceMap       = first.texSurfaceMap;
        dp.texNormalMap        = first.texNormalMap;
        dp.outlinePixels       = first.outlinePixels;
        dp.screenSpaceOutline  = first.screenSpaceOutline;
        dp.stencilRef          = first.stencilRef;
        dp.shadowCullMode      = first.shadowCullMode;
        dp.castShadow          = first.castShadow;
        dp.prevPosElementBase  = first.prevPosElementBase;
        dp.customPSID          = first.customPSID;
        dp.customCbvVA         = (thisMatIdx < m_matCustomCbvVA.size())
                                 ? m_matCustomCbvVA[thisMatIdx]
                                 : 0ULL;
        dp.customTexTable      = (thisMatIdx < m_matCustomTexHandle.size())
                                 ? m_matCustomTexHandle[thisMatIdx]
                                 : 0ULL;
        m_drawPackets.push_back(dp);

        // Also write IndirectDrawCommand for ExecuteIndirect path.
        if (m_indirectArgMapped)
        {
            auto* indirectArgs = static_cast<IndirectDrawCommand*>(m_indirectArgMapped);
            uint32_t cmdIdx = static_cast<uint32_t>(m_drawPackets.size()) - 1;
            if (cmdIdx < kMaxInstances)
            {
                IndirectDrawCommand& ic = indirectArgs[cmdIdx];
                ic.meshDescIdx     = dp.meshDescriptorIndex;
                ic.instanceOffset  = dp.instanceOffset;
                ic.materialIndex   = dp.materialIndex;
                ic.prevPosInfo     = dp.prevPosElementBase;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                ic.instanceCount          = dp.instanceCount;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
            }
        }

        i = j;
    }

    m_indirectDrawCount = static_cast<uint32_t>(m_drawPackets.size());

    // Build per-PSO groups for ExecuteIndirect batching (only opaque filter).
    m_indirectGroups.clear();
    if (m_indirectArgMapped && m_indirectDrawCount > 0)
    {
        uint32_t groupStart = 0;
        for (uint32_t di = 0; di < m_indirectDrawCount; ++di)
        {
            const DrawPacket& dp = m_drawPackets[di];
            // Only include opaque draws in indirect groups (transparent handled separately).
            if (dp.filter != DrawFilter::Opaque) continue;

            bool newGroup = m_indirectGroups.empty()
                || m_indirectGroups.back().perm           != dp.permutation
                || m_indirectGroups.back().stencilRef     != dp.stencilRef
                || m_indirectGroups.back().customPSID     != dp.customPSID
                || m_indirectGroups.back().customCbvVA    != dp.customCbvVA
                || m_indirectGroups.back().customTexTable != dp.customTexTable;

            if (newGroup)
            {
                IndirectGroup g;
                g.perm           = dp.permutation;
                g.stencilRef     = dp.stencilRef;
                g.customPSID     = dp.customPSID;
                g.customCbvVA    = dp.customCbvVA;
                g.customTexTable = dp.customTexTable;
                g.argOffset      = di * sizeof(IndirectDrawCommand);
                g.cmdCount       = 1;
                m_indirectGroups.push_back(g);
            }
            else
            {
                m_indirectGroups.back().cmdCount++;
            }
        }
    }

    // Phase 4: upload lights + sync IBL (separated for future extensibility).
    BuildScene_UploadLights(world);

    // Phase 5: pack reflection probes into GPU StructuredBuffer (placement/bounds/count only).
    BuildScene_UploadProbes(world);

    // Phase 5b: DDGI volume scan + tick — must run before LightingPass binds DDGI SRVs.
    BuildScene_UpdateDDGI(world);

    // Phase 6: terrain heightmap sync + CB upload + arm TerrainPass.
    BuildScene_SyncTerrain(world);

    // 1 Hz draw-stats log: packet count, instance count, max batch size.
    {
        static uint32_t s_statFrameCounter = 0;
        if ((++s_statFrameCounter % 60u) == 0)
        {
            uint32_t totalInstances  = 0;
            uint32_t instancedDraws  = 0;  // packets with instanceCount > 1
            uint32_t maxBatch        = 0;
            uint32_t opaqueDraws     = 0;
            uint32_t shadowDraws     = 0;
            uint32_t transparentDraws= 0;
            uint32_t customDraws     = 0;
            for (const DrawPacket& dp : m_drawPackets)
            {
                totalInstances += dp.instanceCount;
                if (dp.instanceCount > 1) ++instancedDraws;
                if (dp.instanceCount > maxBatch) maxBatch = dp.instanceCount;
                switch (dp.filter)
                {
                case DrawFilter::Opaque:      ++opaqueDraws;      break;
                case DrawFilter::Shadow:      ++shadowDraws;      break;
                case DrawFilter::Transparent: ++transparentDraws; break;
                case DrawFilter::Custom:      ++customDraws;      break;
                default: break;
                }
            }
           // (disabled) DrawStats LOG_INFO; re-enable for tuning batch behaviour.
        }
    }
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_UploadLights(World& world)
{
    auto* lb = m_lightCBMapped ? static_cast<LightCB*>(m_lightCBMapped) : nullptr;

    // ---- Spot-shadow slice assignment: first kMaxCasters shadow-casting spots.
    // Non-casters keep shadowSliceIdx = ~0u; shaders fall back to occlusion-only path.
    uint32_t activeCasters = 0;
    if (m_spotShadowPass && m_spotShadowVPMapped)
    {
        auto* vpDst = static_cast<XMFLOAT4X4*>(m_spotShadowVPMapped);
        const uint32_t kMaxCasters = SpotShadowPass::kMaxCasters;

        for (auto& L : m_frameLights)
        {
            if (activeCasters >= kMaxCasters) break;
            if (L.type != LightType::Spot || !L.castsShadow)  continue;
            if (L.radius <= 0.0f || L.spotAngle <= 0.0f)      continue;

            // Light-space VP; up chosen away from light axis so LookToLH stays non-degenerate.
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&L.direction));
            XMFLOAT3 d3;  XMStoreFloat3(&d3, dir);
            XMVECTOR up = (std::fabs(d3.y) > 0.99f)
                        ? XMVectorSet(0, 0, 1, 0)
                        : XMVectorSet(0, 1, 0, 0);
            XMMATRIX view = XMMatrixLookToLH(
                XMLoadFloat3(&L.position), dir, up);

            // Reversed-Z perspective (engine convention: near=1.0, far=0.0, GREATER_EQUAL).
            // fov = 2× outer spot angle so the entire cone is covered.
            float fov = std::min(L.spotAngle * 2.0f, XM_PI - 0.01f);
            XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, 1.0f, L.radius, 0.1f);
            XMMATRIX vp   = view * proj;

            // SpotShadowPass transposes internally; pass un-transposed row-major.
            XMFLOAT4X4 vpUntransposed;
            XMStoreFloat4x4(&vpUntransposed, vp);
            m_spotShadowPass->SetShadowMatrix(activeCasters, vpUntransposed);

            // StructuredBuffer<float4x4> default is column-major — store transposed.
            XMStoreFloat4x4(&vpDst[activeCasters], XMMatrixTranspose(vp));

            L.shadowSliceIdx = activeCasters;

            // Propagate to volumetric list so FroxelLightInject samples the spot-shadow atlas
            // (avoids "light through wall" leaks from the screen-space+voxel fallback).
            for (auto& vl : m_volumetricLights)
            {
                if (vl.type == static_cast<uint32_t>(LightType::Spot)
                    && std::abs(vl.position.x - L.position.x) < 1e-4f
                    && std::abs(vl.position.y - L.position.y) < 1e-4f
                    && std::abs(vl.position.z - L.position.z) < 1e-4f)
                {
                    vl.shadowSliceIdx = activeCasters;
                    break;
                }
            }

            ++activeCasters;
        }

        m_spotShadowPass->SetActiveCasterCount(activeCasters);
    }

    {
        // Upload to ClusterPass
        if (m_clusterPass)
        {
            // Convert to ClusterPass::ResolvedLight
            std::vector<ClusterPass::ResolvedLight> clusterLights(m_frameLights.size());
            for (size_t i = 0; i < m_frameLights.size(); ++i)
            {
                auto& s = m_frameLights[i];
                auto& d = clusterLights[i];
                d.position       = s.position;
                d.radius         = s.radius;
                d.color          = s.color;
                d.intensity      = s.intensity;
                d.direction      = s.direction;
                d.spotAngle      = s.spotAngle;
                d.type           = s.type;
                d.shadowSliceIdx = s.shadowSliceIdx;
            }
            m_clusterPass->SetLights(clusterLights);

            // Set cluster camera params
            XMMATRIX view = XMLoadFloat4x4(&m_view.viewMatrix);
            XMMATRIX proj = XMLoadFloat4x4(&m_view.projMatrixNoJitter);
            XMMATRIX invP = XMMatrixInverse(nullptr, proj);
            XMFLOAT4X4 invProjF, viewF;
            XMStoreFloat4x4(&invProjF, XMMatrixTranspose(invP));
            XMStoreFloat4x4(&viewF, XMMatrixTranspose(view));
            m_clusterPass->SetCamera(invProjF, viewF,
                                     m_camera.nearZ, m_camera.farZ,
                                     m_vpWidth, m_vpHeight);

            // Update LightCB with cluster params
            if (lb)
            {
                XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->viewMatrix),
                                XMMatrixTranspose(view));
                lb->clusterNearZ      = m_camera.nearZ;
                lb->clusterFarZ       = m_camera.farZ;
                lb->clusterLightCount = m_clusterPass->GetLightCount();
            }
        }

        // NPR params are per-material now; LightCB.nprMinBrightness=0 so shadows reach true black.
        lb->nprMinBrightness = 0.0f;
    }

    // ---- Sync IBL from SkyboxComponent → LightCB + passes ------------------
    SyncSkyboxIBL(world);

    // ---- Clustered decals — asset-driven; shares ClusterPass camera state.
    // Pump Resolve()+TryPromote() on each unique asset referenced this frame.
    if (m_decalPass)
    {
        std::vector<DecalPass::ResolvedDecal> resolvedDecals;
        auto* pDecal = world.GetPool<DecalComponent>();
        auto* pGT    = world.GetPool<GlobalTransform>();
        const bool haveTexSys = (m_texSys && m_resMgr);

        if (pDecal && pGT && pDecal->Size() > 0 && haveTexSys)
        {
            // Pump assets referenced this frame exactly once each.
            std::unordered_set<Resource::DecalMaterialAsset*> pumped;
            pumped.reserve(pDecal->Size());

            const auto& ents = pDecal->Entities();
            resolvedDecals.reserve(ents.size());
            for (size_t i = 0; i < ents.size(); ++i)
            {
                Entity e = ents[i];
                DecalComponent* dc  = pDecal->Get(e);
                const GlobalTransform* gt = pGT->Get(e);
                if (!dc || !gt || !dc->material) continue;

                Resource::DecalMaterialAsset* asset = dc->material.get();
                if (pumped.insert(asset).second)
                {
                    asset->Resolve(*m_texSys, *m_resMgr, m_gfx);
                    asset->TryPromote(*m_texSys);
                }

                // Skip only when no textures AND no scalar overrides (allows pure-scalar decals).
                bool anySlot = false;
                for (uint32_t s = 0; s < Resource::DecalMaterialAsset::SLOT_COUNT; ++s)
                    if (asset->texBindless[s] >= 0) { anySlot = true; break; }
                const bool scalarOnlyWrite =
                    (asset->flags & (Resource::DecalMaterialAsset::WRITE_ROUGHNESS |
                                     Resource::DecalMaterialAsset::WRITE_SPECULAR  |
                                     Resource::DecalMaterialAsset::WRITE_AO)) != 0;
                if (!anySlot && !scalarOnlyWrite) continue;

                // worldToDecal = inverse(world); row-major DX-style (last row = translation).
                XMMATRIX worldMtx = XMLoadFloat4x4(&gt->matrix);
                XMMATRIX inv      = XMMatrixInverse(nullptr, worldMtx);

                DecalPass::ResolvedDecal rd;
                XMStoreFloat4x4(&rd.worldToDecal, XMMatrixTranspose(inv));
                rd.boundsCenter = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };

                // OBB-sphere radius: unit cube's half-diagonal = sqrt(3)/2.
                XMVECTOR sx = XMVector3Length(worldMtx.r[0]);
                XMVECTOR sy = XMVector3Length(worldMtx.r[1]);
                XMVECTOR sz = XMVector3Length(worldMtx.r[2]);
                float maxScale = std::max({ XMVectorGetX(sx), XMVectorGetX(sy), XMVectorGetX(sz) });
                rd.boundsRadius = maxScale * 0.8660254f;

                // Decal world-space +Z = normalized row 2 of the world matrix.
                XMVECTOR zAxis = XMVector3Normalize(worldMtx.r[2]);
                XMStoreFloat3(&rd.decalForwardWS, zAxis);

                // Fill 9 bindless indices + scalars from the asset.
                using A = Resource::DecalMaterialAsset;
                rd.texBaseColor    = asset->texBindless[A::SLOT_BASECOLOR];
                rd.texNormal       = asset->texBindless[A::SLOT_NORMAL];
                rd.texOpacity      = asset->texBindless[A::SLOT_OPACITY];
                rd.texRoughness    = asset->texBindless[A::SLOT_ROUGHNESS];
                rd.texSpecular     = asset->texBindless[A::SLOT_SPECULAR];
                rd.texAO           = asset->texBindless[A::SLOT_AO];
                rd.texBump         = asset->texBindless[A::SLOT_BUMP];
                rd.texCavity       = asset->texBindless[A::SLOT_CAVITY];
                rd.texDisplacement = asset->texBindless[A::SLOT_DISPLACEMENT];

                rd.flags          = asset->flags;
                rd.angleFadeStart = asset->angleFadeStart;
                rd.sortLayer      = static_cast<float>(asset->sortLayer);

                // Tint: per-instance override wins; .a pre-multiplied with fadeAlpha.
                const bool tintOverridden = (dc->tintOverride.w > 0.f);
                const DirectX::XMFLOAT4 baseTint = tintOverridden ? dc->tintOverride : asset->baseColorTint;
                rd.baseColorTint = { baseTint.x, baseTint.y, baseTint.z, baseTint.w * dc->fadeAlpha };

                rd.scalars0 = { asset->opacity, asset->roughness, asset->specular, asset->ao };
                rd.scalars1 = { asset->normalStrength, asset->bumpStrength,
                                asset->cavityStrength, asset->displacementScale };

                resolvedDecals.push_back(rd);
            }
        }

        m_decalPass->SetDecals(resolvedDecals);

        // Camera data: invVP→apply CS world-pos; invProj+view→cull CS; camPos→displacement parallax.
        XMMATRIX view    = XMLoadFloat4x4(&m_view.viewMatrix);
        XMMATRIX proj    = XMLoadFloat4x4(&m_view.projMatrixNoJitter);
        XMMATRIX invP    = XMMatrixInverse(nullptr, proj);
        XMMATRIX viewProj = view * proj;
        XMMATRIX invVP   = XMMatrixInverse(nullptr, viewProj);
        XMFLOAT4X4 invProjF, invVPF, viewF;
        XMStoreFloat4x4(&invProjF, XMMatrixTranspose(invP));
        XMStoreFloat4x4(&invVPF,   XMMatrixTranspose(invVP));
        XMStoreFloat4x4(&viewF,    XMMatrixTranspose(view));
        m_decalPass->SetCamera(invProjF, invVPF, viewF,
                                m_camera.position,
                                m_camera.nearZ, m_camera.farZ,
                                m_vpWidth, m_vpHeight);
    }
}

// Pack ReflectionProbeComponents into GPU StructuredBuffer; FIFO slice assignment (kMaxReflectionProbes).
void Renderer::BuildScene_UploadProbes(World& world)
{
    using namespace Reflection;

    uint32_t activeCount = 0;
    m_probeMgr.SetActiveProbeCount(0);
    auto* lb = static_cast<LightCB*>(m_lightCBMapped);

    auto publishCount = [&]() {
        m_probeMgr.SetActiveProbeCount(activeCount);
        if (lb) lb->reflectionProbeCount = activeCount;
    };

    auto* dst = m_probeMgr.GetUploadPointer();
    if (!dst) { publishCount(); return; }

    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool)                     { publishCount(); return; }
    const auto& probeEnts = probePool->Entities();
    if (probeEnts.empty())              { publishCount(); return; }

    auto& probeData = probePool->Data();

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
        m_clusterPass->SetProbes(m_probeMgr.GetBufferSrv(), activeCount);
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

// CPU-side DDGI bookkeeping: collect volumes, alloc/resize, refresh CBs, publish to LightCB.
// GPU dispatches (TLAS build + trace + relight) live in Render().
void Renderer::BuildScene_UpdateDDGI(World& world)
{
    auto* lb = static_cast<LightCB*>(m_lightCBMapped);

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

        // Parallel arrays for manager's Tick — alloc/resize on first sight or probe-count change.
        const DDGIVolumeComponent*       vols[DDGI::kMaxVolumes] = {};
        DDGIVolumeRuntimeComponent*      runs[DDGI::kMaxVolumes] = {};

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

        // Trace CS picks random light per ray from cluster buffer; 0 → sky-miss + multi-bounce only.
        const uint32_t ddgiLightCount =
            m_clusterPass ? m_clusterPass->GetLightCount() : 0u;
        m_ddgiMgr.Tick(m_gfx, vols, runs, activeCount, ddgiLightCount);

        // GC orphaned slots — without this, lighting reads stale slot with wrong probe-count layout.
        uint32_t claimed[DDGI::kMaxVolumes] = {};
        for (uint32_t i = 0; i < activeCount; ++i)
            claimed[i] = runs[i] ? runs[i]->volumeSlot : 0xFFFFFFFFu;
        m_ddgiMgr.FreeUnclaimedSlots(m_gfx, claimed, activeCount);
    }

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
                m_ddgiMgr.GetVolumeBufferSrv(),
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

// Single-tile terrain sync: TextureSystem paths, TerrainCB upload, arm TerrainPass.
void Renderer::BuildScene_SyncTerrain(World& world)
{
    if (!m_terrainPass || !m_terrainCBMapped) return;

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

        std::memcpy(m_terrainCBMapped, &cb, sizeof(cb));
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

// ---------------------------------------------------------------------------
void Renderer::ProcessProbeBakeQueue(RHI::CommandList colorLastCL)
{
    if (m_probeMgr.BakeQueueEmpty())                return;
    if (!m_lastWorld)                               return;
    if (!m_probeMgr.GetArrayTexture().IsValid())    return;
    if (!m_materialBuffer.IsValid())                return;

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
        ctx.sunDir   = m_skyIBLPass->GetSunDir();
        ctx.sunColor = m_skyIBLPass->GetSunColor();
        ctx.ambient  = m_skyIBLPass->GetAmbientColor();
        ctx.skySHSrv = m_skyIBLPass->GetSHSrvHandle();
    }
    ctx.materialBufSrv   = m_gfx.GetBufferSRVGpuHandle(m_materialBuffer);
    ctx.bindlessTexTable = m_gfx.GetBindlessTextureTableGpuHandle();
    ctx.bindlessBufTable = m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle().ptr;
    ctx.instanceBuffer   = &m_instanceBuffer;
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

    m_probeMgr.GetCapturePass().BakeProbe(bakeCL, cubeSlice, ctx);

    probeComp->SetBaked(true);
    probeComp->ClearRebakeRequest();
    // Seed realtime tick so freshly-baked realtime probes wait a full interval.
    probeComp->lastBakedFrame = m_currentFrame;

   /* LOG_INFO("Renderer: baked probe slice %u (entity %u, BVH hits=%zu, draws O=%zu S=%zu T=%zu)",
             cubeSlice, probeEntity, bvhVisible.size(),
             ctx.opaqueDraws.size(), ctx.shadowDraws.size(), ctx.transparentDraws.size());*/
}

// ---------------------------------------------------------------------------
void Renderer::SyncSkyboxIBL(World& world)
{
    uint64_t irradianceHandle = 0;
    uint64_t radianceHandle   = 0;
    uint64_t skyboxHandle     = 0;
    uint32_t radianceMips     = 5;
    float    iblStrength      = 1.0f;

    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        SkyboxComponent* sc = world.GetComponent<SkyboxComponent>(e);
        if (!sc) continue;

        if (m_texSys && m_resMgr)
        {
            auto syncTex = [&](SkyboxTexEntry& entry, const std::string& path, uint64_t& outHandle)
            {
                if (path.empty()) return;
                if (entry.path != path)
                {
                    if (entry.handle != Resource::kInvalidTextureHandle)
                        m_texSys->Release(entry.handle, m_gfx);
                    entry.path   = path;
                    entry.handle = m_texSys->Acquire(path, *m_resMgr, m_gfx);
                    outHandle    = 0;
                }
                if (entry.handle != Resource::kInvalidTextureHandle && m_texSys->IsReady(entry.handle))
                    if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                        outHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
            };

            const uint64_t prevIrr = m_skyboxTexCache[0].lastLoggedHandle;
            const uint64_t prevRad = m_skyboxTexCache[1].lastLoggedHandle;
            syncTex(m_skyboxTexCache[0], sc->irradiancePath, irradianceHandle);
            syncTex(m_skyboxTexCache[1], sc->radiancePath,   radianceHandle);
            syncTex(m_skyboxTexCache[2], sc->skyboxPath,     skyboxHandle);

            if (irradianceHandle && irradianceHandle != prevIrr)
            {
                m_skyboxTexCache[0].lastLoggedHandle = irradianceHandle;
                LOG_INFO("SyncSkyboxIBL: irradiance handle ready (gpu=0x%llX)", irradianceHandle);
            }
            if (radianceHandle && radianceHandle != prevRad)
            {
                m_skyboxTexCache[1].lastLoggedHandle = radianceHandle;
                LOG_INFO("SyncSkyboxIBL: radiance handle ready (gpu=0x%llX)", radianceHandle);
            }

            sc->irradianceGpuHandle = irradianceHandle;
            sc->radianceGpuHandle   = radianceHandle;
            sc->skyboxGpuHandle     = skyboxHandle;
        }

        radianceMips = sc->radianceMipLevels;
        // SkyboxComponent.iblStrength ignored — global setting on SkyIBLPass (Post Processing panel).
        break; // only one skybox per scene
    }

    // Global IBL strength lives on SkyIBLPass.
    if (m_skyIBLPass)
        iblStrength = m_skyIBLPass->GetIBLStrength();

    // BRDF LUT — fixed asset, loaded once.
    static constexpr const char* kBRDFLUTPath = "asset/IBL/BRDF_LUT.itex";
    uint64_t brdfLutHandle = 0;
    if (m_texSys && m_resMgr)
    {
        if (m_brdfLutEntry.path.empty())
        {
            m_brdfLutEntry.path   = kBRDFLUTPath;
            m_brdfLutEntry.handle = m_texSys->Acquire(kBRDFLUTPath, *m_resMgr, m_gfx);
        }
        if (m_brdfLutEntry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(m_brdfLutEntry.handle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(m_brdfLutEntry.handle))
                brdfLutHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
        }
    }

    // SkyIBL: atmosphere drives SH/specular/backdrop by default. Disabled fallback priority: skybox > radiance > irradiance.
    uint64_t shSourceHandle = skyboxHandle ? skyboxHandle
                            : radianceHandle ? radianceHandle
                            : irradianceHandle;
    if (m_skyIBLPass)
    {
        m_skyIBLPass->SetSourceCubemap(shSourceHandle);

        // Feed camera state to AP LUT so it builds world-space view rays matching deferred lighting.
        if (m_lightCBMapped)
        {
            auto* lb = static_cast<LightCB*>(m_lightCBMapped);
            DirectX::XMFLOAT3 camPos{ lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
            DirectX::XMFLOAT3 camFwd{ lb->cameraForward[0], lb->cameraForward[1], lb->cameraForward[2] };
            m_skyIBLPass->SetCameraForAerial(camPos, camFwd, lb->invViewProj);
        }

        // TOD: when active SkyIBLPass owns the sun (no LightCB round-trip — that crushed night to 0.01 → flash).
        const bool todActive = m_skyIBLPass->TickTimeOfDay(m_deltaTime);

        if (todActive)
        {
            // TOD → LightCB so shading + CSM stay in sync with sky. LightCB.lightDir = -sunDir.
            const DirectX::XMFLOAT3& sd = m_skyIBLPass->GetSunDir();
            const DirectX::XMFLOAT3& sc = m_skyIBLPass->GetSunColor();
            if (m_lightCBMapped)
            {
                auto* lb = static_cast<LightCB*>(m_lightCBMapped);
                lb->lightDir[0] = -sd.x; lb->lightDir[1] = -sd.y; lb->lightDir[2] = -sd.z;
                lb->lightColor[0] = sc.x; lb->lightColor[1] = sc.y; lb->lightColor[2] = sc.z;
            }
            // Do NOT SetSunDir here — pass already authoritative.
        }
        else if (m_lightCBMapped)
        {
            // TOD off → directional light drives sun (0.01 floor only when nothing else provides color).
            auto* lb = static_cast<LightCB*>(m_lightCBMapped);
            DirectX::XMFLOAT3 sunDir{ -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
            DirectX::XMFLOAT3 sunCol{ lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
            const float minC = 0.01f;
            if (sunCol.x < minC && sunCol.y < minC && sunCol.z < minC)
                sunCol = { 10.0f, 10.0f, 10.0f };
            m_skyIBLPass->SetSunDir(sunDir, sunCol);
        }
    }

    const bool atmosphereOn = m_skyIBLPass && m_skyIBLPass->IsAtmosphereEnabled();
    // Static mode skips every compute pass — gate on atmosphereOn so stale LUTs don't leak.
    const bool skyUseSH     = atmosphereOn
                           && m_skyIBLPass && m_skyIBLPass->IsSHValid();

    // Atmosphere on → ensure non-zero IBL even without SkyboxComponent.
    if (atmosphereOn && iblStrength <= 0.0f)
        iblStrength = 1.0f;

    // Effective mip count may come from SkyIBLPass's pre-filtered cube vs static radiance.
    uint32_t cbRadianceMips = radianceMips;
    if (m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
        cbRadianceMips = m_skyIBLPass->GetSpecularMipCount();

    if (m_lightCBMapped)
    {
        auto* lb = static_cast<LightCB*>(m_lightCBMapped);
        lb->iblRadianceMips = cbRadianceMips;
        lb->iblStrength     = iblStrength;
        lb->iblUseSH        = skyUseSH ? 1u : 0u;
        // Per-frame ambient (editor slider on SkyIBLPass). Black = IBL-only indirect.
        if (m_skyIBLPass)
        {
            const DirectX::XMFLOAT3& a = m_skyIBLPass->GetAmbientColor();
            lb->ambient[0] = a.x; lb->ambient[1] = a.y; lb->ambient[2] = a.z;
        }
        // AP composite gated on atmosphere+valid+opt-in; first frame's ap.a=0 would multiply scene→black.
        const bool apReady = m_skyIBLPass
                          && m_skyIBLPass->IsAtmosphereEnabled()
                          && m_skyIBLPass->IsAerialValid()
                          && m_skyIBLPass->IsAerialCompositeEnabled();
        lb->aerialMaxDistKm = apReady ? m_skyIBLPass->GetAerialMaxDistanceKm() : 0.0f;
    }

    // Atmosphere → prefer live SkyIBLPass specular; static → loaded .itex (SkyIBLPass cube would be stale).
    uint64_t effectiveRadiance = radianceHandle;
    uint32_t effectiveRadianceMips = radianceMips;
    if (atmosphereOn && m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
    {
        const uint64_t spec = m_skyIBLPass->GetSpecularSrvHandle();
        if (spec)
        {
            effectiveRadiance     = spec;
            effectiveRadianceMips = m_skyIBLPass->GetSpecularMipCount();
        }
    }

    // SSR composite cache (same handle as LightingPass split-sum BRDF LUT).
    m_brdfLutSrv             = brdfLutHandle;
    // DDGI miss samples RADIANCE cube (sun disk preserved); irradiance would smear sun → L1 SH ≈ 0.
    m_skyRadianceSrvForDDGI  = effectiveRadiance;

    // Forward handles to passes
    if (m_lightingPass)
    {
        m_lightingPass->SetIBL(irradianceHandle, effectiveRadiance, effectiveRadianceMips, iblStrength);
        m_lightingPass->SetBRDFLUT(brdfLutHandle);
        m_lightingPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0,
                                 skyUseSH);
        // AP: 3D LUT + max distance. Handle 0 → 1×1×1 fallback binds (inscatter=0, T=1, no-op).
        if (m_skyIBLPass)
            m_lightingPass->SetAerialPerspective(
                m_skyIBLPass->GetAerialPerspectiveSrvHandle(),
                m_skyIBLPass->GetAerialMaxDistanceKm());
    }
    if (m_transparentPass)
    {
        // Mirror LightingPass: procedural specular cube when atmosphere on (forward agrees w/ deferred).
        // Diffuse stays on irradiance cube; iblUseSH=1 routes forward through EvalSH2.
        m_transparentPass->SetIBL(irradianceHandle, effectiveRadiance,
                                  effectiveRadianceMips, iblStrength);
        m_transparentPass->SetBRDFLUT(brdfLutHandle);
        m_transparentPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    if (m_skyboxPass)
    {
        const uint64_t staticFallback = skyboxHandle ? skyboxHandle
                                      : radianceHandle ? radianceHandle
                                      : irradianceHandle;
        const uint64_t resolved = m_skyIBLPass
            ? m_skyIBLPass->ResolveSkyboxSrvHandle(staticFallback)
            : staticFallback;
        m_skyboxPass->SetEnvMap(resolved);

        // Analytic sun disk in PS for pixel-sharp result; uses whichever sun drives the atmosphere.
        if (m_skyIBLPass)
        {
            const DirectX::XMFLOAT3& sd = m_skyIBLPass->GetSunDir();
            const DirectX::XMFLOAT3& sc = m_skyIBLPass->GetSunColor();
            // Real solar half-angle ≈ 0.27° (0.00465 rad); slightly tighter for crisp point.
            m_skyboxPass->SetSun(sd, sc, /*half-angle rad*/ 0.005f,
                                 /*intensity*/ 15.0f);

            if (m_moonHandle == Resource::kInvalidTextureHandle && m_texSys && m_resMgr)
                m_moonHandle = m_texSys->Acquire(
                    "asset/Default_Texture/moon.itex", *m_resMgr, m_gfx);
            if (m_moonSRV == 0 && m_texSys
                && m_moonHandle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(m_moonHandle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(m_moonHandle))
                    m_moonSRV = m_gfx.GetTextureSRVGpuHandle(*tex);
            }

            // Moon disk independent of active-body lighting; fades smoothly across horizon (~1.5°).
            m_skyboxPass->SetMoon(m_moonSRV,
                                  m_skyIBLPass->IsMoonDiskVisible(),
                                  m_skyIBLPass->GetMoonDir(),
                                  m_skyIBLPass->GetMoonColor(),
                                  /*half-angle rad*/ 0.026f);

            // Stars drive off MOON altitude (not GetSunDir which is active lighting body).
            // smoothstep(-0.20, 0.10, moonY): below=day/no stars; above=night/max; matches moon fade-in.
            float moonY = m_skyIBLPass->GetMoonDir().y;
            auto smoothstepF = [](float e0, float e1, float x) {
                float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            float nightAlpha = smoothstepF(-0.20f, 0.10f, moonY);

            // Star twinkle: wraps every ~10 min for sin-phase float precision.
            static float s_starTime = 0.0f;
            s_starTime = std::fmod(s_starTime + m_deltaTime, 600.0f);

            m_skyboxPass->SetStars(nightAlpha, s_starTime);
        }
    }
}



// ---------------------------------------------------------------------------
void Renderer::UploadFrameData(FrameIndex /*frame*/, uint32_t vpW, uint32_t vpH)
{
    if (vpW == 0 || vpH == 0)  return;
    if (!m_perObjectCBMapped)   return;

    m_vpWidth  = vpW;
    m_vpHeight = vpH;

    // Build view from current camera orientation.
    const float cp = std::cos(m_camera.pitch);
    const float sp = std::sin(m_camera.pitch);
    const float cy = std::cos(m_camera.yaw);
    const float sy = std::sin(m_camera.yaw);
    const XMVECTOR forward = XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
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
    std::memcpy(m_perObjectCBMapped, &cb, sizeof(cb));

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
    if (m_lightCBMapped)
    {
        auto* lb = static_cast<LightCB*>(m_lightCBMapped);
        lb->cameraPos[0] = m_camera.position.x;
        lb->cameraPos[1] = m_camera.position.y;
        lb->cameraPos[2] = m_camera.position.z;

        XMMATRIX invVP = XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjJittered));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->invViewProj), invVP);

        m_taaJitter.CommitFrame(viewProjNoJitter, viewProjJittered, invVP);
    }

    // ---- Compute cascade shadow matrices ------------------------------------
    if (m_shadowSystem && m_lightCBMapped)
    {
        auto* lb = static_cast<LightCB*>(m_lightCBMapped);

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
    // ---- PerViewCB ----------------------------------------------------------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = kPerViewCBSize;
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!m_gfx.CreateBuffer(desc, m_perObjectCB))
        {
            m_perObjectCB.Reset();
            return;
        }
        m_perObjectCBMapped = m_gfx.MapBuffer(m_perObjectCB);
    }
    {
        m_vpWidth  = static_cast<uint32_t>(m_gfx.GetWidth());
        m_vpHeight = static_cast<uint32_t>(m_gfx.GetHeight());
        UploadFrameData(0, m_vpWidth, m_vpHeight);
    }

    // ---- TerrainParams CB (UPLOAD, mapped) ---------------------------------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = kTerrainCBSize;
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (m_gfx.CreateBuffer(desc, m_terrainCB))
            m_terrainCBMapped = m_gfx.MapBuffer(m_terrainCB);
        else
            LOG_ERROR("Renderer: TerrainCB creation failed");
    }

    // ---- InstanceBuffer (GPUInstanceData per instance, UPLOAD heap) ---------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxInstances) * sizeof(GPUInstanceData);
        desc.stride     = sizeof(GPUInstanceData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        if (!m_gfx.CreateBuffer(desc, m_instanceBuffer))
        {
            LOG_ERROR("Renderer: InstanceBuffer creation failed");
            return;
        }
        m_instanceBufferMapped = m_gfx.MapBuffer(m_instanceBuffer);
    }

    // ---- ExecuteIndirect buffers --------------------------------------------
    {
        const uint64_t argSize = static_cast<uint64_t>(kMaxInstances) * sizeof(IndirectDrawCommand);

        // DEFAULT heap: GPU-side indirect arg buffer
        RHI::GPUBufferDesc desc;
        desc.size       = argSize;
        desc.stride     = sizeof(IndirectDrawCommand);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bind_flags = RHI::BindFlag::UNORDERED_ACCESS; // GPU culling will write here
        m_gfx.CreateBuffer(desc, m_indirectArgBuffer);

        // UPLOAD heap: CPU staging
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::NONE;
        if (m_gfx.CreateBuffer(desc, m_indirectArgUpload))
            m_indirectArgMapped = m_gfx.MapBuffer(m_indirectArgUpload);

        // Draw count buffer (4 bytes)
        desc.size       = sizeof(uint32_t);
        desc.stride     = sizeof(uint32_t);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bind_flags = RHI::BindFlag::UNORDERED_ACCESS;
        m_gfx.CreateBuffer(desc, m_drawCountBuffer);

        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::NONE;
        if (m_gfx.CreateBuffer(desc, m_drawCountUpload))
            m_drawCountMapped = m_gfx.MapBuffer(m_drawCountUpload);

        LOG_INFO("Renderer: ExecuteIndirect buffers ready (max %u commands)", kMaxInstances);
    }

    // ---- LightCB ------------------------------------------------------------
    {
        RHI::GPUBufferDesc desc;
        desc.size       = kLightCBSize;
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!m_gfx.CreateBuffer(desc, m_lightCB))
        {
            m_lightCB.Reset();
            return;
        }
        m_lightCBMapped = m_gfx.MapBuffer(m_lightCB);
    }
    {
        auto norm3 = [](float x, float y, float z, float o[3]) {
            float l = std::sqrt(x*x + y*y + z*z);
            o[0] = x/l; o[1] = y/l; o[2] = z/l;
        };
        LightCB lb{};
        norm3(1.f, -2.f, 0.5f, lb.lightDir);
        lb.lightColor[0] = 1.f;  lb.lightColor[1] = 0.92f; lb.lightColor[2] = 0.82f;
        lb.ambient[0]    = 0.1f; lb.ambient[1]    = 0.12f; lb.ambient[2]    = 0.18f;
        lb.cameraPos[0]  = m_camera.position.x;
        lb.cameraPos[1]  = m_camera.position.y;
        lb.cameraPos[2]  = m_camera.position.z;
        std::memcpy(m_lightCBMapped, &lb, sizeof(lb));
    }

    // ---- SpotShadow VP matrix buffer; persistent-mapped, transposed on write.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(SpotShadowPass::kMaxCasters)
                        * sizeof(DirectX::XMFLOAT4X4);
        desc.stride     = sizeof(DirectX::XMFLOAT4X4);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (m_gfx.CreateBuffer(desc, m_spotShadowVPBuffer))
        {
            m_spotShadowVPMapped = m_gfx.MapBuffer(m_spotShadowVPBuffer);
            m_spotShadowVPSrv    = m_gfx.GetBufferSRVGpuHandle(m_spotShadowVPBuffer);
            if (m_spotShadowVPMapped)
            {
                DirectX::XMFLOAT4X4 ident;
                DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
                auto* dst = static_cast<DirectX::XMFLOAT4X4*>(m_spotShadowVPMapped);
                for (uint32_t i = 0; i < SpotShadowPass::kMaxCasters; ++i)
                    dst[i] = ident;
            }
        }
    }

    // ---- MaterialBuffer (StructuredBuffer<MaterialGPUData>, UPLOAD heap) ----
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxMaterials) * sizeof(Resource::MaterialGPUData);
        desc.stride     = sizeof(Resource::MaterialGPUData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!m_gfx.CreateBuffer(desc, m_materialBuffer))
        {
            LOG_ERROR("Renderer: MaterialBuffer creation failed");
            return;
        }
        m_materialBufferMapped = m_gfx.MapBuffer(m_materialBuffer);

        // Pre-fill with default PBR values so slot 0 always has valid data.
        if (m_materialBufferMapped)
        {
            auto* data = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped);
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

    // SSR chain (see SSR_HiZ_Architecture_Prompt.md): DepthHier → trace → Resolve → Temporal → Upsample → Composite.
    m_ssrPass = std::make_unique<SSRPass>();
    m_ssrPass->Init(m_gfx);

    m_ssrResolvePass = std::make_unique<SSRResolvePass>();
    m_ssrResolvePass->Init(m_gfx);

    m_ssrTemporalPass = std::make_unique<SSRTemporalPass>();
    m_ssrTemporalPass->Init(m_gfx);

    m_ssrUpsamplePass = std::make_unique<SSRUpsamplePass>();
    m_ssrUpsamplePass->Init(m_gfx);

    m_ssrCompositePass = std::make_unique<SSRCompositePass>();
    m_ssrCompositePass->Init(m_gfx);

    m_ssrDepthHierPass = std::make_unique<SSRDepthHierarchyPass>();
    m_ssrDepthHierPass->Init(m_gfx);

    // Karis firefly HDR mip pyramid for SSR resolve cone-footprint sampling.
    m_sceneColorPyramidPass = std::make_unique<SceneColorPyramidPass>();
    m_sceneColorPyramidPass->Init(m_gfx);

    // Lighting reads UPSAMPLE output (1-frame latent) to dampen its specIBL by (1 - ssrConf).
    if (m_lightingPass && m_ssrUpsamplePass)
    {
        const uint32_t rw = m_gfx.GetRenderWidth();
        const uint32_t rh = m_gfx.GetRenderHeight();
        m_ssrPass->EnsureTexture(rw, rh);
        m_ssrResolvePass->EnsureTextures(rw, rh);
        m_ssrTemporalPass->EnsureTextures(rw, rh);
        m_ssrUpsamplePass->EnsureTexture(rw, rh);
        m_lightingPass->SetSSRResult(m_ssrUpsamplePass->GetColorSrv());
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


