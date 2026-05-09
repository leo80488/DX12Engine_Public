#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>
#include <limits>

// Forward declarations for CommandList recording methods
class IGraphicsDevice;
namespace RG { class RenderContext; }

namespace RHI
{
    // -----------------------------------------------------------------------
    // Enums
    // -----------------------------------------------------------------------

    enum class ShaderStage : uint8_t
    {
        MS,     // Mesh Shader
        AS,     // Amplification Shader
        VS,     // Vertex Shader
        HS,     // Hull Shader
        DS,     // Domain Shader
        GS,     // Geometry Shader
        PS,     // Pixel Shader
        CS,     // Compute Shader
        LIB,    // Shader Library
        Count,
    };

    enum class ShaderFormat : uint8_t
    {
        NONE,
        HLSL5,  // DXBC
        HLSL6,  // DXIL
    };

    enum class ShaderModel : uint8_t
    {
        SM_5_0, SM_6_0, SM_6_1, SM_6_2,
        SM_6_3, SM_6_4, SM_6_5, SM_6_6, SM_6_7,
    };

    enum class PrimitiveTopology : uint8_t
    {
        UNDEFINED,
        TRIANGLELIST,
        TRIANGLESTRIP,
        POINTLIST,
        LINELIST,
        LINESTRIP,
        PATCHLIST,
    };

    enum class ComparisonFunc : uint8_t
    {
        NEVER, LESS, EQUAL, LESS_EQUAL,
        GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS,
    };

    enum class DepthWriteMask : uint8_t { ZERO, ALL };

    enum class StencilOp : uint8_t
    {
        KEEP, ZERO, REPLACE, INCR_SAT,
        DECR_SAT, INVERT, INCR, DECR,
    };

    enum class Blend : uint8_t
    {
        ZERO, ONE,
        SRC_COLOR, INV_SRC_COLOR,
        SRC_ALPHA, INV_SRC_ALPHA,
        DEST_ALPHA, INV_DEST_ALPHA,
        DEST_COLOR, INV_DEST_COLOR,
        SRC_ALPHA_SAT,
        BLEND_FACTOR, INV_BLEND_FACTOR,
        SRC1_COLOR, INV_SRC1_COLOR,
        SRC1_ALPHA, INV_SRC1_ALPHA,
    };

    enum class BlendOp : uint8_t { ADD, SUBTRACT, REV_SUBTRACT, MIN, MAX };
    enum class FillMode : uint8_t { WIREFRAME, SOLID };
    enum class CullMode : uint8_t { NONE, FRONT, BACK };

    enum class InputClassification : uint8_t
    {
        PER_VERTEX_DATA,
        PER_INSTANCE_DATA,
    };

    enum class Usage : uint8_t
    {
        DEFAULT,    // GPU read/write, CPU no access
        UPLOAD,     // CPU write, GPU read (staging)
        READBACK,   // CPU read, GPU write
    };

    enum class TextureAddressMode : uint8_t
    {
        WRAP, MIRROR, CLAMP, BORDER, MIRROR_ONCE,
    };

    enum class Filter : uint8_t
    {
        MIN_MAG_MIP_POINT,
        MIN_MAG_POINT_MIP_LINEAR,
        MIN_POINT_MAG_LINEAR_MIP_POINT,
        MIN_POINT_MAG_MIP_LINEAR,
        MIN_LINEAR_MAG_MIP_POINT,
        MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MIN_MAG_LINEAR_MIP_POINT,
        MIN_MAG_MIP_LINEAR,
        ANISOTROPIC,
        COMPARISON_MIN_MAG_MIP_POINT,
        COMPARISON_MIN_MAG_POINT_MIP_LINEAR,
        COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT,
        COMPARISON_MIN_POINT_MAG_MIP_LINEAR,
        COMPARISON_MIN_LINEAR_MAG_MIP_POINT,
        COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        COMPARISON_MIN_MAG_MIP_LINEAR,
        COMPARISON_ANISOTROPIC,
        MINIMUM_MIN_MAG_MIP_POINT,
        MINIMUM_MIN_MAG_POINT_MIP_LINEAR,
        MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
        MINIMUM_MIN_POINT_MAG_MIP_LINEAR,
        MINIMUM_MIN_LINEAR_MAG_MIP_POINT,
        MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MINIMUM_MIN_MAG_LINEAR_MIP_POINT,
        MINIMUM_MIN_MAG_MIP_LINEAR,
        MINIMUM_ANISOTROPIC,
        MAXIMUM_MIN_MAG_MIP_POINT,
        MAXIMUM_MIN_MAG_POINT_MIP_LINEAR,
        MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
        MAXIMUM_MIN_POINT_MAG_MIP_LINEAR,
        MAXIMUM_MIN_LINEAR_MAG_MIP_POINT,
        MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MAXIMUM_MIN_MAG_LINEAR_MIP_POINT,
        MAXIMUM_MIN_MAG_MIP_LINEAR,
        MAXIMUM_ANISOTROPIC,
    };

