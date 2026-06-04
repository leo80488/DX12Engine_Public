#pragma once

// ECS components inspired by Wicked Engine: MaterialComponent, MeshComponent
// Integrated with this project's ECS: no wi:: dependency, uses DirectXMath + std

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"
#include "Graphics/ShadingModel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <DirectXMath.h>

// Avoid Windows headers defining min/max macros which break std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// Helper types
// ---------------------------------------------------------------------------

struct AABB
{
    DirectX::XMFLOAT3 _min = { 0, 0, 0 };
    DirectX::XMFLOAT3 _max = { 0, 0, 0 };
};

enum class BlendMode : uint32_t
{
    Opaque,
    Alpha,          // SRC_ALPHA / INV_SRC_ALPHA — forward transparent pass
    Additive,       // ONE / ONE — forward transparent pass
    Premultiplied,  // ONE / INV_SRC_ALPHA — forward transparent pass
    Multiply,       // DST_COLOR / INV_SRC_ALPHA — forward transparent pass
    Count
};

enum class StencilRef : uint32_t
{
    Default = 0,
    Custom
};

enum class ShadingRate : uint8_t
{
    Rate_1x1,
    Rate_1x2,
    Rate_2x1,
    Rate_2x2
};

// Per-material shadow-caster cull mode. Default preserves the existing
// back-cull + bias behaviour. Front-cull is the classic "closed-mesh
// character" shortcut — since only the inner faces are written to the shadow
// map, self-shadow acne is physically impossible regardless of bias. None
// is for single-sided geometry (hair cards, eyelash planes, cloth, leaves)
// that would lose its shadow under either cull direction.
//
// Alpha-test draws ALWAYS render with CullMode::NONE regardless of this
// value (see ShadowPass), so setting Front/Default on an alpha-test material
// is a no-op.
enum class ShadowCullMode : uint8_t
{
    Default = 0,  // back-cull (current behaviour for every legacy material)
    Front   = 1,  // front-cull — closed-mesh characters / organic solids
    None    = 2,  // no cull — single-sided geometry
};

// ---------------------------------------------------------------------------
// X-Macro: all inspector-editable PBR properties of MaterialComponent.
//
// Signature: X(type, name, label, min, max)
//   • type  — C++ field type (float or DirectX::XMFLOAT4)
//   • name  — member name used in code
//   • label — human-readable ImGui label
//   • min/max — slider range (ignored by ColorEdit4 for XMFLOAT4)
//
// Adding one row here automatically:
//   1. declares the struct field (via MATERIAL_PROPS in MaterialComponent)
//   2. generates the corresponding ImGui widget in RenderMaterialInspector
// ---------------------------------------------------------------------------
// X(enum_name, label, default_path)
// Drives: TEXTURESLOT enum, constructor default paths, inspector slot labels.
#define MATERIAL_TEXTURE_SLOTS(X) \
    X(BASECOLORMAP, "Base Color",    "asset/Default_Texture/Default_BaseColor.itex") \
    X(NORMALMAP,    "Normal Map",    "asset/Default_Texture/Default_Normal.itex")    \
    X(SURFACEMAP,   "SURFACEMAP Map",  "asset/Default_Texture/Default_ARM.itex") \
    X(METALLICMAP,  "Metallic map",  "asset/Default_Texture/Default_Metallic.itex") \
    X(ROUGHNESSMAP, "Roughness map", "asset/Default_Texture/Default_Roughness.itex") \
    X(OCCLUSIONMAP, "Occlusion Map", "asset/Default_Texture/Default_AO.itex") \
    X(EMISSIVEMAP,  "Emissive Map",  "asset/Default_Texture/Default_Emissive.itex")  \
    X(RAMPMAP,      "Ramp Texture",  "asset/Default_Texture/T_R2T1LinnaeaMd10011_Skin.itex")  \

// X(enum_name, label)
// Drives: SHADERTYPE enum, inspector shader combo labels.
#define MATERIAL_SHADER_TYPES(X) \
    X(SHADERTYPE_PBR,                         "PBR")            \
    X(SHADERTYPE_PBR_PARALLAXOCCLUSIONMAPPING, "PBR Parallax")   \
    X(SHADERTYPE_UNLIT,                        "Unlit")          \
    X(SHADERTYPE_PBR_CLEARCOAT,                "PBR ClearCoat")  \
    X(SHADERTYPE_NPR_RAMP,                    "NPR Ramp")       \
    X(SHADERTYPE_NPR_COLOR,                   "NPR Color")

