#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/GPUInstanceData.h"
#include "Graphics/IndirectDrawCommand.h"

// render passes
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"

// ECS components + systems
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/BillboardComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/MaterialReflectionSync.h"
#include "ECS/VFXSpawnRequests.h"
#include "ECS/CharacterControllerComponent.h"   // KCC capsule debug-wire

// resource system
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialSystem.h"
#include "Resource/ProceduralMesh.h"

// scene
#include "Scene/MeshSpawner.h"

// engine systems
#include "System/Log.h"
#include "System/TaskSystem.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <execution>
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

// Renderer_Scene.cpp - split out of Renderer.cpp (one TU per Renderer subsystem).
// Owns the ECS->DrawPacket pipeline: BuildRenderScene orchestrator + its
// BuildScene_* gather/sort/emit helpers, material-texture sync, the unified
// mesh-VFX drain, and the file-local material-packing helpers (anon namespace).
// All members belong to class Renderer (Graphics/Renderer.h). Include block
// mirrors Renderer.cpp so the cluster keeps compiling.

// ---- file-local material-packing helpers (were anon-namespace in Renderer.cpp) ----
namespace
{
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
// Unified VFX — resolve runtime Mesh-VFX emitters. VFXSpawnSystem already
// created the renderable entity (transform / attach / lifetime / default
// material); here we load its mesh path into the MeshLibrary and patch on a
// MeshLibRef (+ authored material) so BuildRenderScene picks it up this frame.
void Renderer::DrainMeshVFXSpawns(World& world)
{
    if (!m_meshLib) return;

    // Collect-then-resolve: AddComponent<MeshLibRef> below mutates pools, so we
    // drain the mailbox into a local list first to keep its view stable.
    std::vector<PendingMeshVFXSpawns::Req> reqs;
    world.ForEach<PendingMeshVFXSpawns>([&](Entity, PendingMeshVFXSpawns& mb){
        for (auto& r : mb.reqs) reqs.push_back(std::move(r));
        mb.reqs.clear();
    });

    for (auto& r : reqs)
    {
        if (!world.IsHandleValid(r.entity)) continue;   // VFX entity already gone
        const Entity e = r.entity.entity;
        if (r.meshPath.empty()) continue;

        // NOTE: MeshLibrary::Load is path-deduplicated + refcounted. We do not
        // currently Release on VFX-entity destroy, so the library slot's
        // refcount is not balanced until the world reloads (m_meshLib teardown
        // frees it regardless). Because of dedup this is one GPU buffer per
        // unique mesh path, not a per-spawn leak — acceptable for the demo seam;
        // a Release-on-destroy hook in OnEntityDestroyed is the proper follow-up.
        Resource::Handle h = m_meshLib->Load(r.meshPath, m_gfx);
        if (!h.IsValid()) {
            LOG_WARNING("VFX mesh: failed to load '%s'", r.meshPath.c_str());
            continue;
        }

        // Default-constructed cache fields force a fresh descriptor resolve in
        // BuildRenderScene (cachedGeneration 0 != current generation).
        MeshLibRef ref;
        ref.libHandle = h;
        ref.meshId    = 0;                  // VFX mesh files carry a single mesh
        world.AddComponent<MeshLibRef>(e, ref);

        // Load the authored material into the entity's pre-created
        // MaterialComponent (mirrors MeshSpawner). Empty path keeps the default.
        if (!r.materialPath.empty())
            if (auto* mc = world.GetComponent<MaterialComponent>(e)) {
                if (Resource::LoadMaterial(r.materialPath, *mc))
                    mc->SetDirty();
            }

        (void)r.castShadow;   // TODO: map to VisibilityComponent CastShadow flag
    }
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
            slot.gpuHandle     = 0;  // clear until load completes
            slot.bindlessIndex = -1; // stop feeding the old (recycled) bindless
                                     // slot to shaders until the new tex loads
                                     // (mirrors SyncMaterialCustomTextures)
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
// Material → filter / permutation / stencil ref / view-space depth from a
// MaterialComponent. Promoted from a BuildRenderScene-local lambda so the
// per-source gather helpers can share it. `camForward` is the current-frame
// camera forward (m_view is only updated after BuildRenderScene).
void Renderer::ApplyBlendMode(DrawCandidate& c, const XMFLOAT3& camForward)
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
    // Cast-shadow is AND of two layers (see DesignMd/ecs_visibility_design.md §4):
    //   Material::CAST_SHADOW       — art-time per-asset opt-out (glass etc.)
    //   VisibilityComponent.CastShadow — runtime per-entity toggle (stealth etc.)
    // Visibility writes c.castShadow BEFORE ApplyBlendMode runs (primitive +
    // MeshLibRef paths), so AND-ing here preserves both. Billboards / beams
    // never call ApplyBlendMode without a material; their default castShadow=1
    // means the material flag alone gates them.
    c.castShadow = (c.castShadow && c.mc->IsCastingShadow()) ? 1 : 0;

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
        c.depth = dx * camForward.x
                + dy * camForward.y
                + dz * camForward.z;
    }
}