    enum class SamplerBorderColor : uint8_t
    {
        TRANSPARENT_BLACK, OPAQUE_BLACK, OPAQUE_WHITE,
    };

    enum class Format : uint8_t
    {
        UNKNOWN,
        R32G32B32A32_FLOAT, R32G32B32A32_UINT, R32G32B32A32_SINT,
        R32G32B32_FLOAT,    R32G32B32_UINT,    R32G32B32_SINT,
        R16G16B16A16_FLOAT, R16G16B16A16_UNORM, R16G16B16A16_UINT,
        R16G16B16A16_SNORM, R16G16B16A16_SINT,
        R32G32_FLOAT, R32G32_UINT, R32G32_SINT,
        D32_FLOAT_S8X24_UINT,
        R10G10B10A2_UNORM, R10G10B10A2_UINT, R11G11B10_FLOAT,
        R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB, R8G8B8A8_UINT,
        R8G8B8A8_SNORM, R8G8B8A8_SINT,
        B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB,
        R16G16_FLOAT, R16G16_UNORM, R16G16_UINT, R16G16_SNORM, R16G16_SINT,
        D32_FLOAT, R32_FLOAT, R32_UINT, R32_SINT,
        D24_UNORM_S8_UINT, R9G9B9E5_SHAREDEXP,
        R8G8_UNORM, R8G8_UINT, R8G8_SNORM, R8G8_SINT,
        R16_FLOAT, D16_UNORM, R16_UNORM, R16_UINT, R16_SNORM, R16_SINT,
        R8_UNORM, R8_UINT, R8_SNORM, R8_SINT,
        BC1_UNORM, BC1_UNORM_SRGB,
        BC2_UNORM, BC2_UNORM_SRGB,
        BC3_UNORM, BC3_UNORM_SRGB,
        BC4_UNORM, BC4_SNORM,
        BC5_UNORM, BC5_SNORM,
        BC6H_UF16, BC6H_SF16,
        BC7_UNORM, BC7_UNORM_SRGB,
        NV12,
    };

    enum class GpuQueryType : uint8_t
    {
        TIMESTAMP, OCCLUSION, OCCLUSION_BINARY,
    };

    enum class IndexBufferFormat  : uint8_t { UINT16, UINT32 };
    enum class SubresourceType    : uint8_t { SRV, UAV, RTV, DSV };

    enum class ImageAspect : uint8_t
    {
        COLOR, DEPTH, STENCIL, LUMINANCE, CHROMINANCE,
    };

    enum class ColorWrite : uint32_t
    {
        DISABLE      = 0,
        ENABLE_RED   = 1 << 0,
        ENABLE_GREEN = 1 << 1,
        ENABLE_BLUE  = 1 << 2,
        ENABLE_ALPHA = 1 << 3,
        ENABLE_ALL   = ~0u,
    };

    // -----------------------------------------------------------------------
    // Flag enums — support bitwise operators (|, &, ^, ~, |=, &=, ^=)
    // -----------------------------------------------------------------------

    enum class BindFlag : uint32_t
    {
        NONE             = 0,
        VERTEX_BUFFER    = 1 << 0,
        INDEX_BUFFER     = 1 << 1,
        CONSTANT_BUFFER  = 1 << 2,
        SHADER_RESOURCE  = 1 << 3,
        RENDER_TARGET    = 1 << 4,
        DEPTH_STENCIL    = 1 << 5,
        UNORDERED_ACCESS = 1 << 6,
        SHADING_RATE     = 1 << 7,
    };