// X(type, name, label, min, max)
// Drives: struct field declarations, inspector PBR slider/color widgets.
#define MATERIAL_PROPS(X) \
    X(float,             roughnessMax,         "Roughness Max",   0.f, 1.f) \
    X(float,             roughnessMin,         "Roughness Min",   0.f, 1.f) \
    X(float,             metalnessMax,         "Metalness Max",   0.f, 1.f) \
    X(float,             metalnessMin,         "Metalness Min",   0.f, 1.f) \
    X(float,             reflectance,       "Reflectance",     0.f, 1.f) \
    X(float,             normalMapStrength, "Normal Strength", 0.f, 4.f) \
    X(float,             saturation,        "Saturation",      0.f, 2.f) \
    X(float,             alphaRef,          "Alpha Ref",       0.f, 1.f) \
    X(DirectX::XMFLOAT4, baseColor,         "Base Color",      0.f, 1.f) \
    X(DirectX::XMFLOAT4, specularColor,     "Specular",        0.f, 1.f) \
    X(DirectX::XMFLOAT4, emissiveColor,     "Emissive",        0.f, 1.f)

// ---------------------------------------------------------------------------
// MaterialComponent (alignas(32): PBR material and texture slots)
// ---------------------------------------------------------------------------

struct alignas(32) MaterialComponent
{
    enum FLAGS : uint32_t
    {
        EMPTY = 0,
        DIRTY = 1 << 0,
        CAST_SHADOW = 1 << 1,
        USE_VERTEXCOLORS = 1 << 5,
        SPECULAR_GLOSSINESS_WORKFLOW = 1 << 6,
        OCCLUSION_PRIMARY = 1 << 7,
        OCCLUSION_SECONDARY = 1 << 8,
        USE_WIND = 1 << 9,
        DISABLE_RECEIVE_SHADOW = 1 << 10,
        DOUBLE_SIDED = 1 << 11,
        OUTLINE = 1 << 12,
        OUTLINE_SCREENSPACE = 1 << 13,  // enable screen-space edge detection outline
        EXCLUDE_FROM_SSAO = 1 << 14,    // skip XeGTAO on pixels using this material
                                        // (mask characters/skin so they neither cast
                                        //  spurious AO nor accumulate temporal noise)
        ASSET_DIRTY = 1 << 17,  // runtime instance differs from on-disk .imat (unsaved)
        INTERNAL = 1 << 18,
        GPU_LINEAR_CACHE_DIRTY = 1u << 19, // renderer-owned: sRGB colors changed, linear cache needs refresh
    };
    uint32_t _flags = CAST_SHADOW | GPU_LINEAR_CACHE_DIRTY;

    // Cached linear-space colors, populated by Renderer::WriteMatSlot the
    // first time it sees a dirty material and reused every subsequent frame
    // until a setter invalidates them. Skips 14× std::powf per batch.
    mutable DirectX::XMFLOAT4 _linearBaseColor     = { 1, 1, 1, 1 };
    mutable DirectX::XMFLOAT4 _linearSpecularColor = { 1, 1, 1, 1 };
    mutable DirectX::XMFLOAT4 _linearEmissiveColor = { 1, 1, 1, 0 };
    mutable DirectX::XMFLOAT3 _linearNprDiffuseRamp = { 1, 0.92f, 0.82f };
    mutable DirectX::XMFLOAT3 _linearNprShadowRamp  = { 0.38f, 0.32f, 0.5f };

    enum SHADERTYPE : uint32_t
    {
#define SHADER_ENUM_(e, label) e,
        MATERIAL_SHADER_TYPES(SHADER_ENUM_)
#undef SHADER_ENUM_
        SHADERTYPE_COUNT
    };
    SHADERTYPE shaderType = SHADERTYPE_PBR;

    StencilRef engineStencilRef = StencilRef::Default;
    BlendMode userBlendMode = BlendMode::Opaque;
    // Shadow-pass rasterizer cull override. See ShadowCullMode comment for
    // usage. Default keeps every existing material on the back-cull path.
    ShadowCullMode shadowCullMode = ShadowCullMode::Default;

    // PBR props — declared via MATERIAL_PROPS; defaults set in constructor.
#define MAT_DECL_(type, name, label, mn, mx) type name{};
    MATERIAL_PROPS(MAT_DECL_)
#undef MAT_DECL_



    DirectX::XMFLOAT4 texMulAdd = { 1, 1, 0, 0 };
    float parallaxOcclusionMapping = 0.0f;

    // ---- NPR Ramp parameters (only used when shaderType == SHADERTYPE_NPR_RAMP) ----
    // Default 0 = true-black shadows (matches PBR histogram, AutoExposure behaves
    // the same for NPR and PBR; bump up if you want a soft-shadow floor).
    float nprMinBrightness   = 0.0f;   // minimum brightness in shadow (0=full dark, 1=no shadow)
    float nprShadowThreshold = 0.5f;   // NdotL threshold for shadow/lit transition
    float nprShadowSmooth    = 0.05f;  // smoothstep width around threshold
    float nprRimPower        = 4.0f;   // rim light falloff exponent
    float nprRimStrength     = 0.3f;   // rim light intensity
    float nprRampBlend       = 1.0f;   // 0 = pure baseColor tint, 1 = full ramp color
    float nprBrightnessClamp = 1.0f;   // max output brightness relative to baseColor lum (0 = disabled)
    float nprMaxBrightness   = 0.0f;   // absolute HDR luminance ceiling for final NPR color (0 = disabled)