// ---------------------------------------------------------------------------
// Lazy-resolve customShaderPath → dynamic PS id; stash in MaterialComponent on
// first hit. Post-resolve, sync custom-param maps to shader reflection
// (preserves existing user values).
//
// Register in BOTH GBufferPass and TransparentPass shader libraries — each
// pass owns its own ShaderLibrary + PSOCache, so without mirroring the
// registration in TransparentPass, custom-PS additive draws (afterimages,
// beam outer-glow) fail PSO lookup ("PS not found id=N"). Both registries
// assign sequential IDs starting at ShaderID::Count, so as long as the
// call order is identical the IDs stay in sync.
uint32_t Renderer::ResolveCustomPSID(const MaterialComponent& m)
{
    if (!m.useCustomShader || m.customShaderPath.empty()) return 0;
    if (m.customShaderID < 0 && m_gbufferPass)
    {
        const uint32_t id = m_gbufferPass->GetShaderLibrary().RegisterDynamic(
            m.customShaderPath.c_str(), RHI::ShaderStage::PS);
        m.customShaderID =
            (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);

        // Mirror into TransparentPass's library so additive / alpha draws
        // can find the same id. The transparent path is opt-in per material
        // (BlendMode != Opaque), but registering unconditionally is cheap.
        if (m.customShaderID > 0 && m_transparentPass)
        {
            m_transparentPass->GetReloadableShaderLibrary()->RegisterDynamic(
                m.customShaderPath.c_str(), RHI::ShaderStage::PS);
        }

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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_SyncMaterials(World& world)
{
    auto* pMaterial = world.GetPool<MaterialComponent>();
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

    // Flush deferred texture releases queued by the previous OnWorldClear.
    // Order is critical: the Phase 0 loop above already Acquired every
    // texture path used by the NEW world, so any path shared with the
    // outgoing world has refcount=2 in TextureSystem. Releasing now drops
    // back to 1 — the slot stays alive and the GPU upload is preserved.
    // Paths NOT in the new world drop to 0 → FreeSlot → m_pendingDestroy
    // (TextureSystem::Tick frees on the next BeginFrame, already deferred).
    if (m_texSys && !m_pendingMatTexReleaseAfterNextSync.empty())
    {
        for (Resource::TextureHandle h : m_pendingMatTexReleaseAfterNextSync)
            m_texSys->Release(h, m_gfx);
        m_pendingMatTexReleaseAfterNextSync.clear();
    }
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_CullBVH(World& world,
                                  std::vector<uint8_t>& bvhVisibleMask,
                                  std::vector<uint8_t>& bvhTestedMask)
{
    auto* pMeshLibRef = world.GetPool<MeshLibRef>();
    auto* pWorldAabb  = world.GetPool<WorldAabb>();
    auto* pSkinned    = world.GetPool<MeshSkinnedComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    Entity maxEntity = 0;
    for (Entity e : world.GetEntities())
        if (e > maxEntity) maxEntity = e;
    bvhVisibleMask.assign(static_cast<size_t>(maxEntity) + 1, 0);
    bvhTestedMask .assign(static_cast<size_t>(maxEntity) + 1, 0);
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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_DebugWireframes(World& world)
{
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

        // CharacterController (KCC) capsule collision — shown under Show
        // Collision (it's the gameplay collision shape), alongside the static
        // mesh-collider wireframe that CollisionMesh::EmitDebugWireframe adds in
        // App.cpp. PhysicsSystem writes the capsule FEET into the transform
        // (cv->GetPosition(); the Jolt shape is offset up by halfHeight+radius
        // via mShapeOffset) and the capsule is always world-upright (the KCC
        // never rotates it), so rebuild the two hemisphere-segment centers
        // straight from the entity's world position + the component's dims.
        if (m_debugWirePass->showCollision)
        {
            if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
            {
                const auto& ccEnts = ccPool->Entities();
                auto&       ccData = ccPool->Data();
                for (size_t i = 0; i < ccEnts.size(); ++i)
                {
                    const Entity e = ccEnts[i];
                    if (!world.IsAlive(e)) continue;
                    const CharacterControllerComponent& cc = ccData[i];

                    XMFLOAT3 feet;
                    if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e))
                        feet = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
                    else if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
                        feet = lt->translation;
                    else
                        continue;

                    const float r  = cc.capsuleRadius;
                    const float hh = cc.capsuleHalfHeight;
                    const XMFLOAT3 segLo{ feet.x, feet.y + r,            feet.z };
                    const XMFLOAT3 segHi{ feet.x, feet.y + 2.f * hh + r, feet.z };
                    m_debugWirePass->AddCapsule(segLo, segHi, r, 0xFF00FF00); // green
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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_GatherPrimitivesAndLights(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    auto* pLightData  = world.GetPool<LightData>();
    auto* pVolLight   = world.GetPool<VolumetricLightComponent>();
    auto* pGlobalXf   = world.GetPool<GlobalTransform>();
    auto* pMeshHandle = world.GetPool<MeshHandle>();
    auto* pVisibility = world.GetPool<VisibilityComponent>();
    auto* pMaterial   = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    auto* lb = m_lightCB.Current(m_gfx);
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
            // Honour VisibilityComponent (explicit + inheritedHidden propagation from TransformSystem).
            uint32_t primViewMask  = ViewBit::All;
            bool     primMainPass  = true;
            bool     primCastShadow = true;
            if (const VisibilityComponent* v = pGet(pVisibility, e))
            {
                if (!v->IsEffectivelyVisible()) continue;
                primViewMask   = v->viewMask;
                primMainPass   = v->RendersInMainPass();
                primCastShadow = v->CastsShadow();
            }

            const auto& mesh = m_meshMgr.GetPrimitive(mh->gpuMeshID);
            if (mesh.meshDescSlot == RHI::kInvalidBufferIndex) continue;

            // GlobalTransform is the single source of truth.
            const GlobalTransform* gt = pGet(pGlobalXf, e);
            if (!gt) continue;
            const XMFLOAT4X4 worldMtx = gt->matrix;

            DrawCandidate c;
            c.entity       = e;
            c.viewMask          = primViewMask;
            c.renderInMainPass  = primMainPass;
            c.castShadow        = primCastShadow ? 1u : 0u;
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
                c.customPSID    = ResolveCustomPSID(*c.mc);
            }
            ApplyBlendMode(c, camForwardThisFrame);
            candidates.push_back(c);
            // Custom candidate for outlines: push-then-mutate avoids full DrawCandidate memcpy.
            // Editor picking-outline forces this path on even when the material flag is off.
            const bool materialOutline = c.mc && c.mc->IsOutlineEnabled()
                                         && c.filter == DrawFilter::Opaque;
            // Picking outline rides on sub-passes 2+3 (silhouette mask + screen-space),
            // so it works for transparent entities too — material outline can't because
            // it relies on inverted-hull blending which assumes opaque depth.
            const bool pickingOutline  = c.mc
                                         && m_pickingOutlineEntity != NullEntity
                                         && e == m_pickingOutlineEntity
                                         && (c.filter == DrawFilter::Opaque
                                             || c.filter == DrawFilter::Transparent);
            if (materialOutline || pickingOutline)
            {
                // Picking outline takes visual priority over material outline.
                const bool  drawPicking        = pickingOutline;
                const float outlinePixels      = drawPicking ? 0.0f
                                                 : c.mc->outlinePixels;
                const bool  screenSpaceOutline = drawPicking ? true
                                                 : c.mc->IsOutlineScreenSpaceEnabled();
                candidates.push_back(c);
                DrawCandidate& outline = candidates.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
                outline.isPickingOutline   = drawPicking;
            }
            continue;
        }

        // Scene-mesh entities now live in the MeshLibRef pass below.
    }
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_GatherMeshLibRefs(World& world,
        std::vector<DrawCandidate>& candidates,
        const std::vector<uint8_t>& bvhTestedMask,
        const std::vector<uint8_t>& bvhVisibleMask,
        const XMFLOAT3& camForwardThisFrame)
{
    auto* pMeshLibRef = world.GetPool<MeshLibRef>();
    auto* pGlobalXf   = world.GetPool<GlobalTransform>();
    auto* pVisibility = world.GetPool<VisibilityComponent>();
    auto* pSkinned    = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut    = world.GetPool<SkinningOutputComponent>();
    auto* pWorldAabb  = world.GetPool<WorldAabb>();
    auto* pMaterial   = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    auto inMask = [](const std::vector<uint8_t>& m, Entity e) -> bool {
        return e < m.size() && m[e] != 0;
    };
    if (m_meshLib)
    {
        struct MlrJob {
            Entity                 e;
            const MeshLibRef*      mlr;
            const GlobalTransform* gt;
            uint32_t               meshDescSlot;
            uint32_t               indexCount;
            uint32_t               viewMask;        // Phase 3: copied into DrawPacket
            bool                   castShadowVis;   // VisibilityComponent.CastShadow bit
            bool                   renderMainPass;  // VisibilityComponent.RenderInMainPass bit
        };
        // Local (NOT thread_local): worker lambdas read by ref; thread_local trips subscript asserts.
        std::vector<MlrJob> mlrJobs;
        // Phase 2 tag pools — read once, branch on presence in the gather loop.
        // BVH leaves intentionally include hidden entities (design 5.1: refit-only on
        // toggle) so structural rebuilds aren't triggered by per-frame visibility flips.
        auto* pDisabledTag    = world.GetPool<DisabledTag>();
    #ifndef WITH_EDITOR
        auto* pEditorOnlyTag  = world.GetPool<EditorOnlyTag>();
        auto* pHiddenInGame   = world.GetPool<HiddenInGameTag>();
    #endif
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

                // ---- Phase 1 author-intent filter (runs before BVH-driven OBB work).
                // Pre-default: if there's no VisibilityComponent at all we treat the
                // entity as fully visible across all views (design §8.4: missing-mask
                // entities must not silently disappear).
                uint32_t entityViewMask  = ViewBit::All;
                bool     entityCastShadow = true;
                bool     entityMainPass   = true;
                if (const VisibilityComponent* v = pGet(pVisibility, e))
                {
                    if (!v->IsEffectivelyVisible()) continue;
                    entityViewMask   = v->viewMask;
                    entityCastShadow = v->CastsShadow();
                    entityMainPass   = v->RendersInMainPass();
                }
                // Phase 2 structural-hide tags — always skip Disabled; in Game builds
                // also skip Editor-only / HiddenInGame entities.
                if (pDisabledTag    && pDisabledTag   ->Get(e)) continue;
            #ifndef WITH_EDITOR
                if (pEditorOnlyTag  && pEditorOnlyTag ->Get(e)) continue;
                if (pHiddenInGame   && pHiddenInGame  ->Get(e)) continue;
            #endif
                // ----

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
                mlrJobs.push_back({ e, &mlr, gt, slot, indexCountForDraw,
                                    entityViewMask, entityCastShadow, entityMainPass });
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
                    // Shadow-only fallback gated by BOTH layers (see applyBlendMode).
                    if (!j.castShadowVis) return;
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
                    if (!j.castShadowVis) return;
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
            // Phase 3: stamp the entity's visibility decisions onto the candidate
            // so per-pass routing (ShadowPass viewMask, main-pass opt-out) is a
            // straight bitfield check downstream — no per-pass ECS lookups.
            c.viewMask          = j.viewMask;
            c.renderInMainPass  = j.renderMainPass;
            c.castShadow        = j.castShadowVis ? 1u : 0u;
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
                ApplyBlendMode(c, camForwardThisFrame);
            }
            out.push_back(c);

            // Outline path: push-then-mutate avoids full DrawCandidate memcpy.
            // Editor picking-outline forces this path on even when the material flag is off.
            const bool materialOutline = !isShadowOnly && c.mc && c.mc->IsOutlineEnabled()
                                         && c.filter == DrawFilter::Opaque;
            // Picking outline works for both opaque and transparent (sub-pass 2/3 path).
            const bool pickingOutline  = !isShadowOnly && c.mc
                                         && m_pickingOutlineEntity != NullEntity
                                         && j.e == m_pickingOutlineEntity
                                         && (c.filter == DrawFilter::Opaque
                                             || c.filter == DrawFilter::Transparent);
            if (materialOutline || pickingOutline)
            {
                // Picking outline takes visual priority; must flow through sub-passes 2+3.
                const bool  drawPicking        = pickingOutline;
                const float outlinePixels      = drawPicking ? 0.0f
                                                 : c.mc->outlinePixels;
                const bool  screenSpaceOutline = drawPicking ? true
                                                 : c.mc->IsOutlineScreenSpaceEnabled();
                out.push_back(c);
                DrawCandidate& outline = out.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
                outline.isPickingOutline   = drawPicking;
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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_GatherBillboards(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    auto* pBillboard = world.GetPool<BillboardComponent>();
    auto* pGlobalXf  = world.GetPool<GlobalTransform>();
    auto* pLightData = world.GetPool<LightData>();
    auto* pMaterial  = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
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

            // Editor toggle: hide all light-icon billboards globally. Skips at
            // DrawCandidate construction so the quads never reach any pass.
            if (!m_lightBillboardsVisible && pGet(pLightData, e)) continue;

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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_GatherBeams(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    auto* pGlobalXf = world.GetPool<GlobalTransform>();
    auto* pMaterial = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
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

                ApplyBlendMode(c, camForwardThisFrame);
                c.customPSID = ResolveCustomPSID(*mc);

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
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_GatherAfterimages(
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    if (m_afterimageSystem && m_afterimageSystem->IsInitialised())
    {
        auto& slots = m_afterimageSystem->GetSlotsMutable();
        for (uint32_t ai = 0; ai < AfterimageSystem::kMaxSnapshots; ++ai)
        {
            auto& slot = slots[ai];
            if (!slot.alive) continue;
            if (slot.vertexCount == 0 || slot.indexCount == 0) continue;
            if (slot.meshDescSlot == 0xFFFFFFFFu)              continue;

            DrawCandidate c;
            c.entity       = NullEntity; // no ECS backing — picking ignores
            c.meshDescSlot = slot.meshDescSlot;
            c.indexCount   = slot.indexCount;

            // World matrix — transposed for HLSL row-vec convention (same as BeamComponent path).
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&slot.worldMatrix)));
            c.worldCenter = slot.worldCenter;

            c.mc = &slot.material;
            // No textures — ghost PS is purely procedural.
            c.texBaseColor  = 0;
            c.texSurfaceMap = 0;
            c.texNormalMap  = 0;

            ApplyBlendMode(c, camForwardThisFrame);
            c.customPSID = ResolveCustomPSID(slot.material);

            // View-space Z for transparent painter sort.
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

// ---------------------------------------------------------------------------
void Renderer::BuildScene_SortAndEmit(const std::vector<DrawCandidate>& candidates)
{
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
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    auto* instances = static_cast<GPUInstanceData*>(m_instanceBufferMapped[frameSlot]);
    auto* matBuf   = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped[frameSlot]);
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
                cur.stencilRef    != first.stencilRef   ||
                cur.meshDescSlot  != first.meshDescSlot  ||
                cur.texBaseColor  != first.texBaseColor  ||
                cur.texSurfaceMap != first.texSurfaceMap ||
                cur.texNormalMap  != first.texNormalMap  ||
                cur.matHash       != first.matHash       ||
                cur.outlinePixels != first.outlinePixels ||
                cur.isPickingOutline != first.isPickingOutline ||
                cur.customPSID    != first.customPSID    ||
                cur.viewMask      != first.viewMask      ||
                cur.castShadow    != first.castShadow    ||
                cur.renderInMainPass != first.renderInMainPass)
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
                // Prev-frame world for TAA velocity. Look up; default to current
                // on first sight (zero velocity). Update the cache so next frame
                // sees this frame's world as its prev. Multiple DrawCandidates
                // per entity (e.g. opaque + outline copy) share worldMatrix so
                // the overwrite is idempotent within the frame.
                auto pwIt = m_prevWorldMatrices.find(ck.entity);
                inst.prevWorld = (pwIt != m_prevWorldMatrices.end())
                    ? pwIt->second
                    : ck.worldMatrix;
                m_prevWorldMatrices[ck.entity] = ck.worldMatrix;
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
        dp.isPickingOutline    = first.isPickingOutline;
        dp.stencilRef          = first.stencilRef;
        dp.shadowCullMode      = first.shadowCullMode;
        dp.castShadow          = first.castShadow;
        dp.viewMask            = first.viewMask;
        dp.renderInMainPass    = first.renderInMainPass ? 1u : 0u;
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
        if (m_indirectArgMapped[frameSlot])
        {
            auto* indirectArgs = static_cast<IndirectDrawCommand*>(m_indirectArgMapped[frameSlot]);
            uint32_t cmdIdx = static_cast<uint32_t>(m_drawPackets.size()) - 1;
            if (cmdIdx < kMaxInstances)
            {
                IndirectDrawCommand& ic = indirectArgs[cmdIdx];
                ic.meshDescIdx     = dp.meshDescriptorIndex;
                ic.instanceOffset  = dp.instanceOffset;
                ic.materialIndex   = dp.materialIndex;
                ic.prevPosInfo     = dp.prevPosElementBase;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                // Phase 3: degenerate instanceCount=0 for packets the main pass
                // wants to skip (renderInMainPass=0 OR viewMask excludes MainCamera).
                // Keeps the indirect arg slot 1:1 with m_drawPackets so group
                // offsets stay contiguous, while ExecuteIndirect treats the slot
                // as a no-op draw. CPU per-draw fallback short-circuits earlier.
                const bool wantsMain =
                    dp.renderInMainPass != 0
                    && (dp.viewMask & ViewBit::MainCamera) != 0;
                ic.instanceCount          = wantsMain ? dp.instanceCount : 0;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
            }
        }

        i = j;
    }

    m_indirectDrawCount = static_cast<uint32_t>(m_drawPackets.size());

    // Build per-PSO groups for ExecuteIndirect batching (only opaque filter).
    m_indirectGroups.clear();
    if (m_indirectArgMapped[frameSlot] && m_indirectDrawCount > 0)
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
}

// ---------------------------------------------------------------------------
void Renderer::BuildRenderScene(World& world)
{
    m_drawPackets.clear();

    // Current-frame camera forward for transparent sort (m_view is updated AFTER this function).
    const XMFLOAT3 camForwardThisFrame = m_camera.forward;

    // ---- Sync material textures + NPR ramp scan + deferred-release flush. ----
    BuildScene_SyncMaterials(world);

    // ---- Phase 0.5: BVH + frustum cull — dense per-entity visible/tested bitmasks. ----
    std::vector<uint8_t> bvhVisibleMask, bvhTestedMask;
    BuildScene_CullBVH(world, bvhVisibleMask, bvhTestedMask);

    // ---- Debug wireframes (NOT gated on culling — needs Clear() each frame regardless). ----
    BuildScene_DebugWireframes(world);

    // ---- Phase 1: Collect draw candidates ----------------------------------
    // DrawCandidate + ApplyBlendMode + ResolveCustomPSID are now class members
    // (see Renderer.h / the two methods just above) so the per-source gather
    // helpers can share them.
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
    BuildScene_GatherPrimitivesAndLights(world, candidates, camForwardThisFrame);

    // ---- MeshLibRef entities (P1-P6 path): serial gather + parallel OBB cull + candidate build. ----
    BuildScene_GatherMeshLibRefs(world, candidates, bvhTestedMask, bvhVisibleMask, camForwardThisFrame);

    // ---- Billboard entities → additional DrawCandidates. ----
    BuildScene_GatherBillboards(world, candidates, camForwardThisFrame);

    // ---- Beam entities → CS-generated tube DrawCandidates. ----
    BuildScene_GatherBeams(world, candidates, camForwardThisFrame);

    // ---- Afterimage snapshots → DrawCandidates (procedural ghost PS). ----
    BuildScene_GatherAfterimages(candidates, camForwardThisFrame);

    s_lastCandidateCount = static_cast<uint32_t>(candidates.size());
    s_lastFrameLightCount = static_cast<uint32_t>(m_frameLights.size());
    s_lastVolLightCount   = static_cast<uint32_t>(m_volumetricLights.size());

    // ---- Phase 2: sort candidates, then write instance/material buffers + emit DrawPackets. ----
    BuildScene_SortAndEmit(candidates);

    // Phase 4: upload lights + sync IBL (separated for future extensibility).
    BuildScene_UploadLights(world);

    // Phase 5: pack reflection probes into GPU StructuredBuffer (placement/bounds/count only).
    BuildScene_UploadProbes(world);

    // Phase 5b: DDGI volume scan + tick — must run before LightingPass binds DDGI SRVs.
    BuildScene_UpdateDDGI(world);

    // Phase 6: terrain heightmap sync + CB upload + arm TerrainPass.
    BuildScene_SyncTerrain(world);
}

// ---------------------------------------------------------------------------
void Renderer::BuildScene_UploadLights(World& world)
{
    auto* lb = m_lightCB.Current(m_gfx);

    // ---- Spot-shadow slice assignment: first kMaxCasters shadow-casting spots.
    // Non-casters keep shadowSliceIdx = ~0u; shaders fall back to occlusion-only path.
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    uint32_t activeCasters = 0;
    if (m_spotShadowPass && m_spotShadowVPMapped[frameSlot])
    {
        auto* vpDst = static_cast<XMFLOAT4X4*>(m_spotShadowVPMapped[frameSlot]);
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