    enum class ResourceState : uint32_t
    {
        UNDEFINED                         = 0,
        SHADER_RESOURCE                   = 1 << 0,
        SHADER_RESOURCE_COMPUTE           = 1 << 1,
        UNORDERED_ACCESS                  = 1 << 2,
        COPY_SRC                          = 1 << 3,
        COPY_DST                          = 1 << 4,
        RENDERTARGET                      = 1 << 5,
        DEPTHSTENCIL                      = 1 << 6,
        DEPTHSTENCIL_READONLY             = 1 << 7,
        SHADING_RATE_SOURCE               = 1 << 8,
        VERTEX_BUFFER                     = 1 << 9,
        INDEX_BUFFER                      = 1 << 10,
        CONSTANT_BUFFER                   = 1 << 11,
        INDIRECT_ARGUMENT                 = 1 << 12,
        RAYTRACING_ACCELERATION_STRUCTURE = 1 << 13,
        PREDICATION                       = 1 << 14,
        VIDEO_DECODE_SRC                  = 1 << 15,
        VIDEO_DECODE_DST                  = 1 << 16,
        VIDEO_DECODE_DPB                  = 1 << 17,
        SWAPCHAIN                         = 1 << 18,
        DEPTH_READ_SRV                    = 1 << 19,  // depth read + pixel shader SRV simultaneously
    };

    enum class ResourceMiscFlag : uint32_t
    {
        NONE                          = 0,
        TEXTURECUBE                   = 1 << 0,
        INDIRECT_ARGS                 = 1 << 1,
        BUFFER_RAW                    = 1 << 2,
        BUFFER_STRUCTURED             = 1 << 3,
        RAY_TRACING                   = 1 << 4,
        PREDICATION                   = 1 << 5,
        TRANSIENT_ATTACHMENT          = 1 << 6,
        SPARSE                        = 1 << 7,
        ALIASING_BUFFER               = 1 << 8,
        ALIASING_TEXTURE_NON_RT_DS    = 1 << 9,
        ALIASING_TEXTURE_RT_DS        = 1 << 10,
        ALIASING                      = ALIASING_BUFFER | ALIASING_TEXTURE_NON_RT_DS | ALIASING_TEXTURE_RT_DS,
        TYPED_FORMAT_CASTING          = 1 << 11,
        TYPELESS_FORMAT_CASTING       = 1 << 12,
        VIDEO_DECODE                  = 1 << 13,
        VIDEO_DECODE_OUTPUT_ONLY      = 1 << 14,
        VIDEO_DECODE_DPB_ONLY         = 1 << 15,
        NO_DEFAULT_DESCRIPTORS        = 1 << 16,
        SHARED                        = 1 << 17,
        VIDEO_COMPATIBILITY_H264      = 1 << 18,
        VIDEO_COMPATIBILITY_H265      = 1 << 19,
    };

    // -----------------------------------------------------------------------
    // Bitwise operator overloads for flag enums
    // Allows: bind_flags | BindFlag::SHADER_RESOURCE
    //         bind_flags & BindFlag::RENDER_TARGET
    //         bind_flags |= BindFlag::UAV
    // without casting to uint32_t.
    // -----------------------------------------------------------------------

#define RHI_DEFINE_ENUM_BITOPS(E)                                                                          \
    inline constexpr E  operator| (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); } \
    inline constexpr E  operator& (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); } \
    inline constexpr E  operator^ (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b)); } \
    inline constexpr E  operator~ (E a)       noexcept { return static_cast<E>(~static_cast<uint32_t>(a)); }                           \
    inline constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }                                                          \
    inline constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }                                                          \
    inline constexpr E& operator^=(E& a, E b) noexcept { return a = a ^ b; }

    RHI_DEFINE_ENUM_BITOPS(BindFlag)
    RHI_DEFINE_ENUM_BITOPS(ResourceState)
    RHI_DEFINE_ENUM_BITOPS(ResourceMiscFlag)