    // ---- NPR Ramp texture-layer blend weights (only used when shaderType == SHADERTYPE_NPR_RAMP) ----
    float nprMidWeight       = 0.4f;   // mid-tone ramp row blend weight
    float nprVeinWeight      = 0.5f;   // deep-shadow vein ramp row blend weight (modulated by shadowMask * ao)
    float nprSSSWeight       = 0.3f;   // SSS ramp row blend weight

    // ---- NPR Color ramp (only used when shaderType == SHADERTYPE_NPR_COLOR) ----
    // Two editable ramp colors drive a texture-less diffuse ramp.
    DirectX::XMFLOAT4 nprDiffuseRampColor = { 1.00f, 0.92f, 0.82f, 1.0f }; // lit side tint
    DirectX::XMFLOAT4 nprShadowRampColor  = { 0.38f, 0.32f, 0.50f, 1.0f }; // shadow side tint

    uint8_t userStencilRef = 0;

    DirectX::XMFLOAT2 texAnimDirection = { 0, 0 };
    float texAnimFrameRate = 0.0f;
    float texAnimElapsedTime = 0.0f;

    // Per-material outline thickness (pixels, screen-space invariant).
    // Only used when OUTLINE flag is set.
    float outlinePixels = 2.0f;

    enum TEXTURESLOT : uint32_t
    {
#define SLOT_ENUM_(e, label, path) e,
        MATERIAL_TEXTURE_SLOTS(SLOT_ENUM_)
#undef SLOT_ENUM_
        TEXTURESLOT_COUNT
    };

    struct TextureMap
    {
        std::string name;
        uint32_t uvset = 0;
        int descriptorIndex = -1;  // Descriptor index returned by the CBV_SRV_UAV allocator, or -1
        uint64_t gpuHandle = 0;    // Cached GPU descriptor handle for the texture SRV (0 = unloaded)
        uint64_t previewGpuHandle = 0; // UNORM alias handle for editor preview (no sRGB decode)
        int32_t  bindlessIndex = -1;   // Texture pool handle_id (index into bindless g_AllTextures[])
    };
    TextureMap textures[TEXTURESLOT_COUNT];

    // ---- Custom-shader parameter stores (Phase C) ---------------------------
    // Populated from ShaderReflect::Reflection when the material's custom PS
    // resolves (see SyncMaterialWithReflection). Keyed by the binding/variable
    // NAME as it appears in the shader, so values survive register-slot
    // reshuffling between shader edits.
    //
    // customTextures: each entry reuses TextureMap; only `name` + `uvset` are
    //                 persisted, the bindless handle / descriptor index are
    //                 runtime state populated by the resource manager.
    // customParams:   float-bag, always 4 wide. Scalars use [0], vec2 uses
    //                 [0..1], vec3 uses [0..2], vec4 uses all four. Ints
    //                 bit-cast into the same array when VarType is integer —
    //                 a richer tagged union can come later.
    std::unordered_map<std::string, TextureMap>          customTextures;
    std::unordered_map<std::string, std::array<float,4>> customParams;

    MaterialComponent()
    {
        // Defaults for MATERIAL_PROPS fields (zero-initialised above via {}).
        roughnessMax         = 1.0f;
        roughnessMin         = 0.0f;

        metalnessMax         = 1.0f;
        metalnessMin         = 0.0f;

        reflectance       = 0.5f;
        normalMapStrength = 1.0f;
        saturation        = 1.0f;
        alphaRef          = 1.0f;
        baseColor         = { 1, 1, 1, 1 };
        specularColor     = { 1, 1, 1, 1 };
        emissiveColor     = { 1, 1, 1, 0 };

#define SLOT_DEFAULT_(e, label, path) textures[e].name = path;
        MATERIAL_TEXTURE_SLOTS(SLOT_DEFAULT_)
#undef SLOT_DEFAULT_
    }

    // ---- Custom GBuffer pixel shader (Phase B) ------------------------------
    // Enable + customShaderPath together direct the GBuffer pass to compile
    // the user's HLSL and route this material's draws through the resulting
    // PSO. customShadingModel tells the lighting pass how to interpret the
    // GBuffer outputs the custom PS produces.
    //
    // customShaderPath is the persistent identity (relative to the shader
    // directory). customShaderID is a runtime-only handle assigned by
    // ShaderLibrary the first time the path is registered — NOT serialized.
    bool         useCustomShader      = false;
    std::string  customShaderPath;
    ShadingModel customShadingModel   = ShadingModel::Standard;

    // Runtime cookie — cached dynamic ShaderID after Renderer resolves
    // customShaderPath via GBufferPass::GetShaderLibrary().RegisterDynamic.
    // `mutable` so the const-correct BuildRenderScene candidate loop can
    // write the cache without widening the public MaterialComponent API.
    // -1 = unresolved (or failed); positive values index ShaderLibrary's
    // dynamic registry at (customShaderID - ShaderID::Count).
    mutable int customShaderID = -1;
    uint32_t layerMask = ~0u;
    int samplerDescriptor = -1;

    Entity cameraSource = NullEntity;

    constexpr void SetUserStencilRef(uint8_t value) { userStencilRef = value & 0x0Fu; }
    uint32_t GetStencilRef() const;

    constexpr float GetOpacity() const { return baseColor.w; }
    constexpr float GetEmissiveStrength() const { return emissiveColor.w; }
    constexpr int GetCustomShaderID() const { return customShaderID; }

    constexpr void SetDirty(bool value = true)
    {
        if (value) _flags |= DIRTY | GPU_LINEAR_CACHE_DIRTY;
        else       _flags &= ~DIRTY;   // keep GPU_LINEAR_CACHE_DIRTY until renderer consumes it
    }
    // Mark both GPU dirty and asset dirty (call from editor when user modifies a property).
    // Must also set GPU_LINEAR_CACHE_DIRTY so the renderer re-runs srgb→linear
    // conversion on baseColor / specularColor / emissiveColor — otherwise the
    // inspector slider changes but the cached `_linearBaseColor` stays stale
    // and the GPU uploads the old value.
    constexpr void SetEditDirty()
    { _flags |= DIRTY | ASSET_DIRTY | GPU_LINEAR_CACHE_DIRTY; }
    constexpr bool IsDirty() const { return (_flags & DIRTY) != 0; }

    // Asset dirty: runtime instance differs from on-disk .imat file.
    // Set when editor modifies material; cleared on Save-to-disk.
    constexpr void SetAssetDirty(bool value = true) { if (value) _flags |= ASSET_DIRTY; else _flags &= ~ASSET_DIRTY; }
    constexpr bool IsAssetDirty() const { return (_flags & ASSET_DIRTY) != 0; }

    constexpr void SetInternal(bool value = true) { if (value) _flags |= INTERNAL; else _flags &= ~INTERNAL; }
    constexpr bool IsInternal() const { return (_flags & INTERNAL) != 0; }

    constexpr void SetCastShadow(bool value) { SetDirty(); if (value) _flags |= CAST_SHADOW; else _flags &= ~CAST_SHADOW; }
    constexpr void SetReceiveShadow(bool value) { SetDirty(); if (value) _flags &= ~DISABLE_RECEIVE_SHADOW; else _flags |= DISABLE_RECEIVE_SHADOW; }
    constexpr void SetOcclusionEnabled_Primary(bool value) { SetDirty(); if (value) _flags |= OCCLUSION_PRIMARY; else _flags &= ~OCCLUSION_PRIMARY; }
    constexpr void SetOcclusionEnabled_Secondary(bool value) { SetDirty(); if (value) _flags |= OCCLUSION_SECONDARY; else _flags &= ~OCCLUSION_SECONDARY; }

    constexpr bool IsCastingShadow() const { return (_flags & CAST_SHADOW) != 0; }
    constexpr bool IsAlphaTestEnabled() const { return alphaRef <= 1.0f - 1.0f / 256.0f; }
    constexpr bool IsUsingVertexColors() const { return (_flags & USE_VERTEXCOLORS) != 0; }
    constexpr bool IsUsingWind() const { return (_flags & USE_WIND) != 0; }
    constexpr bool IsReceiveShadow() const { return (_flags & DISABLE_RECEIVE_SHADOW) == 0; }
    constexpr bool IsUsingSpecularGlossinessWorkflow() const { return (_flags & SPECULAR_GLOSSINESS_WORKFLOW) != 0; }
    constexpr bool IsOcclusionEnabled_Primary() const { return (_flags & OCCLUSION_PRIMARY) != 0; }
    constexpr bool IsOcclusionEnabled_Secondary() const { return (_flags & OCCLUSION_SECONDARY) != 0; }
    constexpr bool IsCustomShader() const { return customShaderID >= 0; }
    constexpr bool IsDoubleSided() const { return (_flags & DOUBLE_SIDED) != 0; }
    constexpr bool IsOutlineEnabled() const { return (_flags & OUTLINE) != 0; }
    constexpr bool IsOutlineScreenSpaceEnabled() const { return (_flags & OUTLINE_SCREENSPACE) != 0; }