#undef RHI_DEFINE_ENUM_BITOPS

    // Helper: test whether any bits are set (replaces the common cast-to-uint32 pattern).
    inline constexpr bool HasFlag(BindFlag     flags, BindFlag     test) noexcept { return (flags & test) != BindFlag::NONE; }
    inline constexpr bool HasFlag(ResourceState flags, ResourceState test) noexcept { return (flags & test) != ResourceState::UNDEFINED; }
    inline constexpr bool HasFlag(ResourceMiscFlag flags, ResourceMiscFlag test) noexcept { return (flags & test) != ResourceMiscFlag::NONE; }

    // -----------------------------------------------------------------------
    // Descriptor structs
    // -----------------------------------------------------------------------

    struct Viewport
    {
        float top_left_x = 0;
        float top_left_y = 0;
        float width      = 0;
        float height     = 0;
        float min_depth  = 0;
        float max_depth  = 1;
    };

    struct InputLayout
    {
        static constexpr uint32_t APPEND_ALIGNED_ELEMENT = ~0u;
        struct Element
        {
            // Raw pointer — must point to a string with static or sufficiently long lifetime
            // (e.g. a string literal like "POSITION"). No heap allocation at pipeline-build time.
            const char*         semantic_name     = nullptr;
            uint32_t            semantic_index    = 0;
            Format              format            = Format::UNKNOWN;
            uint32_t            input_slot        = 0;
            uint32_t            aligned_byte_offset = APPEND_ALIGNED_ELEMENT;
            InputClassification input_slot_class  = InputClassification::PER_VERTEX_DATA;
        };
        std::vector<Element> elements;
    };

    // -----------------------------------------------------------------------
    // ClearValue — static factory helpers avoid confusion between color and
    // depth/stencil union members.
    // -----------------------------------------------------------------------
    union ClearValue
    {
        float color[4];
        struct ClearDepthStencil { float depth; uint32_t stencil; } depth_stencil;

        static ClearValue Color(float r, float g, float b, float a = 1.0f) noexcept
        {
            ClearValue v{};
            v.color[0] = r; v.color[1] = g; v.color[2] = b; v.color[3] = a;
            return v;
        }

        // Reversed-Z convention: near plane is z=1, far plane is z=0. Clearing
        // to 0.0 means "start every pixel at the far plane"; subsequent draws
        // that are closer pass the GREATER_EQUAL depth test and overwrite it.
        static ClearValue DepthStencil(float depth = 0.0f, uint32_t stencil = 0) noexcept
        {
            ClearValue v{};
            v.depth_stencil.depth   = depth;
            v.depth_stencil.stencil = stencil;
            return v;
        }
    };

    enum class ComponentSwizzle : uint8_t { R, G, B, A, ZERO, ONE };

    struct TextureDesc
    {
        enum class Type : uint8_t { TEXTURE_1D, TEXTURE_2D, TEXTURE_3D } type = Type::TEXTURE_2D;
        Format           format       = Format::UNKNOWN;
        Usage            usage        = Usage::DEFAULT;
        BindFlag         bind_flags   = BindFlag::NONE;
        uint32_t         width        = 1;
        uint32_t         height       = 1;
        uint32_t         depth        = 1;
        uint32_t         array_size   = 1;
        uint32_t         mip_levels   = 1;
        uint32_t         sample_count = 1;
        ClearValue       clear        = {};
        ResourceMiscFlag misc_flags   = ResourceMiscFlag::NONE;
        ResourceState    layout       = ResourceState::SHADER_RESOURCE;
        // UTF-8. When set, GraphicsDX12::CreateTexture forwards it to
        // ID3D12Resource::SetName so D3D12 validation messages and PIX/RenderDoc
        // captures show a meaningful identifier instead of "Unnamed". Caller
        // owns the string for the duration of the CreateTexture call only —
        // SetName copies internally.
        const char*      debug_name   = nullptr;
    };

    struct SamplerDesc
    {
        Filter             filter          = Filter::MIN_MAG_MIP_LINEAR;
        TextureAddressMode address_u       = TextureAddressMode::WRAP;
        TextureAddressMode address_v       = TextureAddressMode::WRAP;
        TextureAddressMode address_w       = TextureAddressMode::WRAP;
        float              mip_lod_bias    = 0;
        uint32_t           max_anisotropy  = 0;
        ComparisonFunc     comparison_func = ComparisonFunc::NEVER;
        SamplerBorderColor border_color    = SamplerBorderColor::TRANSPARENT_BLACK;
        float              min_lod         = 0;
        float              max_lod         = (std::numeric_limits<float>::max)();
    };

    struct RasterizerState
    {
        FillMode fill_mode                         = FillMode::SOLID;
        CullMode cull_mode                         = CullMode::BACK;
        bool     front_counter_clockwise           = false;
        int32_t  depth_bias                        = 0;
        float    depth_bias_clamp                  = 0;
        float    slope_scaled_depth_bias           = 0;
        bool     depth_clip_enable                 = true;
        bool     multisample_enable                = false;
        bool     antialiased_line_enable           = false;
        bool     conservative_rasterization_enable = false;
        uint32_t forced_sample_count               = 0;
    };
    inline static constexpr RasterizerState default_rasterizerstate;

    struct DepthStencilState
    {
        bool           depth_enable       = false;
        DepthWriteMask depth_write_mask   = DepthWriteMask::ZERO;
        ComparisonFunc depth_func         = ComparisonFunc::LESS;
        bool           stencil_enable     = false;
        uint8_t        stencil_read_mask  = 0xff;
        uint8_t        stencil_write_mask = 0xff;
        struct DepthStencilOp
        {
            StencilOp      stencil_fail_op       = StencilOp::KEEP;
            StencilOp      stencil_depth_fail_op = StencilOp::KEEP;
            StencilOp      stencil_pass_op       = StencilOp::KEEP;
            ComparisonFunc stencil_func          = ComparisonFunc::ALWAYS;
        };
        DepthStencilOp front_face;
        DepthStencilOp back_face;
        bool           depth_bounds_test_enable = false;
    };
    inline static constexpr DepthStencilState default_depthstencilstate;

    struct BlendState
    {
        bool alpha_to_coverage_enable = false;
        bool independent_blend_enable = false;
        struct RenderTargetBlendState
        {
            bool      blend_enable              = false;
            Blend     src_blend                 = Blend::SRC_ALPHA;
            Blend     dest_blend                = Blend::INV_SRC_ALPHA;
            BlendOp   blend_op                  = BlendOp::ADD;
            Blend     src_blend_alpha           = Blend::ONE;
            Blend     dest_blend_alpha          = Blend::ONE;
            BlendOp   blend_op_alpha            = BlendOp::ADD;
            ColorWrite render_target_write_mask = ColorWrite::ENABLE_ALL;
        };
        RenderTargetBlendState render_target[8];
    };
    inline static constexpr BlendState default_blendstate;

    struct GPUBufferDesc
    {
        uint64_t         size       = 0;
        uint32_t         stride     = 0;     // for structured buffers
        uint32_t         alignment  = 0;
        Usage            usage      = Usage::DEFAULT;
        Format           format     = Format::UNKNOWN;
        BindFlag         bind_flags = BindFlag::NONE;
        ResourceMiscFlag misc_flags = ResourceMiscFlag::NONE;
    };

    // -----------------------------------------------------------------------
    // PVF (Programmable Vertex Fetching) types
    // -----------------------------------------------------------------------

    // Sentinel value: no buffer / stream absent
    static constexpr uint32_t kInvalidBufferIndex = 0xFFFFFFFFu;

    // Per-attribute format — values must never change (stored in MeshDescriptor on GPU)
    enum class VertexFormat : uint32_t
    {
        Float3      = 0,   // float3 — positions, normals
        Float2      = 1,   // float2 — UVs (full precision)
        Half2       = 2,   // float16×2 — UVs (packed)
        R10G10B10A2 = 3,   // packed normal / tangent
        R8G8B8A8    = 4,   // vertex color
        Float4      = 5,   // float4 — tangent with bitangent sign
        Invalid     = 0xFFFFFFFFu,
    };

    // Describes one vertex attribute stream (16 bytes, matches HLSL struct)
    struct StreamDescriptor
    {
        uint32_t bufferIndex = kInvalidBufferIndex;                          // bindless index into g_Buffers[]
        uint32_t byteOffset  = 0;                                            // byte offset to vertex 0
        uint32_t byteStride  = 0;                                            // bytes between consecutive vertices
        uint32_t format      = static_cast<uint32_t>(VertexFormat::Invalid); // VertexFormat enum value
    };

    // One mesh descriptor slot in the global GPU StructuredBuffer<MeshDescriptor>
    // Layout must match HLSL MeshDescriptor in pvf_fetch.hlsli (96 bytes)
    struct MeshDescriptor
    {
        StreamDescriptor position;           // always present
        StreamDescriptor normal;             // bufferIndex = kInvalidBufferIndex if absent
        StreamDescriptor tangent;            // bufferIndex = kInvalidBufferIndex if absent
        StreamDescriptor uv0;                // bufferIndex = kInvalidBufferIndex if absent
        StreamDescriptor uv1;                // bufferIndex = kInvalidBufferIndex if absent
        uint32_t indexBufferIndex = kInvalidBufferIndex; // INVALID if non-indexed
        uint32_t indexByteOffset  = 0;
        uint32_t indexFormat      = 0;       // 0 = uint16, 1 = uint32
        uint32_t vertexCount      = 0;
        // Total = 5×16 + 4×4 = 96 bytes
    };
    static_assert(sizeof(MeshDescriptor) == 96, "MeshDescriptor must be 96 bytes to match HLSL");

    struct GPUQueryHeapDesc
    {
        GpuQueryType type        = GpuQueryType::TIMESTAMP;
        uint32_t     query_count = 0;
    };

    // -----------------------------------------------------------------------
    // GPU resource handles — lightweight value types (no heap allocation).
    //
    // handle_id is an opaque index into the backend's per-type resource pool.
    // ~0u  means invalid / not yet created.
    // The backend fills handle_id on successful resource creation.
    //
    // Derived types carry a copy of the creation descriptor for convenience;
    // the authoritative data lives in the backend pool.
    // -----------------------------------------------------------------------

    struct GPUResource
    {
        // Discriminates which backend pool owns this handle.
        // Set by the backend on creation; lets generic GPUResource* parameters
        // (e.g. GPUBarrier::Memory) resolve to the correct pool without dynamic_cast.
        enum class Type : uint8_t { Unknown, Buffer, Texture, Shader, PipelineState, PipelineLayout };

        uint32_t handle_id = ~0u;
        Type     type      = Type::Unknown;

        constexpr bool IsValid() const noexcept { return handle_id != ~0u; }
        void Reset() noexcept { handle_id = ~0u; type = Type::Unknown; }
    };

    struct GPUBuffer : GPUResource
    {
        GPUBufferDesc desc{};
    };

    struct Texture : GPUResource
    {
        TextureDesc desc{};
    };

    struct Shader : GPUResource
    {
        ShaderStage stage = ShaderStage::Count;
    };

    struct PipelineState : GPUResource
    {
        // All pipeline state lives in the backend pool (PipelineState_DX12 etc.)
    };

    // -----------------------------------------------------------------------
    // PipelineLayout (Root Signature in DX12, Pipeline Layout in Vulkan)
    //
    // Describes how shader resources are bound (descriptor tables, push
    // constants, static samplers). Must be created before a PipelineState
    // that references it.
    // -----------------------------------------------------------------------
    struct PipelineLayout : GPUResource
    {
        // All layout data lives in the backend pool.
    };

    // -----------------------------------------------------------------------
    // PipelineStateDesc — references Shader and PipelineLayout handles
    // -----------------------------------------------------------------------
    struct PipelineStateDesc
    {
        const PipelineLayout*    layout = nullptr;  // required; describes resource binding layout
        const Shader*          vs  = nullptr;
        const Shader*          ps  = nullptr;
        const Shader*          hs  = nullptr;
        const Shader*          ds  = nullptr;
        const Shader*          gs  = nullptr;
        const Shader*          ms  = nullptr;   // mesh shader
        const Shader*          as  = nullptr;   // amplification shader
        const Shader*          cs  = nullptr;   // compute shader
        const BlendState*      bs  = nullptr;
        const RasterizerState* rs  = nullptr;
        const DepthStencilState* dss = nullptr;
        const InputLayout*     il  = nullptr;
        PrimitiveTopology      pt  = PrimitiveTopology::TRIANGLELIST;
        uint32_t               patch_control_points = 3;
        uint32_t               sample_mask          = 0xFFFFFFFF;
        // Render target formats (up to 8 colour + 1 depth)
        Format                 rtv_formats[8] = { Format::R8G8B8A8_UNORM };
        uint32_t               rtv_count      = 1;
        Format                 dsv_format     = Format::UNKNOWN;
        uint32_t               sample_count   = 1;
        // Stable key for the PSO disk library.  Set by PSOCache::CreateNew via
        // PSODesc::Hash() (ShaderID-based, deterministic across runs).  Zero
        // means "not set"; ComputePSOName falls back to handle_id hashing.
        uint64_t               cache_key      = 0;
    };

    // -----------------------------------------------------------------------
    // GPU Barriers
    // -----------------------------------------------------------------------
    struct GPUBarrier
    {
        enum class Type { MEMORY, IMAGE, BUFFER, ALIASING } type = Type::MEMORY;

        struct Memory   { const GPUResource* resource; };
        struct Image
        {
            const Texture* texture;
            ResourceState  layout_before;
            ResourceState  layout_after;
            int            mip;
            int            slice;
        };
        struct Buffer
        {
            const GPUBuffer* buffer;
            ResourceState    state_before;
            ResourceState    state_after;
        };
        struct Aliasing
        {
            const GPUResource* resource_before;
            const GPUResource* resource_after;
        };
        union { Memory memory; Image image; Buffer buffer; Aliasing aliasing; };

        GPUBarrier() {}

        static GPUBarrier Memory(const GPUResource* res = nullptr) noexcept
        {
            GPUBarrier b; b.type = Type::MEMORY; b.memory.resource = res; return b;
        }
        static GPUBarrier Image(const Texture* tex,
            ResourceState before, ResourceState after,
            int mip = -1, int slice = -1) noexcept
        {
            GPUBarrier b; b.type = Type::IMAGE;
            b.image = { tex, before, after, mip, slice }; return b;
        }
        static GPUBarrier Buffer(const GPUBuffer* buf,
            ResourceState before, ResourceState after) noexcept
        {
            GPUBarrier b; b.type = Type::BUFFER;
            b.buffer = { buf, before, after }; return b;
        }
        static GPUBarrier Aliasing(const GPUResource* before,
                                   const GPUResource* after) noexcept
        {
            GPUBarrier b; b.type = Type::ALIASING;
            b.aliasing = { before, after }; return b;
        }
    };

    struct SwapChainDesc
    {
        uint32_t width        = 0;
        uint32_t height       = 0;
        uint32_t buffer_count = 2;
        Format   format       = Format::R10G10B10A2_UNORM;
        bool     fullscreen   = false;
        bool     vsync        = true;
        float    clear_color[4] = { 0, 0, 0, 1 };
        bool     allow_hdr    = true;
    };

    struct SubresourceData
    {
        const void* data_ptr    = nullptr;
        uint32_t    row_pitch   = 0;
        uint32_t    slice_pitch = 0;
    };

    // -----------------------------------------------------------------------
    // Queue type and command list handle
    // -----------------------------------------------------------------------

    enum class QUEUE_TYPE : uint8_t
    {
        GRAPHICS = 0,   // D3D12_COMMAND_LIST_TYPE_DIRECT
        COMPUTE  = 1,   // D3D12_COMMAND_LIST_TYPE_COMPUTE
        COPY     = 2,   // D3D12_COMMAND_LIST_TYPE_COPY
        COUNT    = 3,
    };

} // namespace RHI

// -----------------------------------------------------------------------
// RenderGraph virtual texture types — defined here so CommandList can use
// RGTextureHandle without a circular include through RGTypes.h.
// -----------------------------------------------------------------------
namespace RG
{
    struct RGTextureHandle
    {
        static constexpr uint32_t INVALID_ID = ~0u;
        uint32_t id = INVALID_ID;
        bool IsValid() const noexcept { return id != INVALID_ID; }
        bool operator==(const RGTextureHandle& o) const noexcept { return id == o.id; }
        bool operator!=(const RGTextureHandle& o) const noexcept { return id != o.id; }
    };

    struct RGTextureDesc
    {
        RHI::Format    format    = RHI::Format::UNKNOWN;
        uint32_t       width     = 0;   // 0 = match render dimensions
        uint32_t       height    = 0;
        bool           isDepth   = false;
        // Set to true when any pass in the graph will bind this texture as a
        // UAV (e.g. Decal Apply Pass writes into the GBuffer in-place). Causes
        // the physical resource to be created with BindFlag::UNORDERED_ACCESS
        // and a UAV descriptor to be allocated.
        bool           isUAV     = false;
        const wchar_t* debugName = nullptr;
    };
} // namespace RG

// -----------------------------------------------------------------------
// CommandList — unified handle + recording interface.
//
// IGraphicsDevice::BeginCommandList returns a CommandList with only
// internal_id set (gfx/ctx are null).  RenderGraph::Execute attaches gfx
// and ctx before handing it to each pass, making it fully recording-capable.
//
// All IGraphicsDevice virtual methods still accept CommandList so the
// opaque-handle contract is preserved.
// -----------------------------------------------------------------------
namespace RHI
{
    struct CommandList
    {
        uint32_t           internal_id = ~0u;
        IGraphicsDevice*   gfx         = nullptr;  // non-owning; set by RenderGraph
        RG::RenderContext* ctx         = nullptr;  // non-owning; set by RenderGraph