    BlendMode GetBlendMode() const;

    constexpr void SetBaseColor(const DirectX::XMFLOAT4& value) { SetDirty(); baseColor = value; }
    constexpr void SetSpecularColor(const DirectX::XMFLOAT4& value) { SetDirty(); specularColor = value; }
    constexpr void SetEmissiveColor(const DirectX::XMFLOAT4& value) { SetDirty(); emissiveColor = value; }
    constexpr void SetRoughnessMax(float value) { SetDirty(); roughnessMax = value; }
    constexpr void SetRoughnessMin(float value) { SetDirty(); roughnessMin = value; }

    constexpr void SetReflectance(float value) { SetDirty(); reflectance = value; }
    constexpr void SetMetalnessMax(float value) { SetDirty(); metalnessMax = value; }
    constexpr void SetMetalnessMin(float value) { SetDirty(); metalnessMin = value; }

    constexpr void SetEmissiveStrength(float value) { SetDirty(); emissiveColor.w = value; }
    constexpr void SetSaturation(float value) { SetDirty(); saturation = value; }
    constexpr void SetNormalMapStrength(float value) { SetDirty(); normalMapStrength = value; }
    constexpr void SetParallaxOcclusionMapping(float value) { SetDirty(); parallaxOcclusionMapping = value; }
    constexpr void SetOpacity(float value) { SetDirty(); baseColor.w = value; }
    constexpr void SetAlphaRef(float value) { SetDirty(); alphaRef = value; }
    constexpr void SetUseVertexColors(bool value) { SetDirty(); if (value) _flags |= USE_VERTEXCOLORS; else _flags &= ~USE_VERTEXCOLORS; }
    constexpr void SetUseWind(bool value) { SetDirty(); if (value) _flags |= USE_WIND; else _flags &= ~USE_WIND; }
    constexpr void SetUseSpecularGlossinessWorkflow(bool value) { SetDirty(); if (value) _flags |= SPECULAR_GLOSSINESS_WORKFLOW; else _flags &= ~SPECULAR_GLOSSINESS_WORKFLOW; }
    constexpr void SetDoubleSided(bool value = true) { if (value) _flags |= DOUBLE_SIDED; else _flags &= ~DOUBLE_SIDED; }
    constexpr void SetOutlineEnabled(bool value = true) { if (value) _flags |= OUTLINE; else _flags &= ~OUTLINE; }
    constexpr void SetOutlineScreenSpaceEnabled(bool value = true) { if (value) _flags |= OUTLINE_SCREENSPACE; else _flags &= ~OUTLINE_SCREENSPACE; }
    constexpr void SetCustomShaderID(int id) { customShaderID = id; }
    constexpr void DisableCustomShader() { customShaderID = -1; }
};

// ---------------------------------------------------------------------------
// MeshComponent (alignas(32): mesh vertices/indices and subsets)
// ---------------------------------------------------------------------------

struct alignas(32) MeshComponent
{
    enum FLAGS : uint32_t
    {
        EMPTY = 0,
        RENDERABLE = 1 << 0,
        DOUBLE_SIDED = 1 << 1,
        DYNAMIC = 1 << 2,
        DOUBLE_SIDED_SHADOW = 1 << 7,
    };
    uint32_t _flags = RENDERABLE;

    std::vector<DirectX::XMFLOAT3> vertex_positions;
    std::vector<DirectX::XMFLOAT3> vertex_normals;
    std::vector<DirectX::XMFLOAT4> vertex_tangents;
    std::vector<DirectX::XMFLOAT2> vertex_uvset_0;
    std::vector<DirectX::XMFLOAT2> vertex_uvset_1;
    std::vector<DirectX::XMUINT4> vertex_boneindices;
    std::vector<DirectX::XMFLOAT4> vertex_boneweights;
    std::vector<uint32_t> vertex_colors;
    std::vector<uint32_t> indices;

    enum MESH_SUBSET_FLAGS : uint32_t
    {
        MESH_SUBSET_DOUBLESIDED = 1 << 0,
    };

    struct MeshSubset
    {
        std::string surfaceName;
        Entity materialID = NullEntity;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint32_t materialIndex = 0;
        uint32_t flags = 0;
        constexpr bool IsDoubleSided() const { return (flags & MESH_SUBSET_DOUBLESIDED) != 0; }
    };
    std::vector<MeshSubset> subsets;

    Entity armatureID = NullEntity;
    float tessellationFactor = 0.0f;
    uint32_t subsets_per_lod = 0;

    AABB aabb;