        constexpr bool IsValid() const noexcept { return internal_id != ~0u; }
        bool operator==(const CommandList& o) const noexcept { return internal_id == o.internal_id; }

        // ---- Device / context accessors -------------------------------------
        IGraphicsDevice&   GetDevice()  const;
        RG::RenderContext& GetContext() const;

        // ---- Viewport / scissor / topology ----------------------------------
        void SetViewport(uint32_t width = 0, uint32_t height = 0);
        void SetScissorRect(uint32_t width = 0, uint32_t height = 0);
        void SetPrimitiveTopology(PrimitiveTopology topo = PrimitiveTopology::TRIANGLELIST);

        // ---- Pipeline state -------------------------------------------------
        void SetPipelineState(const PipelineState& pso);

        // ---- Render targets — RG virtual-handle variants --------------------
        void SetRenderTarget(RG::RGTextureHandle rtv, RG::RGTextureHandle dsv = {});
        void SetRenderTargets(std::initializer_list<RG::RGTextureHandle> rtvs,
                              RG::RGTextureHandle dsv = {});
        // Like SetRenderTargets but appends the device-internal HdrSceneColor
        // as the last RT (RT[rtvs.size()]). Used by GBufferPass / TerrainPass
        // for Unreal-style direct emissive write to scene color.
        void SetRenderTargetsAndHdr(std::initializer_list<RG::RGTextureHandle> rtvs,
                                    RG::RGTextureHandle dsv = {});
        void ClearRenderTarget(RG::RGTextureHandle h, const float color[4]);
        void ClearHdrRenderTarget(const float color[4]);
        void ClearDepthStencil(RG::RGTextureHandle h,
                               float depth = 0.0f, uint8_t stencil = 0); // reversed Z: far = 0

        // ---- Render targets — raw RHI::Texture* variants (e.g. PickingPass) -
        void SetRenderTargets(uint32_t numRTs,
                              const Texture* const* rtvs,
                              const Texture*        dsv);
        void ClearRenderTarget(const Texture& tex, const float color[4]);
        void ClearDepthStencil(const Texture& tex,
                               float depth = 0.0f, uint8_t stencil = 0); // reversed Z: far = 0

        // ---- Descriptor heap / root bindings --------------------------------
        void BindDescriptorHeaps();
        void BindCBByName(uint32_t slot, const char* name);
        void BindSRVByHandle(uint32_t slot, RG::RGTextureHandle h);
        void BindSampler(uint32_t slot, int samplerIndex);
        void SetPVFRootConstants(uint32_t meshDescIdx,
                                 uint32_t instanceOffset,
                                 uint32_t materialIndex);
        void BindBufferSRVByName(uint32_t rootSlot, const char* name);
        void BindBufferSRV(uint32_t slot, const GPUBuffer& buf);
        void BindDescriptorTableHandle(uint32_t rootSlot, uint64_t gpuHandle);

        // ---- Draw calls -----------------------------------------------------
        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount = 1,
                           uint32_t startVertex = 0, uint32_t startInstance = 0);
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t startIndex, int32_t baseVertex,
                                  uint32_t startInstance);
        void DrawFullscreenTriangle();

        // Mesh-shader dispatch (DX12 Ultimate). Forwards to
        // IGraphicsDevice::DispatchMesh — when an Amplification Shader is
        // bound, the AS receives the (x,y,z) group counts and chooses the
        // MS dispatch via DispatchMesh from inside the AS.
        void DispatchMesh(uint32_t threadGroupX,
                          uint32_t threadGroupY = 1,
                          uint32_t threadGroupZ = 1);

        // ---- Barriers / copies ----------------------------------------------
        void PushBarrier(const GPUBarrier& barrier);
        void CopyTexturePixelToBuffer(const Texture& src,
                                      uint32_t srcX, uint32_t srcY,
                                      GPUBuffer& dst);

        // ---- Dimension / bindless queries -----------------------------------
        uint32_t GetWidth()              const;
        uint32_t GetHeight()             const;
        uint64_t GetBindlessTableHandle() const;
    };

} // namespace RHI