    struct BufferView
    {
        uint64_t offset = ~0ull;
        uint64_t size = 0ull;
        int descriptor_srv = -1;
        constexpr bool IsValid() const { return offset != ~0ull; }
    };
    BufferView ib;
    BufferView vb_pos;
    BufferView vb_nor;
    BufferView vb_tan;
    BufferView vb_uvs;
    BufferView vb_col;
    BufferView vb_bon;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferResource;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};

    uint32_t geometryOffset = 0;

    constexpr void SetRenderable(bool value) { if (value) _flags |= RENDERABLE; else _flags &= ~RENDERABLE; }
    constexpr void SetDoubleSided(bool value) { if (value) _flags |= DOUBLE_SIDED; else _flags &= ~DOUBLE_SIDED; }
    constexpr void SetDoubleSidedShadow(bool value) { if (value) _flags |= DOUBLE_SIDED_SHADOW; else _flags &= ~DOUBLE_SIDED_SHADOW; }
    constexpr void SetDynamic(bool value) { if (value) _flags |= DYNAMIC; else _flags &= ~DYNAMIC; }

    constexpr bool IsRenderable() const { return (_flags & RENDERABLE) != 0; }
    constexpr bool IsDoubleSided() const { return (_flags & DOUBLE_SIDED) != 0; }
    constexpr bool IsDoubleSidedShadow() const { return (_flags & DOUBLE_SIDED_SHADOW) != 0; }
    constexpr bool IsDynamic() const { return (_flags & DYNAMIC) != 0; }
    constexpr bool IsSkinned() const { return armatureID != NullEntity; }

    uint32_t GetLODCount() const;
    void GetLODSubsetRange(uint32_t lod, uint32_t& first_subset, uint32_t& last_subset) const;

    void CreateRenderData(class GraphicsDX12* backend);
    void DeleteRenderData();

    const D3D12_VERTEX_BUFFER_VIEW* GetVertexBufferView() const { return m_vertexBufferResource ? &m_vertexBufferView : nullptr; }
    const D3D12_INDEX_BUFFER_VIEW* GetIndexBufferView() const { return m_indexBufferResource ? &m_indexBufferView : nullptr; }

    void ComputeNormals(bool smooth);
    void FlipCulling();
    void Recenter();
};

// ===========================================================================
// Render-pipeline ECS components (6-layer architecture)
// These are read by culling systems and the Renderer::BeginFrame translation
// layer. They must NOT appear below the Renderer boundary (in RenderPasses).
// ===========================================================================

// NOTE: the legacy `Transform` struct (position / rotation / scale +
// manually-maintained localToWorld) was removed. All transform data now
// flows through LocalTransform + GlobalTransform in HierarchyComponents.h —
// TransformSystem::Propagate owns the parent-inherited world matrix so
// there is no more "is this matrix hierarchy-aware?" ambiguity.

// BoundingVolume — axis-aligned bounding box in local space used for culling
struct BoundingVolume
{
    DirectX::XMFLOAT3 center  = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 extents = { 0.5f, 0.5f, 0.5f }; // half-extents
};

// MeshHandle — index into the GPU mesh registry owned by the Renderer
struct MeshHandle
{
    uint32_t gpuMeshID = UINT32_MAX; // UINT32_MAX = invalid/not uploaded
    bool IsValid() const { return gpuMeshID != UINT32_MAX; }
};

// MaterialHandle — index into the GPU material table owned by the Renderer
struct MaterialHandle
{
    uint32_t materialID = UINT32_MAX; // UINT32_MAX = invalid
    bool IsValid() const { return materialID != UINT32_MAX; }
};

// VisibleTag — zero-size marker; presence means the entity passed all culling
// tests this frame. Written by FrustumCullSystem / OcclusionCullSystem.
// The Renderer reads this tag to decide which entities to add to DrawPackets.
struct VisibleTag {};

// LightType — discriminator for LightData
enum class LightType : uint8_t
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// LightData — light source parameters. Position comes from GlobalTransform,
// NOT stored here. Direction is local-space for directional/spot; for spot it
// is also transformed by the entity's rotation at upload time.
struct LightData
{
    float             radius    = 10.f;       // attenuation range (point/spot)
    DirectX::XMFLOAT3 color     = { 1.f, 1.f, 1.f };
    float             intensity = 1.f;
    DirectX::XMFLOAT3 direction = { 0.f, -1.f, 0.f }; // local-space direction (directional/spot)
    float             spotAngle = 0.5236f;    // spot half-angle in radians (default 30°)
    LightType         type      = LightType::Directional;
    // Opt-in shadow caster flag for spot lights. When true, SpotShadowPass
    // renders a depth map from the light's POV into the shared atlas; the
    // Lighting and VolumetricFog shaders then sample it to mask both surface
    // contribution and volumetric scatter behind walls. Directional lights
    // always use CSM regardless. Point lights currently ignore the flag
    // (omni shadows would need a cubemap atlas — future work).
    bool              castsShadow = false;
};

// ---------------------------------------------------------------------------
// DecalComponent — per-entity instance of a clustered decal.
//
// Material-driven design (see decal_system_prompt.md §12 "Material System
// Integration"): the component holds a shared pointer to a DecalMaterialAsset
// that defines the textures, tint, flags, and blend behaviour. Many entities
// (1000 blood splats, 500 bullet holes) can reference a single asset — per-
// instance state (transform, lifetime, fade) stays on this component.
//
// The decal volume is a unit cube centred at the entity's GlobalTransform;
// scale the transform to size it. The Apply CS projects each visible pixel
// into decal-local [-0.5, 0.5]^3 and discards out-of-bounds, so axis-aligned
// scaling works as a box clip.
//
// Full include rather than forward decl: std::shared_ptr<T>'s implicit
// destructor needs T complete at the point the compiler emits the
// DecalComponent destructor (vector growth, pool clear). DecalMaterialAsset.h
// is a light include (SystemHandles + MaterialDomain + DirectXMath) so the
// blast radius is small.
// ---------------------------------------------------------------------------
#include "Resource/DecalMaterialAsset.h"

struct DecalComponent
{
    // Shared material template — owned by DecalMaterialLibrary. nullptr =>
    // entity contributes no decal this frame (useful to temporarily disable
    // an instance without destroying it).
    std::shared_ptr<Resource::DecalMaterialAsset> material;

    // ---- Per-instance state ------------------------------------------------
    // Runtime multiplier on top of the asset's opacity. DecalLifetimeSystem
    // drives this for dynamic decals (fade to 0 before recycle). Static
    // decals leave it at 1.
    float fadeAlpha = 1.0f;

    // Lifetime for dynamic decals. -1 = static (never expires). Positive
    // values count down in seconds; DecalLifetimeSystem decrements this every
    // frame and expires the decal at 0.
    float lifetime = -1.0f;

    // Seconds before expiry over which fadeAlpha linearly decays from 1→0.
    // Set to 0 to skip fade-out (instant pop). Only meaningful when
    // lifetime >= 0. Default ~half-second matches FPS blood-splatter feel.
    float fadeOutDuration = 0.5f;

    // When true (default), the whole entity is destroyed on expiry. When
    // false, only the DecalComponent is removed — the entity stays alive
    // (useful for object-pooled decals managed by external code that
    // recycles the slot with a fresh material + lifetime).
    bool destroyEntityOnExpire = true;

    // Optional per-instance tint tweak. w=0 means "no override, use asset
    // tint as-is". w>0 fully replaces the asset tint. Lets a single blood
    // material render with slightly different shades per splat without
    // authoring multiple assets.
    DirectX::XMFLOAT4 tintOverride = { 0.f, 0.f, 0.f, 0.f };
};

// VolumetricLightComponent — opt-in marker placed on a Light entity to make it
// participate in the volumetric fog froxel grid. Without this component the
// light only contributes to direct surface lighting (clustered shading) and
// is invisible inside the fog volume. With it, the volumetric pass injects
// the light's energy into every froxel it touches and you get a visible
// shaft / cone through the fog.
//
// `intensityScale` lets you boost the light's brightness specifically for the
// fog without affecting the direct surface lighting term — useful when a
// flashlight needs a dramatic visible shaft but the lit surface should stay
// at a sensible brightness.
struct VolumetricLightComponent
{
    bool  enabled        = true;
    float intensityScale = 1.f;
};

// CameraData — camera parameters read by Renderer::BeginFrame to build RenderView
struct CameraData
{
    float fov               = DirectX::XM_PIDIV4; // vertical FOV in radians
    float nearZ             = 0.1f;
    float farZ              = 1000.f;
    float aspectRatioOverride = 0.f; // 0 = use window aspect ratio
};

// CameraComponent — lens / view-projection parameters for the camera entity.
//
// The camera's POSE (position + orientation) lives on LocalTransform /
// GlobalTransform like any other entity; its FPS-controller state lives on
// CameraControllerComponent. This struct holds only what the projection
// matrix needs.
struct CameraComponent
{
    float fov   = DirectX::XM_PI / 3.f;  // vertical FOV in radians
    float nearZ = 0.1f;
    float farZ  = 200.f;
};

// ActiveCameraTag — single-instance marker for "the camera the renderer uses
// this frame". App::RefreshMainCamera prefers the tagged entity; absent a
// tag, it falls back to the first entity with a CameraComponent.
//
// Switching:
//   * Lua:  Camera.SetActive(entityId) / Camera.GetActive()
//           (src/ECS/LuaPlayerBindings.cpp registers these on the Camera table)
//   * C++:  call SetActiveCamera helpers via Lua or strip/add the tag directly.
//
// The tag holds no data — adding it to an entity says "this camera wins".
// SetActive strips the tag from any previous holder so at most one entity
// carries it at a time.
struct ActiveCameraTag : ComponentBase {};

// CameraControllerComponent — FPS-style controller state for the camera entity.
//
// CameraSystem reads/writes yaw+pitch here each frame and drives the entity's
// LocalTransform from them (translation via WASD, rotation via mouse delta).
// yaw/pitch are kept as the controller's authoritative Euler state so pitch
// clamping is singularity-free; the equivalent quaternion is mirrored onto
// LocalTransform.rotation.
struct CameraControllerComponent
{
    // Camera mode. Free is the default — original fly-cam behaviour (mouse
    // rotates, WASD strafes). The follow modes are for gameplay cameras
    // attached to a player or NPC: the camera derives its world position
    // from `followTarget` each frame and only consumes mouse input for
    // yaw/pitch. WASD is intentionally ignored in follow modes — the player
    // controller owns horizontal motion.
    enum class Mode : std::uint8_t
    {
        Free        = 0,   // mouse-driven yaw/pitch + WASD position
        ThirdPerson = 1,   // orbit followTarget at thirdPersonDistance
        FirstPerson = 2,   // glue camera to followTarget + headOffset
    };

    float yaw              = -2.47f;  // radians, rotation around world-Y
    float pitch            =  0.44f;  // radians, rotation around cam-X (clamped ±~89°)
    float mouseSensitivity = 0.003f;
    float moveSpeed        = 10.0f;

    // ---- Follow-mode parameters (ignored when mode == Free) -------------
    Mode          mode = Mode::Free;
    // Save-stable follow target reference (was raw Entity ID — see
    // DesignMd/entity_persistence_architecture.md for the GUID migration).
    AttachmentRef followTarget;
    // Distance from focus point to camera in ThirdPerson. The camera sits
    // *behind* the target along (-forward) by this many metres.
    float   thirdPersonDistance = 4.0f;
    // First-person eye position offset from the target's pivot. Default
    // ~1.65 m matches an average human eye height for a feet-pivoted mesh.
    DirectX::XMFLOAT3 headOffset = { 0.f, 1.65f, 0.f };
    // Third-person focus offset from the target's pivot — typically chest
    // height plus a small lateral shoulder bias so the character isn't
    // dead-centred on the screen.
    DirectX::XMFLOAT3 shoulderOffset = { 0.30f, 1.60f, 0.f };

    // Third-person spring-arm collision. When enabled and a PhysicsSystem
    // is supplied to ResolveFollowing, the camera sweeps a sphere from the
    // focus point toward its desired position; on hit the distance is
    // clamped so the camera doesn't poke through walls. Set
    // `cameraCollisionEnabled = false` to disable (useful for cinematics
    // that need a fixed camera regardless of geometry).
    bool  cameraCollisionEnabled = true;
    // Sphere-cast probe radius (metres). Larger = camera backs off earlier
    // before clipping the wall. Match this to a typical near-plane buffer
    // (~10-30 cm) so the near-clip never enters the wall.
    float cameraProbeRadius     = 0.20f;

    // ---- Third-person follow smoothing (spring-arm damping) -------------
    // The camera eases toward the target instead of rigidly snapping every
    // frame, low-pass-filtering the high-frequency shake/blur seen while
    // tracking a moving object. Frame-rate independent: alpha = 1-exp(-dt/lag).
    // Only the FOCUS point (orbit pivot) is smoothed, never the yaw/pitch, so
    // mouse-look stays instant (no rubber-band on rotation).
    // followLag   — focus position smoothing time-constant, seconds.
    //               0 = instant / rigid (DEFAULT — exact original feel); set
    //               ~0.1 for an optional cinematic trailing-camera lag.
    // distanceLag — spring-arm collision distance ease-OUT time-constant.
    //               0 = instant (DEFAULT). >0 eases the pull-OUT so an
    //               intermittent wall probe can't punch the camera in/out.
    // NOTE: these are OPTIONAL polish, OFF by default. They are NOT the fix for
    // follow jitter caused by a child mesh missing physics interpolation — that
    // is handled in PhysicsSystem::ApplyRenderInterpolation via PropagateSubtree.
    float followLag   = 0.0f;
    float distanceLag = 0.0f;

    // ---- Transient runtime smoothing state (NOT serialized) ------------
    // followSmoothInit=false → snap on the first resolve (spawn / world load /
    // mode switch). smoothedFocus/Distance carry the filtered values across
    // frames. These are runtime-only; serialization deliberately omits them so
    // a freshly-loaded controller always re-snaps.
    DirectX::XMFLOAT3 smoothedFocus    = { 0.f, 0.f, 0.f };
    float             smoothedDistance = 0.f;
    bool              followSmoothInit = false;
};
