#include "Graphics/GraphicsDX12.h"
#include "Graphics/DxcCompiler.h"
#include "System/Log.h"
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <queue>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "d3dx12.h"
#include <d3d12sdklayers.h>
using Microsoft::WRL::ComPtr;

// ===========================================================================
// ThrowIfFailed
// ===========================================================================

void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line)
{
    if (FAILED(hr))
    {
        Logger::Get().LogHResult(expr, hr, file, line);
        throw std::runtime_error("DX12 call failed");
    }
}

// ===========================================================================
// Internal DX12 resource types — stored in per-type pools inside GraphicsDX12.
// Accessed by GPUResource::handle_id (pool index).  No virtual dispatch, no heap
// allocation per resource — pools are std::vector<T> grown at creation time.
// ===========================================================================

struct Texture_DX12
{
    ComPtr<ID3D12Resource>  resource;
    D3D12_RESOURCE_STATES   state = D3D12_RESOURCE_STATE_COMMON;
    DescriptorAllocation    srv;        // CBV_SRV_UAV shader-visible heap (native format, incl. SRGB)
    DescriptorAllocation    previewSrv; // UNORM alias SRV for editor preview (only if texture is SRGB)
    DescriptorAllocation    uav;        // CBV_SRV_UAV shader-visible heap
    DescriptorAllocation    rtv;        // RTV heap
    DescriptorAllocation    dsv;        // DSV heap
    // Per-mip UAVs (only populated when mip_levels > 1 + UAV bind flag).
    // For TextureCube / Texture2DArray: each UAV views all array slices at that mip.
    // For Texture3D: volume UAV at that mip. For plain 2D: 2D UAV at that mip.
    std::vector<DescriptorAllocation> mipUavs;
    // Per-array-slice DSVs (only populated when array_size > 1 + DEPTH_STENCIL bind flag).
    // Used by CSM cascades to render/clear one cascade at a time.
    std::vector<DescriptorAllocation> sliceDsvs;

    // Lazy per-(cube, face, mip) RTVs for cubemap-array render targets — keys
    // pack as (cubeIdx<<16)|(face<<8)|mip. Allocated on first request from
    // GetTextureCubeFaceRTVCpuHandle so probes that are never baked don't
    // burn descriptor slots.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeFaceRtvs;
    // Lazy per-(cube, mip) UAVs viewing the 6 slices [cubeIdx*6..+5] at one
    // mip — feeds the prefilter compute shader. Key = (cubeIdx<<8)|mip.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeMipUavs;
};

struct GPUBuffer_DX12
{
    ComPtr<ID3D12Resource>   resource;
    D3D12_RESOURCE_STATES    state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    DescriptorAllocation     cbv;   // CBV_SRV_UAV shader-visible heap
    DescriptorAllocation     srv;   // CBV_SRV_UAV shader-visible heap (structured/typed)
    DescriptorAllocation     uav;   // CBV_SRV_UAV shader-visible heap
};

struct Shader_DX12
{
    std::vector<uint8_t> bytecode;
    // Content-addressed hash of `bytecode`, computed once at CreateShader
    // time. Used by ComputePSOName's fallback so compute/post PSOs (which
    // don't go through PSOCache to set a stable cache_key) still get
    // deterministic names across runs — without it, every launch generated
    // new disk-library entries and pso_cache.bin grew unbounded.
    uint64_t             bytecodeHash = 0;
};

struct PipelineState_DX12
{
    ComPtr<ID3D12PipelineState>    pso;
    ComPtr<ID3D12RootSignature>    rootSignature;
    bool                           isCompute = false;
};


// ===========================================================================
// Root signature slot constants (PVF layout)
// [0]      ROOT_CONSTANTS 3×uint32 b0              → meshDescIdx / instanceOffset / materialIndex
// [1..7]   ROOT_CBV b1-b7                          → BindConstantBuffer(slot 0-6)
// [8]      ROOT_SRV t0 space0                       → InstanceBuffer
// [9]      ROOT_SRV t1 space0                       → MeshDescriptors
// [10..13] DESC_TABLE 1 SRV each t2-t5 space0       → BindResource(slot 0-3)
// [14]     DESC_TABLE 64 SRV t0 space1              → bindless g_Buffers[]
// [15..18] DESC_TABLE 1 sampler each s0-s3          → BindSampler(slot 0-3)
// ===========================================================================
static constexpr UINT kRootConstantsSlot  =  0;
static constexpr UINT kCBVSlotBase        =  1;   // root params 1-7  → b1-b7
static constexpr UINT kCBVSlotCount       =  7;
static constexpr UINT kInstanceBufSlot    =  8;   // root param 8     → t0 space0
static constexpr UINT kMeshDescSlot       =  9;   // root param 9     → t1 space0
static constexpr UINT kSRVSlotBase        = 10;   // root params 10-13 → t2-t5 space0
static constexpr UINT kSRVSlotCount       =  4;
static constexpr UINT kBindlessSlot       = 14;   // root param 14    → t0 space1 (64 slots)
static constexpr UINT kSamplerSlotBase    = 15;   // root params 15-18 → s0-s3
static constexpr UINT kSamplerSlotCount   =  4;
static constexpr UINT kIBLSRVSlotBase     = 19;   // root params 19-21 → t6-t8 space0 (IBL cubemaps + BRDF LUT)
static constexpr UINT kIBLSRVSlotCount    =  3;
static constexpr UINT kShadowSRVSlot      = 22;   // root param 22     → t9-t11 space0 (CSM shadow cascade array, 3 entries)
static constexpr UINT kShadowSRVCount     =  3;
static constexpr UINT kClusterSRVSlotBase = 23;   // root params 23-25 → t10-t12 space0 (clustered lights, index list, grid)
static constexpr UINT kClusterSRVSlotCount=  3;
static constexpr UINT kMaxBindlessBuffers = 4096;  // must match kMaxBuffers in MeshDescriptorHeap.h and g_Buffers[] in pvf_fetch.hlsli

// ===========================================================================
// DX12 enum / format conversion helpers
// ===========================================================================

DXGI_FORMAT GraphicsDX12::ToDxgiFormat(RHI::Format f)
{
    switch (f)
    {
    case RHI::Format::UNKNOWN:               return DXGI_FORMAT_UNKNOWN;
    case RHI::Format::R32G32B32A32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RHI::Format::R32G32B32A32_UINT:     return DXGI_FORMAT_R32G32B32A32_UINT;
    case RHI::Format::R32G32B32A32_SINT:     return DXGI_FORMAT_R32G32B32A32_SINT;
    case RHI::Format::R32G32B32_FLOAT:       return DXGI_FORMAT_R32G32B32_FLOAT;
    case RHI::Format::R32G32B32_UINT:        return DXGI_FORMAT_R32G32B32_UINT;
    case RHI::Format::R32G32B32_SINT:        return DXGI_FORMAT_R32G32B32_SINT;
    case RHI::Format::R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RHI::Format::R16G16B16A16_UNORM:    return DXGI_FORMAT_R16G16B16A16_UNORM;
    case RHI::Format::R16G16B16A16_UINT:     return DXGI_FORMAT_R16G16B16A16_UINT;
    case RHI::Format::R16G16B16A16_SNORM:    return DXGI_FORMAT_R16G16B16A16_SNORM;
    case RHI::Format::R16G16B16A16_SINT:     return DXGI_FORMAT_R16G16B16A16_SINT;
    case RHI::Format::R32G32_FLOAT:          return DXGI_FORMAT_R32G32_FLOAT;
    case RHI::Format::R32G32_UINT:           return DXGI_FORMAT_R32G32_UINT;
    case RHI::Format::R32G32_SINT:           return DXGI_FORMAT_R32G32_SINT;
    case RHI::Format::D32_FLOAT_S8X24_UINT:  return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case RHI::Format::R10G10B10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case RHI::Format::R10G10B10A2_UINT:      return DXGI_FORMAT_R10G10B10A2_UINT;
    case RHI::Format::R11G11B10_FLOAT:       return DXGI_FORMAT_R11G11B10_FLOAT;
    case RHI::Format::R8G8B8A8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RHI::Format::R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case RHI::Format::R8G8B8A8_UINT:         return DXGI_FORMAT_R8G8B8A8_UINT;
    case RHI::Format::R8G8B8A8_SNORM:        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case RHI::Format::R8G8B8A8_SINT:         return DXGI_FORMAT_R8G8B8A8_SINT;
    case RHI::Format::B8G8R8A8_UNORM:        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RHI::Format::B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case RHI::Format::R16G16_FLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
    case RHI::Format::R16G16_UNORM:          return DXGI_FORMAT_R16G16_UNORM;
    case RHI::Format::R16G16_UINT:           return DXGI_FORMAT_R16G16_UINT;
    case RHI::Format::R16G16_SNORM:          return DXGI_FORMAT_R16G16_SNORM;
    case RHI::Format::R16G16_SINT:           return DXGI_FORMAT_R16G16_SINT;
    case RHI::Format::D32_FLOAT:             return DXGI_FORMAT_D32_FLOAT;
    case RHI::Format::R32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
    case RHI::Format::R32_UINT:              return DXGI_FORMAT_R32_UINT;
    case RHI::Format::R32_SINT:              return DXGI_FORMAT_R32_SINT;
    case RHI::Format::D24_UNORM_S8_UINT:     return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RHI::Format::R9G9B9E5_SHAREDEXP:    return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    case RHI::Format::R8G8_UNORM:            return DXGI_FORMAT_R8G8_UNORM;
    case RHI::Format::R8G8_UINT:             return DXGI_FORMAT_R8G8_UINT;
    case RHI::Format::R8G8_SNORM:            return DXGI_FORMAT_R8G8_SNORM;
    case RHI::Format::R8G8_SINT:             return DXGI_FORMAT_R8G8_SINT;
    case RHI::Format::R16_FLOAT:             return DXGI_FORMAT_R16_FLOAT;
    case RHI::Format::D16_UNORM:             return DXGI_FORMAT_D16_UNORM;
    case RHI::Format::R16_UNORM:             return DXGI_FORMAT_R16_UNORM;
    case RHI::Format::R16_UINT:              return DXGI_FORMAT_R16_UINT;
    case RHI::Format::R16_SNORM:             return DXGI_FORMAT_R16_SNORM;
    case RHI::Format::R16_SINT:              return DXGI_FORMAT_R16_SINT;
    case RHI::Format::R8_UNORM:              return DXGI_FORMAT_R8_UNORM;
    case RHI::Format::R8_UINT:               return DXGI_FORMAT_R8_UINT;
    case RHI::Format::R8_SNORM:              return DXGI_FORMAT_R8_SNORM;
    case RHI::Format::R8_SINT:               return DXGI_FORMAT_R8_SINT;
    case RHI::Format::BC1_UNORM:             return DXGI_FORMAT_BC1_UNORM;
    case RHI::Format::BC1_UNORM_SRGB:        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case RHI::Format::BC2_UNORM:             return DXGI_FORMAT_BC2_UNORM;
    case RHI::Format::BC2_UNORM_SRGB:        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case RHI::Format::BC3_UNORM:             return DXGI_FORMAT_BC3_UNORM;
    case RHI::Format::BC3_UNORM_SRGB:        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case RHI::Format::BC4_UNORM:             return DXGI_FORMAT_BC4_UNORM;
    case RHI::Format::BC4_SNORM:             return DXGI_FORMAT_BC4_SNORM;
    case RHI::Format::BC5_UNORM:             return DXGI_FORMAT_BC5_UNORM;
    case RHI::Format::BC5_SNORM:             return DXGI_FORMAT_BC5_SNORM;
    case RHI::Format::BC6H_UF16:             return DXGI_FORMAT_BC6H_UF16;
    case RHI::Format::BC6H_SF16:             return DXGI_FORMAT_BC6H_SF16;
    case RHI::Format::BC7_UNORM:             return DXGI_FORMAT_BC7_UNORM;
    case RHI::Format::BC7_UNORM_SRGB:        return DXGI_FORMAT_BC7_UNORM_SRGB;
    case RHI::Format::NV12:                  return DXGI_FORMAT_NV12;
    default:                                 return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_RESOURCE_STATES GraphicsDX12::ToD3D12ResourceState(RHI::ResourceState s)
{
    // Map the first set bit (single state assumed)
    using RS = RHI::ResourceState;
    switch (s)
    {
    case RS::UNDEFINED:                         return D3D12_RESOURCE_STATE_COMMON;
    case RS::SHADER_RESOURCE:                   return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case RS::SHADER_RESOURCE_COMPUTE:           return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case RS::UNORDERED_ACCESS:                  return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case RS::COPY_SRC:                          return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case RS::COPY_DST:                          return D3D12_RESOURCE_STATE_COPY_DEST;
    case RS::RENDERTARGET:                      return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case RS::DEPTHSTENCIL:                      return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case RS::DEPTHSTENCIL_READONLY:             return D3D12_RESOURCE_STATE_DEPTH_READ;
    case RS::DEPTH_READ_SRV:                    return D3D12_RESOURCE_STATE_DEPTH_READ |
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case RS::VERTEX_BUFFER:                     return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case RS::INDEX_BUFFER:                      return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case RS::CONSTANT_BUFFER:                   return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case RS::INDIRECT_ARGUMENT:                 return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case RS::RAYTRACING_ACCELERATION_STRUCTURE: return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    case RS::PREDICATION:                       return D3D12_RESOURCE_STATE_PREDICATION;
    default:                                    return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_FILTER GraphicsDX12::ToD3D12Filter(RHI::Filter f)
{
    switch (f)
    {
    case RHI::Filter::MIN_MAG_MIP_POINT:              return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case RHI::Filter::MIN_MAG_POINT_MIP_LINEAR:       return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MIN_POINT_MAG_LINEAR_MIP_POINT: return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MIN_POINT_MAG_MIP_LINEAR:       return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    case RHI::Filter::MIN_LINEAR_MAG_MIP_POINT:       return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    case RHI::Filter::MIN_LINEAR_MAG_POINT_MIP_LINEAR:return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MIN_MAG_LINEAR_MIP_POINT:       return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MIN_MAG_MIP_LINEAR:             return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::ANISOTROPIC:                    return D3D12_FILTER_ANISOTROPIC;
    case RHI::Filter::COMPARISON_MIN_MAG_MIP_POINT:   return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    case RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR:  return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::COMPARISON_ANISOTROPIC:         return D3D12_FILTER_COMPARISON_ANISOTROPIC;
    default:                                          return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

D3D12_TEXTURE_ADDRESS_MODE GraphicsDX12::ToD3D12AddressMode(RHI::TextureAddressMode m)
{
    switch (m)
    {
    case RHI::TextureAddressMode::WRAP:        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case RHI::TextureAddressMode::MIRROR:      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case RHI::TextureAddressMode::CLAMP:       return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case RHI::TextureAddressMode::BORDER:      return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case RHI::TextureAddressMode::MIRROR_ONCE: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:                                   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_COMPARISON_FUNC GraphicsDX12::ToD3D12ComparisonFunc(RHI::ComparisonFunc f)
{
    switch (f)
    {
    case RHI::ComparisonFunc::NEVER:         return D3D12_COMPARISON_FUNC_NEVER;
    case RHI::ComparisonFunc::LESS:          return D3D12_COMPARISON_FUNC_LESS;
    case RHI::ComparisonFunc::EQUAL:         return D3D12_COMPARISON_FUNC_EQUAL;
    case RHI::ComparisonFunc::LESS_EQUAL:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case RHI::ComparisonFunc::GREATER:       return D3D12_COMPARISON_FUNC_GREATER;
    case RHI::ComparisonFunc::NOT_EQUAL:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case RHI::ComparisonFunc::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case RHI::ComparisonFunc::ALWAYS:        return D3D12_COMPARISON_FUNC_ALWAYS;
    default:                                 return D3D12_COMPARISON_FUNC_NEVER;
    }
}

D3D12_BLEND GraphicsDX12::ToD3D12Blend(RHI::Blend b)
{
    switch (b)
    {
    case RHI::Blend::ZERO:             return D3D12_BLEND_ZERO;
    case RHI::Blend::ONE:              return D3D12_BLEND_ONE;
    case RHI::Blend::SRC_COLOR:        return D3D12_BLEND_SRC_COLOR;
    case RHI::Blend::INV_SRC_COLOR:    return D3D12_BLEND_INV_SRC_COLOR;
    case RHI::Blend::SRC_ALPHA:        return D3D12_BLEND_SRC_ALPHA;
    case RHI::Blend::INV_SRC_ALPHA:    return D3D12_BLEND_INV_SRC_ALPHA;
    case RHI::Blend::DEST_ALPHA:       return D3D12_BLEND_DEST_ALPHA;
    case RHI::Blend::INV_DEST_ALPHA:   return D3D12_BLEND_INV_DEST_ALPHA;
    case RHI::Blend::DEST_COLOR:       return D3D12_BLEND_DEST_COLOR;
    case RHI::Blend::INV_DEST_COLOR:   return D3D12_BLEND_INV_DEST_COLOR;
    case RHI::Blend::SRC_ALPHA_SAT:    return D3D12_BLEND_SRC_ALPHA_SAT;
    case RHI::Blend::BLEND_FACTOR:     return D3D12_BLEND_BLEND_FACTOR;
    case RHI::Blend::INV_BLEND_FACTOR: return D3D12_BLEND_INV_BLEND_FACTOR;
    case RHI::Blend::SRC1_COLOR:       return D3D12_BLEND_SRC1_COLOR;
    case RHI::Blend::INV_SRC1_COLOR:   return D3D12_BLEND_INV_SRC1_COLOR;
    case RHI::Blend::SRC1_ALPHA:       return D3D12_BLEND_SRC1_ALPHA;
    case RHI::Blend::INV_SRC1_ALPHA:   return D3D12_BLEND_INV_SRC1_ALPHA;
    default:                           return D3D12_BLEND_ZERO;
    }
}

D3D12_BLEND_OP GraphicsDX12::ToD3D12BlendOp(RHI::BlendOp op)
{
    switch (op)
    {
    case RHI::BlendOp::ADD:          return D3D12_BLEND_OP_ADD;
    case RHI::BlendOp::SUBTRACT:     return D3D12_BLEND_OP_SUBTRACT;
    case RHI::BlendOp::REV_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
    case RHI::BlendOp::MIN:          return D3D12_BLEND_OP_MIN;
    case RHI::BlendOp::MAX:          return D3D12_BLEND_OP_MAX;
    default:                         return D3D12_BLEND_OP_ADD;
    }
}

D3D12_FILL_MODE GraphicsDX12::ToD3D12FillMode(RHI::FillMode m)
{
    return (m == RHI::FillMode::WIREFRAME) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

D3D12_CULL_MODE GraphicsDX12::ToD3D12CullMode(RHI::CullMode m)
{
    switch (m)
    {
    case RHI::CullMode::NONE:  return D3D12_CULL_MODE_NONE;
    case RHI::CullMode::FRONT: return D3D12_CULL_MODE_FRONT;
    case RHI::CullMode::BACK:  return D3D12_CULL_MODE_BACK;
    default:                   return D3D12_CULL_MODE_NONE;
    }
}

D3D12_RESOURCE_FLAGS GraphicsDX12::ToD3D12ResourceFlags(RHI::BindFlag flags)
{
    D3D12_RESOURCE_FLAGS out = D3D12_RESOURCE_FLAG_NONE;
    auto f = static_cast<uint32_t>(flags);
    if (f & static_cast<uint32_t>(RHI::BindFlag::RENDER_TARGET))    out |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (f & static_cast<uint32_t>(RHI::BindFlag::DEPTH_STENCIL))    out |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (f & static_cast<uint32_t>(RHI::BindFlag::UNORDERED_ACCESS)) out |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return out;
}

D3D12_HEAP_TYPE GraphicsDX12::ToD3D12HeapType(RHI::Usage usage)
{
    switch (usage)
    {
    case RHI::Usage::UPLOAD:   return D3D12_HEAP_TYPE_UPLOAD;
    case RHI::Usage::READBACK: return D3D12_HEAP_TYPE_READBACK;
    default:                   return D3D12_HEAP_TYPE_DEFAULT;
    }
}

D3D12_PRIMITIVE_TOPOLOGY GraphicsDX12::ToD3D12Topology(RHI::PrimitiveTopology t)
{
    switch (t)
    {
    case RHI::PrimitiveTopology::POINTLIST:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RHI::PrimitiveTopology::LINELIST:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RHI::PrimitiveTopology::LINESTRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RHI::PrimitiveTopology::TRIANGLELIST:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case RHI::PrimitiveTopology::TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default:                                    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

// ===========================================================================
// Default root signature builder
// ===========================================================================

ComPtr<ID3D12RootSignature> GraphicsDX12::CreateDefaultRootSignature()
{
    // PVF root signature — 23 parameters, no IA input layout flag.
    //  [0]      ROOT_CONSTANTS 3×uint32 b0 space0
    //  [1..7]   ROOT_CBV b1-b7 space0
    //  [8]      ROOT_SRV t0 space0  (InstanceBuffer)
    //  [9]      ROOT_SRV t1 space0  (MeshDescriptors)
    //  [10..13] DESC_TABLE 1 SRV t2-t5 space0
    //  [14]     DESC_TABLE 64 SRV t0 space1 (bindless g_Buffers[])
    //  [15..18] DESC_TABLE 1 sampler s0-s3
    //  [19..21] DESC_TABLE 1 SRV t6-t8 space0 (IBL irradiance / radiance cubemaps + BRDF LUT)
    //  [22]     DESC_TABLE 3 SRV t9-t11 space0 (CSM shadow cascade maps)
    //  [23..25] DESC_TABLE 1 SRV t13-t15 space0 (clustered lights, index list, grid)
    //  [26]     DESC_TABLE 1 SRV t16 space0 (NPR ramp texture)
    //  [27]     DESC_TABLE N SRV t0 space2 (bindless texture array)
    //  [28]     DESC_TABLE 1 SRV t12 space0 (MaterialBuffer)
    //  [29]     DESC_TABLE 1 SRV t17 space0 (GBuffer emissive)
    //  [30]     DESC_TABLE 1 SRV t18 space0 (SSAO / XeGTAO)
    //  [31]     DESC_TABLE 1 SRV t19 space0 (Sky SH coefficient buffer — StructuredBuffer<float4>)
    //  [32]     DESC_TABLE 1 SRV t20 space0 (Aerial Perspective 3D LUT — Texture3D<float4>)

    constexpr UINT kRampTexSlot      = 26; // t16 space0 — NPR ramp texture
    constexpr UINT kBindlessTexSlot  = 27; // t0 space2 — bindless texture array
    constexpr UINT kMaterialBufSlot  = 28; // t12 space0 — MaterialBuffer (for NPR per-material params)
    constexpr UINT kEmissiveSRVSlot  = 29; // t17 space0 — GBuffer emissive SRV (deferred lighting)
    constexpr UINT kSSAOSRVSlot      = 30; // t18 space0 — SSAO texture (XeGTAO, one-frame latency)
    constexpr UINT kSkySHSRVSlot      = 31; // t19 space0 — Sky SH coefficient buffer (9 × float4)
    constexpr UINT kAerialPerspSlot   = 32; // t20 space0 — Aerial Perspective 3D LUT (Texture3D<float4>)
    constexpr UINT kSpotShadowAtlasSlot = 33; // t21 space0 — Texture2DArray<float> SpotShadowPass atlas
    constexpr UINT kSpotShadowVPSlot  = 34; // t22 space0 — StructuredBuffer<float4x4> per-slice VPs
    constexpr UINT kCustomMatCBVSlot  = 35; // b8 space0 — Phase E per-material CBV packed from shader reflection
    constexpr UINT kCustomMatTexSlot  = 36; // t0-t(N-1) space3 — Phase F per-material named texture table
    // Source of truth is GraphicsDX12::kCustomMatTextureSlots (header). Use
    // that here so root-sig + descriptor-table sizes can never drift.
    constexpr UINT kCustomMatTexCount = GraphicsDX12::kCustomMatTextureSlots;
    constexpr UINT kReflectionProbeArraySlot  = 37; // t23 space0 — TextureCubeArray<float4> reflection probe pool
    constexpr UINT kReflectionProbeBufferSlot = 38; // t24 space0 — StructuredBuffer<GPUReflectionProbe>
    constexpr UINT kReflectionProbeGridSlot   = 39; // t25 space0 — StructuredBuffer<ProbeGridEntry>
    constexpr UINT kReflectionProbeIndexSlot  = 40; // t26 space0 — StructuredBuffer<uint> probe index list
    constexpr UINT kSSRResultSlot             = 41; // t27 space0 — Texture2D<float4> SSR trace (prev-frame)
    // DDGI bindings — multi-volume. Each per-volume table holds DDGI_MAX_VOLUMES
    // contiguous SRVs into the GPU heap, populated by DDGIVolumeManager when a
    // slot is allocated/freed. Lighting.ps loops `vi < ddgiVolumeCount` to skip
    // unused entries (which still hold valid null SRVs to satisfy validation).
    //
    // Register layout (must match Lighting.ps.hlsl + DDGICommon.hlsli DDGI_MAX_VOLUMES):
    //   t28           — StructuredBuffer<DDGIVolumeGPU> (single, count gates loop)
    //   t29..t32      — StructuredBuffer<DDGIProbeSH>[4]
    //   t33..t36      — Texture2D<float2>[4] (depth atlas)
    //   t37..t40      — StructuredBuffer<DDGIProbeData>[4]
    constexpr UINT kDDGIMaxVolumes      = 4; // mirror of DDGI::kMaxVolumes / DDGI_MAX_VOLUMES
    constexpr UINT kDDGIVolumeBufSlot   = 42; // t28 space0
    constexpr UINT kDDGIProbeSHSlot     = 43; // t29..t32 space0 (NumDescriptors=4)
    constexpr UINT kDDGIDepthSlot       = 44; // t33..t36 space0 (NumDescriptors=4)
    constexpr UINT kDDGIProbeDataSlot   = 45; // t37..t40 space0 (NumDescriptors=4)
    constexpr UINT totalParams = 1 + kCBVSlotCount + 2 + kSRVSlotCount + 1 + kSamplerSlotCount + kIBLSRVSlotCount + 1 + kClusterSRVSlotCount + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 1 + 2 + 2 + 1 + 4; // = 46
    D3D12_ROOT_PARAMETER params[totalParams] = {};

    // [0] Root constants — 4 uint32s at b0 space0
    //     [0] meshDescIdx  [1] instanceOffset  [2] materialIndex  [3] outlinePixelsBits (asfloat)
    params[kRootConstantsSlot].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstantsSlot].Constants.ShaderRegister = 0;
    params[kRootConstantsSlot].Constants.RegisterSpace  = 0;
    params[kRootConstantsSlot].Constants.Num32BitValues = 4;
    params[kRootConstantsSlot].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    // [1..7] Root CBVs — b1-b7 space0
    for (UINT i = 0; i < kCBVSlotCount; ++i)
    {
        params[kCBVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[kCBVSlotBase + i].Descriptor.ShaderRegister = i + 1; // b1..b7
        params[kCBVSlotBase + i].Descriptor.RegisterSpace  = 0;
        params[kCBVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [8] Root SRV — InstanceBuffer at t0 space0
    params[kInstanceBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kInstanceBufSlot].Descriptor.ShaderRegister = 0; // t0
    params[kInstanceBufSlot].Descriptor.RegisterSpace  = 0;
    params[kInstanceBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [9] Root SRV — MeshDescriptors at t1 space0
    params[kMeshDescSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kMeshDescSlot].Descriptor.ShaderRegister = 1; // t1
    params[kMeshDescSlot].Descriptor.RegisterSpace  = 0;
    params[kMeshDescSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [10..13] Descriptor tables — 1 SRV each at t2-t5 space0
    static D3D12_DESCRIPTOR_RANGE srvRanges[kSRVSlotCount] = {};
    for (UINT i = 0; i < kSRVSlotCount; ++i)
    {
        srvRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors                    = 1;
        srvRanges[i].BaseShaderRegister                = i + 2; // t2..t5
        srvRanges[i].RegisterSpace                     = 0;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &srvRanges[i];
        params[kSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [14] Descriptor table — 64 SRVs at t0 space1 (bindless g_Buffers[])
    static D3D12_DESCRIPTOR_RANGE bindlessRange = {};
    bindlessRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors                    = kMaxBindlessBuffers;
    bindlessRange.BaseShaderRegister                = 0; // t0
    bindlessRange.RegisterSpace                     = 1; // space1
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessSlot].DescriptorTable.pDescriptorRanges   = &bindlessRange;
    params[kBindlessSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [15..18] Descriptor tables — 1 sampler each at s0-s3
    static D3D12_DESCRIPTOR_RANGE samplerRanges[kSamplerSlotCount] = {};
    for (UINT i = 0; i < kSamplerSlotCount; ++i)
    {
        samplerRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRanges[i].NumDescriptors                    = 1;
        samplerRanges[i].BaseShaderRegister                = i;
        samplerRanges[i].RegisterSpace                     = 0;
        samplerRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kSamplerSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kSamplerSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kSamplerSlotBase + i].DescriptorTable.pDescriptorRanges   = &samplerRanges[i];
        params[kSamplerSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [19..20] Descriptor tables — 1 SRV each at t6-t7 space0 (IBL cubemaps)
    static D3D12_DESCRIPTOR_RANGE iblRanges[kIBLSRVSlotCount] = {};
    for (UINT i = 0; i < kIBLSRVSlotCount; ++i)
    {
        iblRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        iblRanges[i].NumDescriptors                    = 1;
        iblRanges[i].BaseShaderRegister                = i + kSRVSlotCount + 2; // t6, t7
        iblRanges[i].RegisterSpace                     = 0;
        iblRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kIBLSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kIBLSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kIBLSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &iblRanges[i];
        params[kIBLSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [22] Descriptor table — 3 SRVs at t9-t11 space0 (CSM shadow cascade maps)
    static D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = kShadowSRVCount;
    shadowRange.BaseShaderRegister                = kSRVSlotCount + 2 + kIBLSRVSlotCount; // t9
    shadowRange.RegisterSpace                     = 0;
    shadowRange.OffsetInDescriptorsFromTableStart = 0;

    params[kShadowSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kShadowSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kShadowSRVSlot].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[kShadowSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [23-25] Descriptor tables — 1 SRV each at t10-t12 space0 (clustered lighting)
    static D3D12_DESCRIPTOR_RANGE clusterRanges[kClusterSRVSlotCount] = {};
    for (UINT ci = 0; ci < kClusterSRVSlotCount; ++ci)
    {
        clusterRanges[ci].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        clusterRanges[ci].NumDescriptors                    = 1;
        clusterRanges[ci].BaseShaderRegister                = 13 + ci; // t13, t14, t15 (after shadow t9-t11)
        clusterRanges[ci].RegisterSpace                     = 0;
        clusterRanges[ci].OffsetInDescriptorsFromTableStart = 0;

        params[kClusterSRVSlotBase + ci].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kClusterSRVSlotBase + ci].DescriptorTable.NumDescriptorRanges = 1;
        params[kClusterSRVSlotBase + ci].DescriptorTable.pDescriptorRanges   = &clusterRanges[ci];
        params[kClusterSRVSlotBase + ci].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [26] Descriptor table — 1 SRV at t16 space0 (NPR ramp texture)
    static D3D12_DESCRIPTOR_RANGE rampRange = {};
    rampRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rampRange.NumDescriptors                    = 1;
    rampRange.BaseShaderRegister                = 16; // t16
    rampRange.RegisterSpace                     = 0;
    rampRange.OffsetInDescriptorsFromTableStart = 0;

    params[kRampTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRampTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kRampTexSlot].DescriptorTable.pDescriptorRanges   = &rampRange;
    params[kRampTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [28] Descriptor table — 1 SRV at t12 space0 (MaterialBuffer for per-material NPR params)
    static D3D12_DESCRIPTOR_RANGE materialBufRange = {};
    materialBufRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialBufRange.NumDescriptors                    = 1;
    materialBufRange.BaseShaderRegister                = 12; // t12
    materialBufRange.RegisterSpace                     = 0;
    materialBufRange.OffsetInDescriptorsFromTableStart = 0;

    params[kMaterialBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kMaterialBufSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kMaterialBufSlot].DescriptorTable.pDescriptorRanges   = &materialBufRange;
    params[kMaterialBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [27] Descriptor table — unbounded SRVs at t0 space2 (bindless texture array)
    static D3D12_DESCRIPTOR_RANGE bindlessTexRange = {};
    bindlessTexRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessTexRange.NumDescriptors                    = kMaxBindlessTextures;
    bindlessTexRange.BaseShaderRegister                = 0; // t0
    bindlessTexRange.RegisterSpace                     = 2; // space2
    bindlessTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessTexSlot].DescriptorTable.pDescriptorRanges   = &bindlessTexRange;
    params[kBindlessTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [29] Descriptor table — 1 SRV at t17 space0 (GBuffer emissive for deferred lighting)
    static D3D12_DESCRIPTOR_RANGE emissiveRange = {};
    emissiveRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    emissiveRange.NumDescriptors                    = 1;
    emissiveRange.BaseShaderRegister                = 17; // t17
    emissiveRange.RegisterSpace                     = 0;
    emissiveRange.OffsetInDescriptorsFromTableStart = 0;

    params[kEmissiveSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kEmissiveSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kEmissiveSRVSlot].DescriptorTable.pDescriptorRanges   = &emissiveRange;
    params[kEmissiveSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [30] Descriptor table — 1 SRV at t10 space0 (SSAO texture from XeGTAO)
    static D3D12_DESCRIPTOR_RANGE ssaoRange = {};
    ssaoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssaoRange.NumDescriptors                    = 1;
    ssaoRange.BaseShaderRegister                = 18; // t18
    ssaoRange.RegisterSpace                     = 0;
    ssaoRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSSAOSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSSAOSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSSAOSRVSlot].DescriptorTable.pDescriptorRanges   = &ssaoRange;
    params[kSSAOSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [31] Descriptor table — 1 SRV at t19 space0 (Sky SH coefficient buffer)
    static D3D12_DESCRIPTOR_RANGE skySHRange = {};
    skySHRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    skySHRange.NumDescriptors                    = 1;
    skySHRange.BaseShaderRegister                = 19; // t19
    skySHRange.RegisterSpace                     = 0;
    skySHRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSkySHSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSkySHSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSkySHSRVSlot].DescriptorTable.pDescriptorRanges   = &skySHRange;
    params[kSkySHSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [32] Descriptor table — 1 SRV at t20 space0 (Aerial Perspective 3D LUT)
    static D3D12_DESCRIPTOR_RANGE apRange = {};
    apRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    apRange.NumDescriptors                    = 1;
    apRange.BaseShaderRegister                = 20; // t20
    apRange.RegisterSpace                     = 0;
    apRange.OffsetInDescriptorsFromTableStart = 0;

    params[kAerialPerspSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kAerialPerspSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kAerialPerspSlot].DescriptorTable.pDescriptorRanges   = &apRange;
    params[kAerialPerspSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [33] Descriptor table — 1 SRV at t21 space0 (SpotShadowPass atlas).
    // Texture2DArray<float> D32; sampled with the existing shadow comparison
    // sampler (s2) for PCF inside the Lighting / Volumetric-fog shaders.
    static D3D12_DESCRIPTOR_RANGE spotShadowAtlasRange = {};
    spotShadowAtlasRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    spotShadowAtlasRange.NumDescriptors   = 1;
    spotShadowAtlasRange.BaseShaderRegister = 21; // t21
    spotShadowAtlasRange.RegisterSpace      = 0;
    spotShadowAtlasRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowAtlasSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowAtlasSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowAtlasSlot].DescriptorTable.pDescriptorRanges   = &spotShadowAtlasRange;
    params[kSpotShadowAtlasSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [34] Descriptor table — 1 SRV at t22 space0 (SpotShadowPass per-slice
    // viewProj buffer, StructuredBuffer<float4x4>). Indexed by GPULight::
    // shadowSliceIdx so the consumer shader can transform worldPos into the
    // light's shadow-map space and do a single SampleCmpLevelZero.
    static D3D12_DESCRIPTOR_RANGE spotShadowVPRange = {};
    spotShadowVPRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    spotShadowVPRange.NumDescriptors   = 1;
    spotShadowVPRange.BaseShaderRegister = 22; // t22
    spotShadowVPRange.RegisterSpace      = 0;
    spotShadowVPRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowVPSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowVPSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowVPSlot].DescriptorTable.pDescriptorRanges   = &spotShadowVPRange;
    params[kSpotShadowVPSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [35] Root CBV — per-material custom cbuffer (Phase E). Custom GBuffer
    // pixel shaders may declare `cbuffer Foo : register(b8, space0) {...}`;
    // Renderer packs MaterialComponent::customParams into an upload-heap ring
    // each frame and binds the GPU VA here. Standard shaders don't declare
    // b8 so leaving this slot unbound when not in use is a no-op.
    params[kCustomMatCBVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kCustomMatCBVSlot].Descriptor.ShaderRegister = 8; // b8
    params[kCustomMatCBVSlot].Descriptor.RegisterSpace  = 0;
    params[kCustomMatCBVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [36] Descriptor table — up to kCustomMatTexCount SRVs at t0-t3 space3
    // (Phase F). Custom pixel shaders may declare `Texture2D MyTex :
    // register(tN, space3)` and bind them by name; Renderer copies each
    // reflected texture's SRV into a per-material ring slot each frame.
    // Unused slots get a default white descriptor so sampling never reads
    // random heap memory.
    static D3D12_DESCRIPTOR_RANGE customMatTexRange = {};
    customMatTexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    customMatTexRange.NumDescriptors   = kCustomMatTexCount;
    customMatTexRange.BaseShaderRegister = 0;  // t0
    customMatTexRange.RegisterSpace      = 3;  // space3
    customMatTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kCustomMatTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kCustomMatTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kCustomMatTexSlot].DescriptorTable.pDescriptorRanges   = &customMatTexRange;
    params[kCustomMatTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [37] Descriptor table — 1 SRV at t23 space0 (reflection probe TextureCubeArray).
    // Renderer keeps this resource persistently allocated, so the binding is
    // always satisfied even with zero probes in the world.
    static D3D12_DESCRIPTOR_RANGE reflectionProbeArrayRange = {};
    reflectionProbeArrayRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeArrayRange.NumDescriptors   = 1;
    reflectionProbeArrayRange.BaseShaderRegister = 23; // t23
    reflectionProbeArrayRange.RegisterSpace      = 0;
    reflectionProbeArrayRange.OffsetInDescriptorsFromTableStart = 0;

    params[kReflectionProbeArraySlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeArraySlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeArraySlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeArrayRange;
    params[kReflectionProbeArraySlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [38] Descriptor table — 1 SRV at t24 space0 (StructuredBuffer<GPUReflectionProbe>).
    static D3D12_DESCRIPTOR_RANGE reflectionProbeBufferRange = {};
    reflectionProbeBufferRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeBufferRange.NumDescriptors   = 1;
    reflectionProbeBufferRange.BaseShaderRegister = 24; // t24
    reflectionProbeBufferRange.RegisterSpace      = 0;
    reflectionProbeBufferRange.OffsetInDescriptorsFromTableStart = 0;

    params[kReflectionProbeBufferSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeBufferSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeBufferSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeBufferRange;
    params[kReflectionProbeBufferSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [39] Descriptor table — 1 SRV at t25 space0 (StructuredBuffer<ProbeGridEntry>)
    static D3D12_DESCRIPTOR_RANGE reflectionProbeGridRange = {};
    reflectionProbeGridRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeGridRange.NumDescriptors   = 1;
    reflectionProbeGridRange.BaseShaderRegister = 25; // t25
    reflectionProbeGridRange.RegisterSpace      = 0;
    reflectionProbeGridRange.OffsetInDescriptorsFromTableStart = 0;
    params[kReflectionProbeGridSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeGridSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeGridSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeGridRange;
    params[kReflectionProbeGridSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [40] Descriptor table — 1 SRV at t26 space0 (StructuredBuffer<uint> probe index list)
    static D3D12_DESCRIPTOR_RANGE reflectionProbeIndexRange = {};
    reflectionProbeIndexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeIndexRange.NumDescriptors   = 1;
    reflectionProbeIndexRange.BaseShaderRegister = 26; // t26
    reflectionProbeIndexRange.RegisterSpace      = 0;
    reflectionProbeIndexRange.OffsetInDescriptorsFromTableStart = 0;
    params[kReflectionProbeIndexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeIndexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeIndexSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeIndexRange;
    params[kReflectionProbeIndexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [41] Descriptor table — 1 SRV at t27 space0 (SSR trace result, 1-frame
    // latent). Lighting.ps dampens its own specIBL by (1 - ssrConf) so the
    // composite step can additively write SSR contribution without double-
    // counting the specular term.
    static D3D12_DESCRIPTOR_RANGE ssrResultRange = {};
    ssrResultRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssrResultRange.NumDescriptors   = 1;
    ssrResultRange.BaseShaderRegister = 27; // t27
    ssrResultRange.RegisterSpace      = 0;
    ssrResultRange.OffsetInDescriptorsFromTableStart = 0;
    params[kSSRResultSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSSRResultSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSSRResultSlot].DescriptorTable.pDescriptorRanges   = &ssrResultRange;
    params[kSSRResultSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [42..45] DDGI integration — multi-volume. Each table-typed slot below holds
    // DDGI_MAX_VOLUMES contiguous SRVs; Lighting.ps iterates `vi < ddgiVolumeCount`
    // (uniform from LightCB) to consume only the active subset.
    static D3D12_DESCRIPTOR_RANGE ddgiVolBufRange{};
    ddgiVolBufRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiVolBufRange.NumDescriptors   = 1;
    ddgiVolBufRange.BaseShaderRegister = 28; // t28
    ddgiVolBufRange.RegisterSpace      = 0;
    params[kDDGIVolumeBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIVolumeBufSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIVolumeBufSlot].DescriptorTable.pDescriptorRanges   = &ddgiVolBufRange;
    params[kDDGIVolumeBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiProbeSHRange{};
    ddgiProbeSHRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiProbeSHRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiProbeSHRange.BaseShaderRegister = 29; // t29..t32
    ddgiProbeSHRange.RegisterSpace      = 0;
    params[kDDGIProbeSHSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIProbeSHSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIProbeSHSlot].DescriptorTable.pDescriptorRanges   = &ddgiProbeSHRange;
    params[kDDGIProbeSHSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiDepthRange{};
    ddgiDepthRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiDepthRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiDepthRange.BaseShaderRegister = 33; // t33..t36
    ddgiDepthRange.RegisterSpace      = 0;
    params[kDDGIDepthSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIDepthSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIDepthSlot].DescriptorTable.pDescriptorRanges   = &ddgiDepthRange;
    params[kDDGIDepthSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiProbeDataRange{};
    ddgiProbeDataRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiProbeDataRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiProbeDataRange.BaseShaderRegister = 37; // t37..t40
    ddgiProbeDataRange.RegisterSpace      = 0;
    params[kDDGIProbeDataSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIProbeDataSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIProbeDataSlot].DescriptorTable.pDescriptorRanges   = &ddgiProbeDataRange;
    params[kDDGIProbeDataSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters     = totalParams;
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE; // No IA layout — PVF

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        const char* msg = errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "";
        LOG_ERROR("CreateDefaultRootSignature failed: %s", msg);
        ThrowIfFailed(hr);
    }

    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> GraphicsDX12::CreateComputeRootSignature()
{
    // [0] ROOT_CBV       b0 space2   — SkinJobDesc / generic compute CB
    // [1] DESC_TABLE  1x SRV t0 space2 — rest positions
    // [2] DESC_TABLE  1x SRV t1 space2 — rest normals
    // [3] DESC_TABLE  1x SRV t2 space2 — blend data
    // [4] DESC_TABLE  1x UAV u0 space2 — output positions
    // [5] DESC_TABLE  1x UAV u1 space2 — output normals
    // [6] DESC_TABLE  1x SRV t5 space2 — morph weights (ByteAddressBuffer)
    // [7] DESC_TABLE  1x SRV t3 space2 — pose matrices
    // [8] DESC_TABLE  1x SRV t4 space2 — morph target deltas
    //   NEW — only used by SceneVoxelize.cs, harmlessly ignored by others:
    // [9]  DESC_TABLE 1 SRV t0 space0 — StructuredBuffer<GPUInstanceData>
    // [10] DESC_TABLE 1 SRV t1 space0 — StructuredBuffer<MeshDescriptor>
    // [11] DESC_TABLE N SRV t0 space1 — bindless ByteAddressBuffer g_Buffers[]
    //   NEW — used by FroxelLightInject.cs to sample SpotShadowPass:
    // [12] DESC_TABLE 1 SRV t6 space2 — Texture2DArray<float> spot shadow atlas
    // [13] DESC_TABLE 1 SRV t7 space2 — StructuredBuffer<float4x4> per-slice VPs
    //   NEW — used by XeGTAO depth prefilter (5-mip pyramid in one dispatch):
    // [14] DESC_TABLE 1 UAV u2 space2
    // [15] DESC_TABLE 1 UAV u3 space2
    // [16] DESC_TABLE 1 UAV u4 space2
    // static samplers (s0/s1 space2) stay the same; s1 is the comparison
    // sampler and gets reused for atlas PCF taps.

    // [14] DESC_TABLE 1 UAV u2 space2 ─┐
    // [15] DESC_TABLE 1 UAV u3 space2  ├─ extra UAV slots for XeGTAO prefilter,
    // [16] DESC_TABLE 1 UAV u4 space2 ─┘  which writes 5 mip levels in one
    //      dispatch (u0..u4, matching Intel's XeGTAO_PrefilterDepths16x16).
    //      Legal to leave these unbound for other compute shaders — they
    //      simply don't reference the registers.
    // [17] DESC_TABLE N SRV t0 space3 — bindless Texture2D[] (same heap region
    //      as graphics slot kBindlessTexSlot). Used by DecalApply.cs to sample
    //      decal textures through a material's bindlessIndex. Unbound leaves
    //      it undefined, harmless when the shader never references t*space3.
    CD3DX12_ROOT_PARAMETER1 params[18];
    params[0].InitAsConstantBufferView(0, 2, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                                       D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 srvRanges[5];
    srvRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2);
    srvRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 2);
    srvRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 2);
    srvRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 2);
    srvRanges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 2); // morph deltas
    CD3DX12_DESCRIPTOR_RANGE1 srvRange5;
    srvRange5.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5, 2);  // morph weights
    params[1].InitAsDescriptorTable(1, &srvRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[2].InitAsDescriptorTable(1, &srvRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[3].InitAsDescriptorTable(1, &srvRanges[2], D3D12_SHADER_VISIBILITY_ALL);
    params[7].InitAsDescriptorTable(1, &srvRanges[3], D3D12_SHADER_VISIBILITY_ALL);
    params[8].InitAsDescriptorTable(1, &srvRanges[4], D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 uavRanges[2];
    uavRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2);
    uavRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 2);
    params[4].InitAsDescriptorTable(1, &uavRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[5].InitAsDescriptorTable(1, &uavRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[6].InitAsDescriptorTable(1, &srvRange5,    D3D12_SHADER_VISIBILITY_ALL);

    // [9], [10] — mirror of graphics [8], [9] so compute can do the same PVF
    // fetch. Filled only when SceneVoxelize runs; other compute shaders just
    // don't touch these root params (legal: the runtime leaves the descriptor
    // table pointer undefined if the shader never references the registers).
    // We use descriptor tables (not root SRVs) because the existing RHI
    // compute helper surface exposes SetComputeDescriptorTable(handle) and we
    // get those GPU handles straight from GetBufferSRVGpuHandle().
    static CD3DX12_DESCRIPTOR_RANGE1 instanceRange;
    static CD3DX12_DESCRIPTOR_RANGE1 meshDescRange;
    instanceRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); // t0 space0
    meshDescRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0); // t1 space0
    params[9 ].InitAsDescriptorTable(1, &instanceRange, D3D12_SHADER_VISIBILITY_ALL);
    params[10].InitAsDescriptorTable(1, &meshDescRange, D3D12_SHADER_VISIBILITY_ALL);

    // [11] — bindless vertex/index buffer array, identical layout to the
    // graphics root sig so pvf_fetch.hlsli "just works" under compute.
    CD3DX12_DESCRIPTOR_RANGE1 bindlessRange;
    bindlessRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                       kMaxBindlessBuffers, 0, 1,
                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[11].InitAsDescriptorTable(1, &bindlessRange, D3D12_SHADER_VISIBILITY_ALL);

    // [12] — SpotShadowPass atlas (Texture2DArray<float>) at t6 space2.
    // [13] — per-slice viewProj StructuredBuffer at t7 space2.
    // These let FroxelLightInject sample per-light shadow maps so spot
    // beams that get blocked by walls don't light the fog behind them.
    static CD3DX12_DESCRIPTOR_RANGE1 spotShadowAtlasRangeC;
    static CD3DX12_DESCRIPTOR_RANGE1 spotShadowVPRangeC;
    spotShadowAtlasRangeC.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 2); // t6 space2
    spotShadowVPRangeC   .Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7, 2); // t7 space2
    params[12].InitAsDescriptorTable(1, &spotShadowAtlasRangeC, D3D12_SHADER_VISIBILITY_ALL);
    params[13].InitAsDescriptorTable(1, &spotShadowVPRangeC,    D3D12_SHADER_VISIBILITY_ALL);

    // [14..16] — u2 / u3 / u4 space2 (extra UAV slots for XeGTAO prefilter).
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU2;
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU3;
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU4;
    uavRangeU2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 2);  // u2 space2
    uavRangeU3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 2);  // u3 space2
    uavRangeU4.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 2);  // u4 space2
    params[14].InitAsDescriptorTable(1, &uavRangeU2, D3D12_SHADER_VISIBILITY_ALL);
    params[15].InitAsDescriptorTable(1, &uavRangeU3, D3D12_SHADER_VISIBILITY_ALL);
    params[16].InitAsDescriptorTable(1, &uavRangeU4, D3D12_SHADER_VISIBILITY_ALL);

    // [17] — bindless Texture2D[] at t0 space3. The GPU handle passed at
    // bind time is the same heap-region base as graphics kBindlessTexSlot;
    // the shader just sees them at a different register/space so compute
    // and graphics shaders don't need identical HLSL layouts. DESCRIPTORS_
    // VOLATILE matches the graphics side — new texture allocations may mutate
    // the table between recordings.
    static CD3DX12_DESCRIPTOR_RANGE1 bindlessTex2DRange;
    bindlessTex2DRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                            kMaxBindlessTextures, 0, 3,
                            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[17].InitAsDescriptorTable(1, &bindlessTex2DRange, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2]{};

    // s0 space2 — plain linear-clamp (for Sample / SampleLevel).
    samplers[0].Init(0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 16, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z (ignored: non-comparison filter)
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[0].RegisterSpace = 2;

    // s1 space2 — comparison sampler (PCF) for CSM shadow sampling in the
    // volumetric-fog light-inject compute shader. Without this the shader's
    // SampleCmpLevelZero call was paired with a non-comparison sampler and
    // silently returned garbage → no visible god rays.
    samplers[1].Init(1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 0, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z: lit when receiver z >= SM z
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[1].RegisterSpace = 2;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(18, params, 2, samplers,
                    D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc,
                     D3D_ROOT_SIGNATURE_VERSION_1_1,
                     &serialized, &error);
    if (FAILED(hr))
    {
        if (error) LOG_ERROR("CreateComputeRootSignature: %s",
                             static_cast<char*>(error->GetBufferPointer()));
        return nullptr;
    }

    ComPtr<ID3D12RootSignature> rs;
    hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                       serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&rs));
    if (FAILED(hr)) { LOG_ERROR("CreateComputeRootSignature: CreateRootSignature failed"); return nullptr; }
    return rs;
}

// ===========================================================================
// Construction / destruction
// ===========================================================================

GraphicsDX12::GraphicsDX12(HWND hWnd, UINT width, UINT height)
    : m_hWnd(hWnd), m_width(width), m_height(height)
{
    LOG_INFO("GraphicsDX12 constructor");
    LoadPipeline(hWnd, width, height);
    LoadAssets();
    // ImGui init lives in the editor (EditorImGuiBridge::Attach) — engine is
    // UI-agnostic. Game builds never touch ImGui at all.
    m_gpuProfiler.Init(m_device.Get(), m_commandQueue.Get());

    // Pre-reserve the command-list pointer table so that concurrent calls to
    // BeginCommandList (from render workers) never trigger a vector reallocation
    // while another thread is reading m_commandLists[id] in GetPoolEntry.
    // push_back only appends a pointer; existing entries are never relocated.
    // 64 slots is far more than any single frame will use.
    m_commandLists.reserve(64);
}

GraphicsDX12::~GraphicsDX12()
{
    // Ensure GPU finished before releasing D3D12 objects.
    if (m_device && m_fenceEvent)
    {
        try { WaitForPreviousFrame(); } catch (...) {}
    }

    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }

    // Release all resource pools + command-list pool *before* live-object reporting.
    // Otherwise ReportLiveDeviceObjects() will list objects that are only still
    // alive until member-destructors run at the end of this destructor body.

    // HDR render target + its descriptor allocations.
    m_hdrRtvAllocation.Free();
    m_hdrSrvAllocation.Free();
    m_hdrUavAllocation.Free();
    m_hdrRenderTarget.Reset();

    // Explicitly destroy command-list pool entries constructed via placement-new.
    // BlockAllocator itself does not call T destructors automatically.
    for (CommandList_DX12* p : m_commandLists)
    {
        if (p) p->~CommandList_DX12();
    }
    m_commandLists.clear();
    m_commandListCount = 0;
    m_cmdAllocator.blocks.clear();
    m_cmdAllocator.freeList.clear();

    // Release CPU-side pools that hold ComPtr resources/pso/root signatures.
    m_bufferPool.clear();
    m_texturePool.clear();
    m_shaderPool.clear();
    m_psoPool.clear();
    m_samplerPool.clear();
    m_bufferFreeList.clear();
    m_textureFreeList.clear();

    // Release descriptor heap allocators (heaps are inside DescriptorHeapAllocator).
    m_rtvAllocator = {};
    m_cbvSrvUavAllocator = {};
    m_dsvAllocator = {};
    m_samplerAllocator = {};

    // Release upload-only staging objects and synchronization primitives.
    m_uploadAllocator.Reset();
    m_commandList.Reset();
    m_fence.Reset();
    for (auto& f : m_queueFences) f.Reset();
    m_scRTVHeap.Reset();

    // Pipeline library bytes & ComPtr.
    m_psoLibraryData.clear();
    m_psoLibrary.Reset();

    // Enforce correct DXGI release order in the destructor body so that
    // member-destructor ordering (reverse-declaration) doesn't matter:
    //   render targets → swap chain (after SetFullscreenState) → queues
    // This prevents the DXGI "process terminating" live-object warning.
    for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
    if (m_swapChain)
    {
        m_swapChain->SetFullscreenState(FALSE, nullptr);
        m_swapChain.Reset();
    }
    for (auto& q : m_queues) q.Reset();

#ifdef _DEBUG
    // Report live objects while the message callback is still registered so
    // output reaches the log.  Use IGNORE_INTERNAL to filter debug-layer
    // bookkeeping objects and see only application-level leaks.
    if (m_device)
    {
        ComPtr<ID3D12DebugDevice1> debugDevice1;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice1))) && debugDevice1)
        {
            debugDevice1->ReportLiveDeviceObjects(
                D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
        else
        {
            ComPtr<ID3D12DebugDevice> debugDevice;
            if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice))) && debugDevice)
                debugDevice->ReportLiveDeviceObjects(
                    D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
    }

    if (m_d3d12MessageCallbackCookie != 0 && m_device)
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
            infoQueue->UnregisterMessageCallback(m_d3d12MessageCallbackCookie);
        m_d3d12MessageCallbackCookie = 0;
    }
#endif
    // Member ComPtr destructors release the rest (device, factory, heaps, pools).
}

// ===========================================================================
// DX12 translation helper — queue type
// ===========================================================================

D3D12_COMMAND_LIST_TYPE GraphicsDX12::ToD3D12CommandListType(RHI::QUEUE_TYPE q)
{
    switch (q)
    {
    case RHI::QUEUE_TYPE::COMPUTE: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case RHI::QUEUE_TYPE::COPY:    return D3D12_COMMAND_LIST_TYPE_COPY;
    default:                       return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
}

// ===========================================================================
// Pool helpers
// ===========================================================================

CommandList_DX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd)
{
    assert(cmd.internal_id < m_commandListCount && "Invalid CommandList handle");
    return *m_commandLists[cmd.internal_id];
}

const CommandList_DX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd) const
{
    assert(cmd.internal_id < m_commandListCount && "Invalid CommandList handle");
    return *m_commandLists[cmd.internal_id];
}

void GraphicsDX12::AddCommandListDependency(RHI::CommandList waiter,
                                             RHI::CommandList dependency)
{
    assert(waiter.internal_id     < m_commandListCount && "waiter handle invalid");
    assert(dependency.internal_id < m_commandListCount && "dependency handle invalid");
    assert(waiter.internal_id != dependency.internal_id && "self-dependency");
    m_commandLists[waiter.internal_id]->wait_for.push_back(dependency.internal_id);
}

// ===========================================================================
// DX12-specific accessors
// ===========================================================================

ID3D12GraphicsCommandList* GraphicsDX12::GetNativeCommandList(RHI::CommandList cmd) const
{
    return GetPoolEntry(cmd).GetCommandList();
}
ID3D12Device*              GraphicsDX12::GetDevice()            const { return m_device.Get(); }

ID3D12Device5* GraphicsDX12::GetDevice5() const
{
    if (!m_dxrSupported) return nullptr;
    // QI on demand — the cost is one COM ref bump and is amortised by the
    // fact that DDGI calls this once at Compile() to cache the pointer.
    ID3D12Device5* dev5 = nullptr;
    if (FAILED(m_device.Get()->QueryInterface(IID_PPV_ARGS(&dev5))))
        return nullptr;
    dev5->Release(); // caller does not own ref; pointer lifetime tied to m_device
    return dev5;
}
DescriptorHeapAllocator&   GraphicsDX12::GetRtvAllocator()            { return m_rtvAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetCbvSrvUavAllocator()      { return m_cbvSrvUavAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetDsvAllocator()             { return m_dsvAllocator; }

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureDsvCpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid() || tex.handle_id >= static_cast<uint32_t>(m_texturePool.size()))
        return D3D12_CPU_DESCRIPTOR_HANDLE{};
    return m_texturePool[tex.handle_id].dsv.GetCpuHandle();
}

ID3D12Resource* GraphicsDX12::GetBufferResource(const RHI::GPUBuffer& buffer) const
{
    if (!buffer.IsValid() || buffer.handle_id >= m_bufferPool.size()) return nullptr;
    return m_bufferPool[buffer.handle_id].resource.Get();
}

// ---------------------------------------------------------------------------
uint64_t GraphicsDX12::CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                                   uint32_t            count)
{
    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(count);
    if (!alloc.IsValid())
    {
        LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: descriptor allocation failed");
        return 0;
    }

    const UINT descSize = m_cbvSrvUavAllocator.GetDescriptorSize();

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!textures[i].IsValid()) continue;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = alloc.GetCpuHandle();
        cpuSlot.ptr += static_cast<SIZE_T>(i) * descSize;

        ID3D12Resource* resource = GetTextureResource(textures[i]);
        if (resource)
            m_device->CreateShaderResourceView(resource, &srvDesc, cpuSlot);
        else
            LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: no resource for slot %u", i);
    }

    alloc.CopyToGpu();
    const uint64_t gpuHandle = alloc.GetGpuHandle().ptr;
    m_descriptorTablePool[gpuHandle] = std::move(alloc);
    LOG_INFO("GraphicsDX12::CreateDepthTextureSRVTable: handle = %llu (%u entries)", gpuHandle, count);
    return gpuHandle;
}

// ---------------------------------------------------------------------------
void GraphicsDX12::FreeDescriptorTable(uint64_t gpuHandle)
{
    if (!gpuHandle) return;
    auto it = m_descriptorTablePool.find(gpuHandle);
    if (it != m_descriptorTablePool.end())
    {
        // Defer: GPU may still reference this table via in-flight draws.
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        m_deferredRelease[m_frameIndex].descriptors.push_back(std::move(it->second));
        m_descriptorTablePool.erase(it);
    }
}

ID3D12Resource* GraphicsDX12::GetTextureResource(const RHI::Texture& texture) const
{
    if (!texture.IsValid() || texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return nullptr;
    return m_texturePool[texture.handle_id].resource.Get();
}

// ---------------------------------------------------------------------------
// Descriptor-table fill helpers (multi-volume DDGI). See header for usage.
// ---------------------------------------------------------------------------
void GraphicsDX12::CreateStructuredBufferSRVAtCpu(const RHI::GPUBuffer& buf,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetBufferResource(buf);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN; // structured
    const uint32_t stride           = buf.desc.stride > 0 ? buf.desc.stride : 4;
    srvd.Buffer.NumElements         = static_cast<UINT>(buf.desc.size / stride);
    srvd.Buffer.StructureByteStride = stride;
    m_device->CreateShaderResourceView(res, &srvd, dst);
}

void GraphicsDX12::CreateTexture2DSRVAtCpu(const RHI::Texture& tex,
                                           D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetTextureResource(tex);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = ToDxgiFormat(tex.desc.format);
    srvd.Texture2D.MipLevels        = tex.desc.mip_levels > 0 ? tex.desc.mip_levels : 1;
    srvd.Texture2D.MostDetailedMip  = 0;
    m_device->CreateShaderResourceView(res, &srvd, dst);
}

void GraphicsDX12::CreateNullStructuredBufferSRVAtCpu(uint32_t stride,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN;
    srvd.Buffer.NumElements         = 1;
    srvd.Buffer.StructureByteStride = stride > 0 ? stride : 4;
    m_device->CreateShaderResourceView(nullptr, &srvd, dst);
}

void GraphicsDX12::CreateNullTexture2DSRVAtCpu(DXGI_FORMAT format,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = format;
    srvd.Texture2D.MipLevels        = 1;
    m_device->CreateShaderResourceView(nullptr, &srvd, dst);
}

// ===========================================================================
// Pipeline setup
// ===========================================================================

void GraphicsDX12::LoadPipeline(HWND hWnd, UINT width, UINT height)
{
    LOG_INFO("LoadPipeline begin");

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            LOG_INFO("D3D12 debug layer enabled — messages will be forwarded to Logger");
        }
    }
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; DXGI_ERROR_NOT_FOUND != m_factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)); ++i)
    {
        DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        LOG_INFO("Trying adapter: %ls", desc.Description);
        bool ok = false;
        for (auto fl : featureLevels)
            if (SUCCEEDED(D3D12CreateDevice(nullptr, fl, IID_PPV_ARGS(&m_device)))) { ok = true; break; }
        if (ok) break;
    }

    // Create one command queue per queue type (GRAPHICS / COMPUTE / COPY)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[0])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[1])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[2])));
    }

#ifdef _DEBUG
    // Forward D3D12 debug layer messages to the app's Log system
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            struct D3D12LogCallback
            {
                static void __stdcall Callback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                    D3D12_MESSAGE_ID, LPCSTR pDescription, void*)
                {
                    LogLevel level = LogLevel::Info;
                    switch (severity)
                    {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                    case D3D12_MESSAGE_SEVERITY_ERROR:   level = LogLevel::Error;   break;
                    case D3D12_MESSAGE_SEVERITY_WARNING: level = LogLevel::Warning; break;
                    default:                             level = LogLevel::Info;    break;
                    }
                    Logger::Get().Log(level, "D3D12", 0, "%s", pDescription ? pDescription : "");
                }
            };
            if (SUCCEEDED(infoQueue->RegisterMessageCallback(
                    &D3D12LogCallback::Callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &m_d3d12MessageCallbackCookie)))
                LOG_INFO("D3D12 debug messages forwarded to Logger");
        }
    }
#endif

    // Check tearing (variable refresh rate) support.
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                          &allowTearing, sizeof(allowTearing));
        m_tearingSupported = (allowTearing == TRUE);
        LOG_INFO("GraphicsDX12: tearing support = %s", m_tearingSupported ? "YES" : "NO");
    }

    // Mesh-shader capability (Options7). Tier_1 is the only non-NotSupported
    // value today. Terrain / any future MS pass checks SupportsMeshShader()
    // and falls back gracefully when the runtime / GPU lacks support.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                    &opts7, sizeof(opts7))))
        {
            m_meshShaderSupported = (opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
        }
        LOG_INFO("GraphicsDX12: mesh shader support = %s",
                 m_meshShaderSupported ? "YES (Tier_1+)" : "NO");
    }

    // DXR raytracing capability (Options5). DDGI probe trace requires Tier_1_0.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                    &opts5, sizeof(opts5))))
        {
            m_dxrSupported = (opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
        }
        LOG_INFO("GraphicsDX12: DXR support = %s",
                 m_dxrSupported ? "YES (Tier_1_0+)" : "NO");
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount = FrameCount;
    scd.Width = width; scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    if (m_tearingSupported)
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hWnd, &scd, nullptr, nullptr, &sc));
    ThrowIfFailed(m_factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(sc.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Swap-chain RTV heap (not managed by allocator)
    D3D12_DESCRIPTOR_HEAP_DESC rhd = {};
    rhd.NumDescriptors = FrameCount;
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&m_scRTVHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Managed descriptor heap allocators
    // RTV / DSV: CPU-only (gpuStatic=0, gpuDynamic=0)
    m_rtvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256,  0, 0);
    m_dsvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,  64,  0, 0);
    // CBV_SRV_UAV: 4096 CPU staging, 65536 GPU static, 934464 GPU dynamic  (total 1 000 000 GPU)
    m_cbvSrvUavAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, 65536, 934464);
    // Sampler: 256 CPU staging, 2048 GPU static, 0 dynamic
    m_samplerAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, MaxSamplers, 0);

    // Allocate bindless texture table (contiguous SRV block for all textures).
    m_bindlessTexTable = m_cbvSrvUavAllocator.Allocate(kMaxBindlessTextures);
    if (m_bindlessTexTable.IsValid())
    {
        // Fill with null SRVs (Texture2D, format=UNKNOWN) to avoid GPU faults on unbound slots.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels     = 1;
        UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH = m_bindlessTexTable.GetGpuCpuHandle();
        for (uint32_t i = 0; i < kMaxBindlessTextures; ++i)
        {
            m_device->CreateShaderResourceView(nullptr, &nullSrv, cpuH);
            cpuH.ptr += descInc;
        }
        LOG_SUCCESS("GraphicsDX12: bindless texture table allocated (%u slots)", kMaxBindlessTextures);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
        m_renderTargets[n]->SetName(bbName.c_str());
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvH);
        rtvH.ptr += m_rtvDescriptorSize;
    }

    // Dedicated upload allocator (used by CreateBuffer / CreateTexture staging)
    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_uploadAllocator)));

    CreateHdrRenderTarget(width, height);
    LOG_SUCCESS("LoadPipeline finished");
}

void GraphicsDX12::LoadAssets()
{
    // Upload command list — stays open so CreateBuffer/CreateTexture can record into it.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    // Created open; leave it open for immediate staging use.

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    // Per-queue GPU-GPU cross-queue synchronization fences
    for (UINT q = 0; q < 3; ++q)
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_queueFences[q])));
        m_queueFenceValues[q] = 0;
    }

    m_viewport    = { 0,0, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(m_width),  static_cast<LONG>(m_height) };

    // Build the shared root signature once — reused by every CreatePipelineState call.
    m_defaultRootSignature = CreateDefaultRootSignature();

    m_computeRootSignature = CreateComputeRootSignature();
    if (!m_computeRootSignature)
    { LOG_ERROR("GraphicsDX12: failed to create compute root signature"); }
    else
    { LOG_SUCCESS("GraphicsDX12: compute root signature ready"); }

    // ---- Command Signature for ExecuteIndirect (root constants + DrawInstanced) ---
    {
        D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
        // Arg 0: 4 root constants at param 0
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant.RootParameterIndex = kRootConstantsSlot;
        args[0].Constant.DestOffsetIn32BitValues = 0;
        args[0].Constant.Num32BitValuesToSet = 4;
        // Arg 1: DrawInstanced
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

        D3D12_COMMAND_SIGNATURE_DESC csDesc = {};
        csDesc.ByteStride = 32; // sizeof(IndirectDrawCommand) = 4*4 root const + 4*4 draw args = 32
        csDesc.NumArgumentDescs = 2;
        csDesc.pArgumentDescs = args;

        HRESULT hr = m_device->CreateCommandSignature(
            &csDesc, m_defaultRootSignature.Get(),
            IID_PPV_ARGS(&m_indirectCommandSignature));
        if (FAILED(hr))
            LOG_ERROR("GraphicsDX12: CreateCommandSignature failed (hr=0x%08X)", static_cast<unsigned>(hr));
        else
            LOG_SUCCESS("GraphicsDX12: indirect command signature ready (stride=%u)", csDesc.ByteStride);
    }

    // Pre-reserve resource pools so that push_back never reallocates while another
    // thread is holding a reference obtained from an earlier Create* call.
    // m_psoPool is a std::deque (stable refs on push_back regardless of size) —
    // see GraphicsDX12.h for rationale. No reserve call on deque.
    m_bufferPool.reserve(4096);
    m_texturePool.reserve(2048);
    m_shaderPool.reserve(512);
}

void GraphicsDX12::WaitForPreviousFrame()
{
    // Sync GRAPHICS queue (CPU-GPU frame pacing fence).
    // Pre-increment so the signaled value is strictly > any previously signaled one —
    // matches EndFrame's pre-increment to keep m_fence values monotonically unique.
    const UINT64 fence = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fence));
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // Sync COMPUTE and COPY queues via their cross-queue fences (reuse m_fenceEvent)
    for (UINT q = 1; q <= 2; ++q)
    {
        if (m_queueFenceValues[q] == 0) continue;
        if (m_queueFences[q]->GetCompletedValue() < m_queueFenceValues[q])
        {
            ThrowIfFailed(m_queueFences[q]->SetEventOnCompletion(
                m_queueFenceValues[q], m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    // GPU is fully idle — drain every deferred-release slot (not just the
    // current backbuffer's), so Resize / destructors can safely Release resources.
    ProcessAllDeferredReleases();

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
void GraphicsDX12::ProcessDeferredReleases(uint32_t idx)
{
    DeferredReleaseList local;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        if (idx >= FrameCount) return;
        local = std::move(m_deferredRelease[idx]);
        m_deferredRelease[idx] = {};
    }
    // Free descriptors (returns slots to heap free lists for reuse next frame).
    for (auto& d : local.descriptors) d.Free();
    // ComPtr destructors release resources when 'local' goes out of scope.
    // Return pool slots to their free lists.
    std::lock_guard<std::mutex> lk(m_resourceMutex);
    for (uint32_t s : local.bufferSlots)  m_bufferFreeList.push_back(s);
    for (uint32_t s : local.textureSlots) m_textureFreeList.push_back(s);
}

void GraphicsDX12::ProcessAllDeferredReleases()
{
    for (uint32_t i = 0; i < FrameCount; ++i)
        ProcessDeferredReleases(i);
}

void GraphicsDX12::FlushAndWait()
{
    // Signal on the graphics queue and CPU-wait until the GPU has processed
    // all previously submitted work.  Only signals the graphics queue fence
    // (picking only uses the graphics queue).
    // Pre-increment so the signaled value is strictly > any previously signaled
    // one; otherwise it can collide with EndFrame's signal and the wait is a no-op.
    const UINT64 fence = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fence));
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void GraphicsDX12::BeginBufferUploadBatch()
{
    // Nested scopes are flattened into a counter — only the outermost pair
    // triggers a flush. The upload command list is already "open" for
    // recording (Reset at the end of LoadAssets / FlushUploadAndWait), so
    // buffers created inside the scope simply accumulate copies on it.
    ++m_batchUploadDepth;
}

void GraphicsDX12::EndBufferUploadBatch()
{
    if (m_batchUploadDepth == 0)
    {
        LOG_ERROR("EndBufferUploadBatch called without matching Begin");
        return;
    }
    --m_batchUploadDepth;
    if (m_batchUploadDepth == 0)
    {
        // Final flush for any staging still queued. Mid-batch auto-flushes
        // may have already trimmed this down, but there's usually a last
        // unflushed tail smaller than kBatchFlushBytes.
        FlushUploadAndWait();
        m_batchStagingKeepAlive.clear();
        m_batchStagingBytes = 0;
    }
}

void GraphicsDX12::FlushBatchStagingIfNeeded()
{
    if (m_batchStagingBytes    < kBatchFlushBytes &&
        m_batchStagingKeepAlive.size() < kBatchFlushCount)
        return;

    // Mid-batch flush: execute the accumulated copies, wait for GPU to
    // complete (so staging buffers are safe to release), then reset. Scope
    // stays open — m_batchUploadDepth is unchanged and subsequent
    // CreateBuffer calls keep deferring per-buffer waits.
    FlushUploadAndWait();
    m_batchStagingKeepAlive.clear();
    m_batchStagingBytes = 0;
}

void GraphicsDX12::FlushUploadAndWait()
{
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    // Use FlushAndWait (not WaitForPreviousFrame) so we don't mutate m_frameIndex
    // mid-frame when this is called from TextureSystem::Tick after BeginFrame.
    FlushAndWait();
    // Re-open for subsequent staging commands
    ThrowIfFailed(m_uploadAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_uploadAllocator.Get(), nullptr));
}

// ===========================================================================
// HDR render target
// ===========================================================================

void GraphicsDX12::CreateHdrRenderTarget(UINT width, UINT height)
{
    if (width == 0 || height == 0) return;
    ReleaseHdrRenderTarget();

    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC   td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = width;  td.Height = height;
    td.DepthOrArraySize   = 1;      td.MipLevels = 1;
    td.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count   = 1;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                          | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // SSR composite writes
    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_R16G16B16A16_FLOAT, {0,0,0,0} };
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_hdrRenderTarget)));
    m_hdrRenderTarget->SetName(L"GraphicsDX12.HdrEditorViewport");

    m_hdrRtvAllocation = m_rtvAllocator.Allocate(1);
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{ DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2D };
    m_device->CreateRenderTargetView(m_hdrRenderTarget.Get(), &rtvd, m_hdrRtvAllocation.GetCpuHandle());

    m_hdrSrvAllocation = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_hdrRenderTarget.Get(), &srvd, m_hdrSrvAllocation.GetCpuHandle());
    m_hdrSrvAllocation.CopyToGpu();

    m_hdrUavAllocation = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_hdrRenderTarget.Get(), nullptr, &uavd,
        m_hdrUavAllocation.GetCpuHandle());
    m_hdrUavAllocation.CopyToGpu();

    m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_hdrWidth = width; m_hdrHeight = height;
}

void GraphicsDX12::ReleaseHdrRenderTarget()
{
    // Defer: GPU may still be reading m_hdrRenderTarget / its descriptors
    // from frames still in flight. Freeing now would let the descriptor slots
    // be reused and the resource destroyed while GPU work is pending.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& dr = m_deferredRelease[m_frameIndex];
    if (m_hdrRtvAllocation.IsValid()) { dr.descriptors.push_back(m_hdrRtvAllocation); m_hdrRtvAllocation = {}; }
    if (m_hdrSrvAllocation.IsValid()) { dr.descriptors.push_back(m_hdrSrvAllocation); m_hdrSrvAllocation = {}; }
    if (m_hdrUavAllocation.IsValid()) { dr.descriptors.push_back(m_hdrUavAllocation); m_hdrUavAllocation = {}; }
    if (m_hdrRenderTarget)            { dr.resources.push_back(std::move(m_hdrRenderTarget)); }
}

// ===========================================================================
// Frame lifecycle
// ===========================================================================

RHI::CommandList GraphicsDX12::BeginFrame()
{
    // Full drain — wait for ALL previous GPU work to finish before the new frame.
    // Pipelined wait (only waiting on the fence from FrameCount frames ago) was
    // suspected of causing a startup flicker; reverted to the safe semantics so
    // every call-site that destroys resources (TextureSystem::Tick, Destroy*,
    // SetViewportSize→ReleaseHdrRenderTarget) gets the old "previous frame is
    // complete after BeginFrame" guarantee.
    {
        const uint64_t fence = ++m_fenceValue;
        ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fence));
        if (m_fence->GetCompletedValue() < fence)
        {
            ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
    for (UINT q = 1; q <= 2; ++q)
    {
        if (m_queueFenceValues[q] == 0) continue;
        if (m_queueFences[q]->GetCompletedValue() < m_queueFenceValues[q])
        {
            ThrowIfFailed(m_queueFences[q]->SetEventOnCompletion(
                m_queueFenceValues[q], m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // GPU is fully idle — drain EVERY deferred-release slot (not just the
    // current backbuffer's). Keeps the deferred-destruction infrastructure in
    // place so Destroy{Buffer,Texture} / ReleaseHdrRenderTarget callers don't
    // need to change, but matches old immediate-release semantics in effect.
    ProcessAllDeferredReleases();

    // Read back previous frame's GPU timestamps (GPU has finished for this slot).
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.BeginFrame();

    // Reset the pool — this slot's entries are no longer in-flight.
    m_commandListCount = 0;

    // Open the primary command list (pool entry 0) for the GRAPHICS queue.
    RHI::CommandList primary = BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    // primary.internal_id == 0 guaranteed (first allocation after reset).

    auto* cmd = GetPoolEntry(primary).GetCommandList();
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    // Transition swap-chain back buffer PRESENT → RENDER_TARGET.
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);

    return primary;
}

void GraphicsDX12::EndFrame()
{
    // Resolve GPU timestamp queries before closing the primary CL.
    auto* primaryCmd = m_commandLists[0]->GetCommandList();
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.ResolveQueries(primaryCmd);

    // Transition swap-chain back buffer RENDER_TARGET → PRESENT on the primary CL.
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    primaryCmd->ResourceBarrier(1, &b);

    const uint32_t n = m_commandListCount;

    // -----------------------------------------------------------------------
    // Kahn's topological sort — determines GPU submission order.
    // An edge dep→i means: pool entry dep must execute before pool entry i.
    // -----------------------------------------------------------------------
    std::vector<int> inDegree(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t dep : m_commandLists[i]->wait_for)
            if (dep < n) ++inDegree[i];

    std::queue<uint32_t> ready;
    for (uint32_t i = 0; i < n; ++i)
        if (inDegree[i] == 0) ready.push(i);

    std::vector<uint32_t> order;
    order.reserve(n);
    while (!ready.empty())
    {
        const uint32_t cur = ready.front(); ready.pop();
        order.push_back(cur);
        // Reduce in-degree of every CL that depends on cur.
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t dep : m_commandLists[j]->wait_for)
                if (dep == cur && --inDegree[j] == 0)
                    ready.push(j);
    }

    if (order.size() != n)
    {
        LOG_ERROR("EndFrame: dependency cycle in command lists — appending remaining in pool order");
        for (uint32_t i = 0; i < n; ++i)
            if (std::find(order.begin(), order.end(), i) == order.end())
                order.push_back(i);
    }

    // -----------------------------------------------------------------------
    // Submit in topological order.
    // Same-queue ordering is guaranteed by submission sequence.
    // Cross-queue ordering is enforced via per-queue GPU fences:
    //   After each submission, signal the queue's m_queueFences entry.
    //   Before each submission, Wait() on every cross-queue dependency's fence.
    // -----------------------------------------------------------------------
    for (uint32_t idx : order)
    {
        auto& cl = *m_commandLists[idx];
        const UINT qi = static_cast<UINT>(cl.queue);

        ThrowIfFailed(cl.GetCommandList()->Close());

        // Insert GPU-side Wait() for every dependency on a different queue.
        for (uint32_t depIdx : cl.wait_for)
        {
            if (depIdx >= n) continue;
            const auto& dep = *m_commandLists[depIdx];
            if (dep.queue != cl.queue && dep.signal_fence_value != 0)
            {
                const UINT depQi = static_cast<UINT>(dep.queue);
                ThrowIfFailed(m_queues[qi]->Wait(
                    m_queueFences[depQi].Get(), dep.signal_fence_value));
            }
        }

        // Execute this command list on its queue.
        ID3D12CommandList* lists[] = { cl.commandLists[qi].Get() };
        m_queues[qi]->ExecuteCommandLists(1, lists);

        // Signal this queue's fence so later cross-queue dependents can Wait() on it.
        const uint64_t sigVal = ++m_queueFenceValues[qi];
        ThrowIfFailed(m_queues[qi]->Signal(m_queueFences[qi].Get(), sigVal));
        cl.signal_fence_value = sigVal;
    }

    // Present: VSync on → SyncInterval=1, VSync off → 0 + ALLOW_TEARING.
    auto logRemovedReason = [&](HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_HUNG &&
            hr != DXGI_ERROR_DEVICE_RESET) return;
        HRESULT reason = m_device->GetDeviceRemovedReason();
        const char* name = "unknown";
        switch ((uint32_t)reason)
        {
        case 0x887A0001: name = "DXGI_ERROR_INVALID_CALL";       break;
        case 0x887A0005: name = "DXGI_ERROR_DEVICE_REMOVED";     break;
        case 0x887A0006: name = "DXGI_ERROR_DEVICE_HUNG";        break;
        case 0x887A0007: name = "DXGI_ERROR_DEVICE_RESET";       break;
        case 0x887A0020: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
        case 0x80070057: name = "E_INVALIDARG (likely shader / resource state)"; break;
        case 0x8007000E: name = "E_OUTOFMEMORY (GPU out of VRAM)"; break;
        }
        LOG_ERROR("Device removed: hr=0x%08X reason=0x%08X (%s) — likely DXR/DDGI bug. Inspect prior log for last command.",
                  (unsigned)hr, (unsigned)reason, name);
    };

    if (vsyncEnabled)
    {
        HRESULT hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) logRemovedReason(hr);
        ThrowIfFailed(hr);
    }
    else
    {
        UINT flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = m_swapChain->Present(0, flags);
        if (FAILED(hr)) logRemovedReason(hr);
        ThrowIfFailed(hr);
    }

    // Signal fence for this frame — next BeginFrame will wait on it.
    // EndFrame does NOT wait — only BeginFrame does (single wait per frame).
    const uint64_t fv = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fv));

    // Record per-backbuffer fence values for the pipelined BeginFrame wait.
    // FrameCount frames later, when this backbuffer is reused, BeginFrame will
    // wait on these values before resetting its allocators / processing the
    // deferred-release slot.
    m_frameFenceValues[m_frameIndex] = fv;
    for (UINT q = 0; q < 3; ++q)
        m_frameQueueFenceValues[m_frameIndex][q] = m_queueFenceValues[q];
}

RHI::CommandList GraphicsDX12::BeginCommandList(RHI::QUEUE_TYPE queue)
{
    // ---- Allocate a pool slot (mutex-protected) ----------------------------
    m_commandListMutex.lock();
    const uint32_t id = m_commandListCount++;
    if (id >= static_cast<uint32_t>(m_commandLists.size()))
        m_commandLists.push_back(m_cmdAllocator.Allocate());
    CommandList_DX12* clPtr = m_commandLists[id];
    m_commandListMutex.unlock();

    // ---- Reset per-frame state ---------------------------------------------
    CommandList_DX12& cl     = *clPtr;
    cl.id                    = id;
    cl.queue                 = queue;
    cl.buffer_index          = m_frameIndex;
    cl.active_pso               = nullptr;
    cl.active_rootsig_graphics  = nullptr;
    cl.active_rootsig_compute   = nullptr;
    cl.prev_pt                  = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    cl.prev_stencilref          = 0;
    cl.dirty_pso                = false;
    cl.frame_barriers.clear();
    cl.discards.clear();
    cl.wait_for.clear();
    cl.signal_fence_value = 0;

    const UINT qi = static_cast<UINT>(queue);
    const D3D12_COMMAND_LIST_TYPE listType = ToD3D12CommandListType(queue);

    // ---- Lazily create command allocators (once per slot × frame × queue) --
    for (UINT f = 0; f < FrameCount; ++f)
    {
        if (!cl.commandAllocators[f][qi])
            ThrowIfFailed(m_device->CreateCommandAllocator(
                listType, IID_PPV_ARGS(&cl.commandAllocators[f][qi])));
    }

    // ---- Lazily create the command list for this slot + queue --------------
    if (!cl.commandLists[qi])
    {
        ThrowIfFailed(m_device->CreateCommandList(
            0, listType, cl.commandAllocators[m_frameIndex][qi].Get(),
            nullptr, IID_PPV_ARGS(&cl.commandLists[qi])));
        // Close immediately so Reset() works from the second frame onward.
        ThrowIfFailed(static_cast<ID3D12GraphicsCommandList*>(
            cl.commandLists[qi].Get())->Close());

        // Debug name visible in PIX / RenderDoc.
        std::wstring name = L"cmd" + std::to_wstring(id);
        cl.commandLists[qi]->SetName(name.c_str());
    }

    // ---- Reset allocator and open the command list -------------------------
    ThrowIfFailed(cl.commandAllocators[m_frameIndex][qi]->Reset());
    ThrowIfFailed(cl.GetCommandList()->Reset(
        cl.commandAllocators[m_frameIndex][qi].Get(), nullptr));

    // ---- Bind shader-visible descriptor heaps (DIRECT + COMPUTE, not COPY) -
    if (queue != RHI::QUEUE_TYPE::COPY)
    {
        ID3D12DescriptorHeap* heaps[] = {
            m_cbvSrvUavAllocator.GetGpuHeap(),
            m_samplerAllocator.GetGpuHeap()
        };
        cl.GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    return RHI::CommandList{ id };
}

// ===========================================================================
// Present composite — non-ImGui fullscreen blit
// ===========================================================================
//
// Builds a self-contained PSO (own root sig + own shaders, no dependencies on
// the engine's default root sig or ShaderLibrary) that samples an SRV and
// writes to the currently-bound swap chain RTV. Shaders are inline strings
// compiled on first use via D3DCompile; PSO + root sig are cached on the
// instance.

void GraphicsDX12::EnsurePresentPSO()
{
    if (m_presentPSO) return;

    // Fullscreen triangle using SV_VertexID. UV is top-left = (0,0).
    static const char* kVS =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint vid : SV_VertexID)\n"
        "{\n"
        "    VSOut o;\n"
        "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
        "    o.uv  = p;\n"
        "    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n";
    static const char* kPS =
        "Texture2D    g_tex : register(t0);\n"
        "SamplerState g_smp : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "{\n"
        "    return g_tex.Sample(g_smp, uv);\n"
        "}\n";

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> rsBlobOnly;
    ComPtr<ID3DBlob> err;

    DxcCompiler::CompileOptions vsOpt;
    vsOpt.sourceName = "Present.vs";
    vsOpt.entry      = "main";
    vsOpt.stage      = RHI::ShaderStage::VS;
    const auto vsRes = DxcCompiler::Compile(kVS, strlen(kVS), vsOpt);
    if (!vsRes.ok)
    {
        LOG_ERROR("Present VS compile: %s",
                  vsRes.errorMsg.empty() ? "(no diag)" : vsRes.errorMsg.c_str());
        return;
    }

    DxcCompiler::CompileOptions psOpt;
    psOpt.sourceName = "Present.ps";
    psOpt.entry      = "main";
    psOpt.stage      = RHI::ShaderStage::PS;
    const auto psRes = DxcCompiler::Compile(kPS, strlen(kPS), psOpt);
    if (!psRes.ok)
    {
        LOG_ERROR("Present PS compile: %s",
                  psRes.errorMsg.empty() ? "(no diag)" : psRes.errorMsg.c_str());
        return;
    }

    // Root signature: one SRV table + one static linear-clamp sampler.
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(1, params, 1, &samp,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &err)))
    {
        LOG_ERROR("Present root sig: %s",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "(no diag)");
        return;
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_presentRootSig)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_presentRootSig.Get();
    psoDesc.VS                    = CD3DX12_SHADER_BYTECODE(vsRes.dxil.data(), vsRes.dxil.size());
    psoDesc.PS                    = CD3DX12_SHADER_BYTECODE(psRes.dxil.data(), psRes.dxil.size());
    psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // matches swap chain (see CreateSwapChain)
    psoDesc.SampleDesc.Count      = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_presentPSO)));
}

void GraphicsDX12::CompositeTextureToSwapChain(uint64_t srvGpuHandle,
                                               RHI::CommandList cmd)
{
    if (srvGpuHandle == 0) return;
    EnsurePresentPSO();
    if (!m_presentPSO) return;

    ID3D12GraphicsCommandList* list = GetPoolEntry(cmd).GetCommandList();

    D3D12_VIEWPORT vp{ 0.f, 0.f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    D3D12_RECT sc{ 0, 0,
        static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetPipelineState(m_presentPSO.Get());
    list->SetGraphicsRootSignature(m_presentRootSig.Get());

    // Ensure the shader-visible heap is bound (caller likely already did, but
    // switching root sigs can reset the implicit binding — re-bind defensively).
    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvUavAllocator.GetGpuHeap() };
    list->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE h{ srvGpuHandle };
    list->SetGraphicsRootDescriptorTable(0, h);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 0, nullptr);
    list->IASetIndexBuffer(nullptr);
    list->DrawInstanced(3, 1, 0, 0);
}

// ===========================================================================
// Window / resize / fullscreen
// ===========================================================================

void GraphicsDX12::ResizeSwapChainAndRtv(UINT width, UINT height)
{
    for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
        m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
        m_renderTargets[n]->SetName(bbName.c_str());
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    m_width = width; m_height = height;
    m_viewport    = { 0,0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(width),  static_cast<LONG>(height) };
}

void GraphicsDX12::Resize(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    WaitForPreviousFrame();
    ResizeSwapChainAndRtv(width, height);
}

void GraphicsDX12::SetFullscreen(bool fullscreen)
{
    if (fullscreen)
    {
        ComPtr<IDXGIOutput> output;
        ThrowIfFailed(m_swapChain->GetContainingOutput(&output));
        DXGI_OUTPUT_DESC od; ThrowIfFailed(output->GetDesc(&od));
        UINT w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
        UINT h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
        WaitForPreviousFrame();
        for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
        ThrowIfFailed(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
            m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        ThrowIfFailed(m_swapChain->SetFullscreenState(TRUE, output.Get()));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE rh = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT n = 0; n < FrameCount; ++n)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
            m_renderTargets[n]->SetName(bbName.c_str());
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rh);
            rh.ptr += m_rtvDescriptorSize;
        }
        m_width = w; m_height = h;
        m_viewport    = { 0,0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
        m_scissorRect = { 0,0, static_cast<LONG>(w),  static_cast<LONG>(h) };
        CreateHdrRenderTarget(w, h);
    }
    else
    {
        WaitForPreviousFrame();
        ThrowIfFailed(m_swapChain->SetFullscreenState(FALSE, nullptr));
        RECT r; if (GetClientRect(m_hWnd, &r))
        {
            UINT w = r.right - r.left, h = r.bottom - r.top;
            if (w && h) ResizeSwapChainAndRtv(w, h);
        }
    }
}

void GraphicsDX12::SetViewportSize(uint32_t width, uint32_t height)
{
    UINT w = width  ? width  : m_width;
    UINT h = height ? height : m_height;
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w == m_hdrWidth && h == m_hdrHeight) return;
    CreateHdrRenderTarget(w, h);
}

void GraphicsDX12::SetRenderTargetToHdr(const float clearColor[4], RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0,0, static_cast<float>(m_hdrWidth), static_cast<float>(m_hdrHeight), 0,1 };
    D3D12_RECT     sr{ 0,0, static_cast<LONG>(m_hdrWidth),  static_cast<LONG>(m_hdrHeight) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_hdrRtvAllocation.GetCpuHandle();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // Use the same clear value as at creation to avoid CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE
    static const float hdrClear[4] = { 0.f, 0.f, 0.f, 0.f };
    cl->ClearRenderTargetView(rtv, hdrClear, 0, nullptr);
}

void GraphicsDX12::SetRenderTargetToHdrWithDepth(const RHI::Texture* dsv, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(m_hdrWidth), static_cast<float>(m_hdrHeight), 0, 1 };
    D3D12_RECT     sr{ 0, 0, static_cast<LONG>(m_hdrWidth),  static_cast<LONG>(m_hdrHeight) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_hdrRtvAllocation.GetCpuHandle();
    if (dsv && dsv->IsValid() && dsv->handle_id < static_cast<uint32_t>(m_texturePool.size()))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(1, &rtv, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    // No clear — skybox draws on top of existing HDR content.
}

void GraphicsDX12::SetRenderTargetToSwapChain(const float clearColor[4], RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrRenderTarget && m_hdrResourceState == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    cl->RSSetViewports(1, &m_viewport);
    cl->RSSetScissorRects(1, &m_scissorRect);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

uint64_t GraphicsDX12::GetHdrSceneSrvGpuHandle() const
{
    return m_hdrSrvAllocation.IsValid() ? m_hdrSrvAllocation.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetHdrSceneUavGpuHandle() const
{
    return m_hdrUavAllocation.IsValid() ? m_hdrUavAllocation.GetGpuHandle().ptr : 0ull;
}

void GraphicsDX12::CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !dst.IsValid()) return;
    if (dst.handle_id >= m_texturePool.size()) return;
    ID3D12Resource* dstRes = m_texturePool[dst.handle_id].resource.Get();
    if (!dstRes) return;
    GetPoolEntry(cmd).GetCommandList()->CopyResource(dstRes, m_hdrRenderTarget.Get());
}

uint64_t GraphicsDX12::GetTextureSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureSRVCpuHandleNative(const RHI::Texture& texture) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE nul{};
    if (!texture.IsValid()) return nul;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return nul;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    // GpuCpuHandle = the CPU-visible handle inside the shader-visible heap,
    // suitable as the SOURCE operand for CopyDescriptorsSimple.
    return entry.srv.IsValid() ? entry.srv.GetGpuCpuHandle() : nul;
}

uint64_t GraphicsDX12::GetTextureSRVCpuHandle(const RHI::Texture& texture) const
{
    return GetTextureSRVCpuHandleNative(texture).ptr;
}

void GraphicsDX12::CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                            uint64_t srcCpuHandle,
                                            uint32_t count)
{
    if (dstCpuHandle == 0 || srcCpuHandle == 0 || count == 0) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst{ static_cast<SIZE_T>(dstCpuHandle) };
    D3D12_CPU_DESCRIPTOR_HANDLE src{ static_cast<SIZE_T>(srcCpuHandle) };
    m_device->CopyDescriptorsSimple(count, dst, src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t GraphicsDX12::GetCbvSrvUavDescriptorIncrement() const
{
    return m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint64_t GraphicsDX12::GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    // Return UNORM alias if available (for SRGB textures), else fall back to normal SRV.
    if (entry.previewSrv.IsValid()) return entry.previewSrv.GetGpuHandle().ptr;
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

// ===========================================================================
// Resource creation — CreateBuffer
// ===========================================================================

bool GraphicsDX12::CreateBuffer(const RHI::GPUBufferDesc& desc,
                                RHI::GPUBuffer& outBuffer,
                                const void* initialData)
{
    if (desc.size == 0) { LOG_ERROR("CreateBuffer: size == 0"); return false; }

    GPUBuffer_DX12 entry;
    D3D12_HEAP_PROPERTIES hp{ ToD3D12HeapType(desc.usage) };

    const bool isCBV = RHI::HasFlag(desc.bind_flags, RHI::BindFlag::CONSTANT_BUFFER);
    UINT64 allocSize = isCBV ? (desc.size + 255) & ~255ull : desc.size;

    D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(allocSize);
    rd.Flags = ToD3D12ResourceFlags(desc.bind_flags);

    D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;
    if (desc.usage == RHI::Usage::UPLOAD)
        initState = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (desc.usage == RHI::Usage::READBACK)
        initState = D3D12_RESOURCE_STATE_COPY_DEST;

    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr,
            IID_PPV_ARGS(&entry.resource))))
    {
        LOG_ERROR("CreateBuffer: CreateCommittedResource failed");
        return false;
    }
    entry.state = initState;

    if (initialData && desc.usage == RHI::Usage::UPLOAD)
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        if (SUCCEEDED(entry.resource->Map(0, &readRange, &mapped)))
        {
            std::memcpy(mapped, initialData, static_cast<size_t>(desc.size));
            entry.resource->Unmap(0, nullptr);
        }
    }
    else if (initialData && desc.usage == RHI::Usage::DEFAULT)
    {
        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC   stageDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.size);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_NONE, &stageDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
        void* mapped = nullptr; D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(staging->Map(0, &rr, &mapped));
        std::memcpy(mapped, initialData, static_cast<size_t>(desc.size));
        staging->Unmap(0, nullptr);

        // After copy, transition to an appropriate shader-readable state.
        const bool isSRV = RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE);
        const D3D12_RESOURCE_STATES finalState = isSRV
            ? (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &b1);
        m_commandList->CopyBufferRegion(entry.resource.Get(), 0, staging.Get(), 0, desc.size);
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        m_commandList->ResourceBarrier(1, &b2);
        entry.state = finalState;

        // Inside a batch scope: defer the fence wait until EndBufferUploadBatch(),
        // but cap how much staging piles up so the batch doesn't OOM on
        // scenes with thousands of small meshes.
        if (m_batchUploadDepth > 0)
        {
            m_batchStagingBytes += desc.size;
            m_batchStagingKeepAlive.push_back(std::move(staging));
            FlushBatchStagingIfNeeded();
        }
        else
        {
            FlushUploadAndWait();
        }
    }

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::VERTEX_BUFFER))
    {
        entry.vbv.BufferLocation = entry.resource->GetGPUVirtualAddress();
        entry.vbv.SizeInBytes    = static_cast<UINT>(desc.size);
        entry.vbv.StrideInBytes  = desc.stride;
    }
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::INDEX_BUFFER))
    {
        entry.ibv.BufferLocation = entry.resource->GetGPUVirtualAddress();
        entry.ibv.SizeInBytes    = static_cast<UINT>(desc.size);
        entry.ibv.Format         = (desc.format == RHI::Format::R16_UINT)
                                   ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }
    if (isCBV)
    {
        entry.cbv = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvd{};
        cbvd.BufferLocation = entry.resource->GetGPUVirtualAddress();
        cbvd.SizeInBytes    = static_cast<UINT>(allocSize);
        m_device->CreateConstantBufferView(&cbvd, entry.cbv.GetCpuHandle());
        entry.cbv.CopyToGpu();
    }
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE))
    {
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (RHI::HasFlag(desc.misc_flags, RHI::ResourceMiscFlag::BUFFER_RAW))
        {
            // ByteAddressBuffer — raw 32-bit access
            srvd.Format               = DXGI_FORMAT_R32_TYPELESS;
            srvd.Buffer.NumElements   = static_cast<UINT>(desc.size / 4);
            srvd.Buffer.Flags         = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        else
        {
            // StructuredBuffer or typed buffer
            srvd.Format                     = ToDxgiFormat(desc.format);
            srvd.Buffer.NumElements         = (desc.stride > 0)
                                              ? static_cast<UINT>(desc.size / desc.stride)
                                              : static_cast<UINT>(desc.size / 4);
            srvd.Buffer.StructureByteStride = desc.stride;
        }
        m_device->CreateShaderResourceView(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();
    }

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;

        if (RHI::HasFlag(desc.misc_flags, RHI::ResourceMiscFlag::BUFFER_RAW))
        {
            // RWByteAddressBuffer — raw 32-bit access. Mirror the SRV path
            // above (R32_TYPELESS + RAW flag) or D3D12 rejects the view as
            // "Format (0, UNKNOWN) cannot be used with a typed View of a
            // Buffer" → device-removed cascade.
            uavd.Format                     = DXGI_FORMAT_R32_TYPELESS;
            uavd.Buffer.NumElements         = static_cast<UINT>(desc.size / 4);
            uavd.Buffer.StructureByteStride = 0;
            uavd.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        else
        {
            // RWStructuredBuffer or typed buffer.
            uavd.Format                     = DXGI_FORMAT_UNKNOWN;
            uavd.Buffer.NumElements         = (desc.stride > 0)
                                              ? static_cast<UINT>(desc.size / desc.stride)
                                              : static_cast<UINT>(desc.size / 4);
            uavd.Buffer.StructureByteStride = desc.stride;
        }

        m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
        entry.uav.CopyToGpu();
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_bufferFreeList.empty())
    {
        outBuffer.handle_id         = m_bufferFreeList.back();
        m_bufferFreeList.pop_back();
        m_bufferPool[outBuffer.handle_id] = std::move(entry);
    }
    else
    {
        outBuffer.handle_id = static_cast<uint32_t>(m_bufferPool.size());
        m_bufferPool.push_back(std::move(entry));
    }
    outBuffer.type = RHI::GPUResource::Type::Buffer;
    outBuffer.desc = desc;
    LOG_SUCCESS("CreateBuffer: %llu bytes", (unsigned long long)desc.size);
    return true;
}

// ===========================================================================
// Resource creation — CreateTexture
// ===========================================================================

bool GraphicsDX12::CreateTexture(const RHI::TextureDesc& desc,
                                 RHI::Texture& outTexture,
                                 const RHI::SubresourceData* initialData)
{
    Texture_DX12 entry;
    DXGI_FORMAT dxgiFmt = ToDxgiFormat(desc.format);
    D3D12_HEAP_PROPERTIES hp{ ToD3D12HeapType(desc.usage) };

    // Depth texture that also needs to be sampled as SRV requires a typeless resource format.
    const bool isDepthFmt = (dxgiFmt == DXGI_FORMAT_D32_FLOAT || dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT);
    const bool needsDepthSRV = isDepthFmt
        && RHI::HasFlag(desc.bind_flags, RHI::BindFlag::DEPTH_STENCIL)
        && RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE);

    D3D12_RESOURCE_DESC rd{};
    switch (desc.type)
    {
    case RHI::TextureDesc::Type::TEXTURE_1D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
    case RHI::TextureDesc::Type::TEXTURE_3D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
    default:                                 rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
    }
    rd.Width              = desc.width;
    rd.Height             = desc.height;
    rd.DepthOrArraySize   = static_cast<UINT16>((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? desc.depth : desc.array_size);
    rd.MipLevels          = static_cast<UINT16>(desc.mip_levels);
    // Typeless resource format for depth textures that need SRV access.
    DXGI_FORMAT resourceFmt = dxgiFmt;
    if (needsDepthSRV)
    {
        if (dxgiFmt == DXGI_FORMAT_D32_FLOAT)          resourceFmt = DXGI_FORMAT_R32_TYPELESS;
        else if (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT) resourceFmt = DXGI_FORMAT_R24G8_TYPELESS;
    }
    rd.Format             = resourceFmt;
    rd.SampleDesc.Count   = desc.sample_count;
    rd.SampleDesc.Quality = 0;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags              = ToD3D12ResourceFlags(desc.bind_flags);

    D3D12_RESOURCE_STATES initState = ToD3D12ResourceState(desc.layout);
    D3D12_CLEAR_VALUE cv{};
    D3D12_CLEAR_VALUE* pCv = nullptr;
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::RENDER_TARGET))
    {
        cv.Format = dxgiFmt;
        std::memcpy(cv.Color, desc.clear.color, sizeof(cv.Color));
        pCv = &cv; initState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    else if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::DEPTH_STENCIL))
    {
        cv.Format               = dxgiFmt; // use the original depth format (D32_FLOAT or D24_S8)
        cv.DepthStencil.Depth   = desc.clear.depth_stencil.depth;
        cv.DepthStencil.Stencil = static_cast<UINT8>(desc.clear.depth_stencil.stencil);
        pCv = &cv; initState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initState, pCv,
            IID_PPV_ARGS(&entry.resource))))
    {
        LOG_ERROR("CreateTexture: CreateCommittedResource failed");
        return false;
    }
    entry.state = initState;

    // Optional debug name → ID3D12Resource::SetName so D3D12 validation
    // messages and PIX/RenderDoc captures show a meaningful identifier.
    if (desc.debug_name && desc.debug_name[0] != '\0')
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, desc.debug_name, -1, nullptr, 0);
        if (needed > 0)
        {
            std::wstring wname(static_cast<size_t>(needed - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, desc.debug_name, -1, wname.data(), needed);
            entry.resource->SetName(wname.c_str());
        }
    }

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE))
    {
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        // SRV format for depth textures: R32_FLOAT for D32, R24_UNORM_X8 for D24_S8.
        DXGI_FORMAT srvFmt = dxgiFmt;
        if (needsDepthSRV)
        {
            if (dxgiFmt == DXGI_FORMAT_D32_FLOAT)              srvFmt = DXGI_FORMAT_R32_FLOAT;
            else if (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT) srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        }
        srvd.Format                  = srvFmt;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (rd.Dimension)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (desc.array_size > 1) {
                srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvd.Texture1DArray.MipLevels = desc.mip_levels;
                srvd.Texture1DArray.ArraySize = desc.array_size;
            } else {
                srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvd.Texture1D.MipLevels = desc.mip_levels;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
            srvd.Texture3D.MipLevels = desc.mip_levels;
            break;
        default: // 2D
            if (desc.sample_count > 1) {
                srvd.ViewDimension = (desc.array_size > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                           : D3D12_SRV_DIMENSION_TEXTURE2DMS;
            } else if (RHI::HasFlag(desc.misc_flags, RHI::ResourceMiscFlag::TEXTURECUBE)
                       && desc.array_size == 6) {
                srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvd.TextureCube.MipLevels      = desc.mip_levels;
                srvd.TextureCube.MostDetailedMip = 0;
                LOG_INFO("CreateTexture: created TextureCube SRV (%ux%u mips=%u)", desc.width, desc.height, desc.mip_levels);
            } else if (RHI::HasFlag(desc.misc_flags, RHI::ResourceMiscFlag::TEXTURECUBE)
                       && desc.array_size > 6 && (desc.array_size % 6) == 0) {
                srvd.ViewDimension                          = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvd.TextureCubeArray.MipLevels             = desc.mip_levels;
                srvd.TextureCubeArray.MostDetailedMip       = 0;
                srvd.TextureCubeArray.First2DArrayFace      = 0;
                srvd.TextureCubeArray.NumCubes              = desc.array_size / 6;
                srvd.TextureCubeArray.ResourceMinLODClamp   = 0.0f;
                LOG_INFO("CreateTexture: created TextureCubeArray SRV (%ux%u mips=%u cubes=%u)",
                         desc.width, desc.height, desc.mip_levels, desc.array_size / 6);
            } else if (desc.array_size > 1) {
                srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvd.Texture2DArray.MipLevels   = desc.mip_levels;
                srvd.Texture2DArray.ArraySize   = desc.array_size;
            } else {
                srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvd.Texture2D.MipLevels = desc.mip_levels;
            }
            break;
        }
        m_device->CreateShaderResourceView(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();

        // For SRGB textures, create a UNORM alias SRV for editor preview.
        // ImGui renders to a UNORM RTV, so sampling an SRGB SRV would linearize
        // the values without re-encoding on write → preview appears too dark.
        // The UNORM alias returns raw sRGB texel data, matching what the user expects.
        auto stripSrgb = [](DXGI_FORMAT f) -> DXGI_FORMAT {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_BC1_UNORM_SRGB:      return DXGI_FORMAT_BC1_UNORM;
            case DXGI_FORMAT_BC2_UNORM_SRGB:      return DXGI_FORMAT_BC2_UNORM;
            case DXGI_FORMAT_BC3_UNORM_SRGB:      return DXGI_FORMAT_BC3_UNORM;
            case DXGI_FORMAT_BC7_UNORM_SRGB:      return DXGI_FORMAT_BC7_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
            default: return f;
            }
        };
        DXGI_FORMAT unormFmt = stripSrgb(dxgiFmt);
        if (unormFmt != dxgiFmt)
        {
            entry.previewSrv = m_cbvSrvUavAllocator.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC previewSrvd = srvd;
            previewSrvd.Format = unormFmt;
            m_device->CreateShaderResourceView(entry.resource.Get(), &previewSrvd, entry.previewSrv.GetCpuHandle());
            entry.previewSrv.CopyToGpu();
        }
    }
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::RENDER_TARGET))
    {
        entry.rtv = m_rtvAllocator.Allocate(1);
        D3D12_RENDER_TARGET_VIEW_DESC rtvd{ dxgiFmt, D3D12_RTV_DIMENSION_TEXTURE2D };
        m_device->CreateRenderTargetView(entry.resource.Get(), &rtvd, entry.rtv.GetCpuHandle());
    }
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::DEPTH_STENCIL))
    {
        entry.dsv = m_dsvAllocator.Allocate(1);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
        dsvd.Format = dxgiFmt;
        if (desc.array_size > 1)
        {
            dsvd.ViewDimension                   = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvd.Texture2DArray.MipSlice         = 0;
            dsvd.Texture2DArray.FirstArraySlice  = 0;
            dsvd.Texture2DArray.ArraySize        = desc.array_size;
        }
        else
        {
            dsvd.ViewDimension                   = D3D12_DSV_DIMENSION_TEXTURE2D;
        }
        m_device->CreateDepthStencilView(entry.resource.Get(), &dsvd, entry.dsv.GetCpuHandle());

        // Per-slice DSVs for array depth textures (CSM cascades, etc.)
        if (desc.array_size > 1)
        {
            entry.sliceDsvs.resize(desc.array_size);
            D3D12_DEPTH_STENCIL_VIEW_DESC sliceDsvd{};
            sliceDsvd.Format                             = dxgiFmt;
            sliceDsvd.ViewDimension                      = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            sliceDsvd.Texture2DArray.MipSlice            = 0;
            sliceDsvd.Texture2DArray.ArraySize           = 1;
            for (uint32_t s = 0; s < desc.array_size; ++s)
            {
                entry.sliceDsvs[s] = m_dsvAllocator.Allocate(1);
                sliceDsvd.Texture2DArray.FirstArraySlice = s;
                m_device->CreateDepthStencilView(entry.resource.Get(), &sliceDsvd,
                                                 entry.sliceDsvs[s].GetCpuHandle());
            }
        }
    }

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        // Whole-resource (mip 0) UAV — covers all array slices when array_size > 1.
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        auto fillUav = [&](D3D12_UNORDERED_ACCESS_VIEW_DESC& uavd, uint32_t mip)
        {
            uavd.Format = dxgiFmt;
            if (desc.type == RHI::TextureDesc::Type::TEXTURE_3D)
            {
                uavd.ViewDimension          = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavd.Texture3D.MipSlice     = mip;
                uavd.Texture3D.FirstWSlice  = 0;
                uavd.Texture3D.WSize        = (std::max)(desc.depth >> mip, 1u);
            }
            else if (desc.array_size > 1)
            {
                // Cube or 2D array — UAV covers all array slices at this mip.
                uavd.ViewDimension                       = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavd.Texture2DArray.MipSlice             = mip;
                uavd.Texture2DArray.FirstArraySlice      = 0;
                uavd.Texture2DArray.ArraySize            = desc.array_size;
                uavd.Texture2DArray.PlaneSlice           = 0;
            }
            else
            {
                uavd.ViewDimension          = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavd.Texture2D.MipSlice     = mip;
            }
        };
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
            fillUav(uavd, 0);
            m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
            entry.uav.CopyToGpu();
        }

        // Per-mip UAVs for multi-mip textures (cubemap prefilter chains, etc.)
        if (desc.mip_levels > 1)
        {
            entry.mipUavs.resize(desc.mip_levels);
            for (uint32_t mip = 0; mip < desc.mip_levels; ++mip)
            {
                entry.mipUavs[mip] = m_cbvSrvUavAllocator.Allocate(1);
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
                fillUav(uavd, mip);
                m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd,
                                                    entry.mipUavs[mip].GetCpuHandle());
                entry.mipUavs[mip].CopyToGpu();
            }
        }
    }

    if (initialData && desc.usage == RHI::Usage::DEFAULT)
    {
        const UINT subresourceCount = desc.mip_levels * desc.array_size;
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0, nullptr, nullptr, nullptr, &uploadSize);

        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
        const auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_NONE, &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
        std::vector<UINT>   numRows(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0,
            footprints.data(), numRows.data(), rowSizes.data(), nullptr);

        uint8_t* stagingPtr = nullptr;
        D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(staging->Map(0, &rr, reinterpret_cast<void**>(&stagingPtr)));
        for (UINT sub = 0; sub < subresourceCount; ++sub)
        {
            const auto& fp  = footprints[sub];
            uint8_t* dstRow = stagingPtr + fp.Offset;
            const uint8_t* srcRow = static_cast<const uint8_t*>(initialData[sub].data_ptr);
            for (UINT row = 0; row < numRows[sub]; ++row)
            {
                std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizes[sub]));
                dstRow  += fp.Footprint.RowPitch;
                srcRow  += initialData[sub].row_pitch;
            }
        }
        staging->Unmap(0, nullptr);

        auto toTransfer = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &toTransfer);
        for (UINT sub = 0; sub < subresourceCount; ++sub)
        {
            CD3DX12_TEXTURE_COPY_LOCATION dst(entry.resource.Get(), sub);
            CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[sub]);
            m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        auto fromTransfer = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
        m_commandList->ResourceBarrier(1, &fromTransfer);
        FlushUploadAndWait();
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_textureFreeList.empty())
    {
        outTexture.handle_id          = m_textureFreeList.back();
        m_textureFreeList.pop_back();
        m_texturePool[outTexture.handle_id] = std::move(entry);
    }
    else
    {
        outTexture.handle_id = static_cast<uint32_t>(m_texturePool.size());
        m_texturePool.push_back(std::move(entry));
    }
    outTexture.type = RHI::GPUResource::Type::Texture;
    outTexture.desc = desc;

    // Copy SRV into bindless texture table at slot [handle_id].
    if (m_bindlessTexTable.IsValid() && outTexture.handle_id < kMaxBindlessTextures)
    {
        const auto& poolEntry = m_texturePool[outTexture.handle_id];
        if (poolEntry.srv.IsValid())
        {
            UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessTexTable.GetGpuCpuHandle();
            dst.ptr += static_cast<SIZE_T>(outTexture.handle_id) * descInc;
            m_device->CopyDescriptorsSimple(1, dst, poolEntry.srv.GetCpuHandle(),
                                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    LOG_SUCCESS("CreateTexture: %ux%u fmt=%u", desc.width, desc.height, (unsigned)desc.format);
    return true;
}

// ===========================================================================
// Resource creation — CreateShader
// ===========================================================================

bool GraphicsDX12::CreateShader(RHI::ShaderStage stage,
                                const void* bytecode, size_t bytecodeSize,
                                RHI::Shader& outShader)
{
    if (!bytecode || bytecodeSize == 0)
    {
        LOG_ERROR("CreateShader: empty bytecode");
        return false;
    }
    Shader_DX12 entry;
    const auto* ptr = static_cast<const uint8_t*>(bytecode);
    entry.bytecode.assign(ptr, ptr + bytecodeSize);

    // Stable FNV-1a over bytecode — same HLSL compile produces the same
    // DXBC produces the same hash, so PSO cache names are stable across
    // engine restarts even when the caller didn't set desc.cache_key.
    {
        uint64_t h = 14695981039346656037ull;
        for (uint8_t b : entry.bytecode) { h ^= b; h *= 1099511628211ull; }
        entry.bytecodeHash = h;
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    outShader.handle_id = static_cast<uint32_t>(m_shaderPool.size());
    outShader.type      = RHI::GPUResource::Type::Shader;
    outShader.stage     = stage;
    m_shaderPool.push_back(std::move(entry));
    LOG_SUCCESS("CreateShader: stage=%u, %zu bytes", (unsigned)stage, bytecodeSize);
    return true;
}

// ===========================================================================
// Resource creation — CreateSampler
// ===========================================================================

bool GraphicsDX12::CreateSampler(const RHI::SamplerDesc& desc, int& outDescriptorIndex)
{
    auto allocation = m_samplerAllocator.Allocate(1);
    if (!allocation.IsValid())
    {
        LOG_ERROR("CreateSampler: sampler heap exhausted (max %u)", MaxSamplers);
        outDescriptorIndex = -1;
        return false;
    }

    D3D12_SAMPLER_DESC sd{};
    sd.Filter         = ToD3D12Filter(desc.filter);
    sd.AddressU       = ToD3D12AddressMode(desc.address_u);
    sd.AddressV       = ToD3D12AddressMode(desc.address_v);
    sd.AddressW       = ToD3D12AddressMode(desc.address_w);
    sd.MipLODBias     = desc.mip_lod_bias;
    sd.MaxAnisotropy  = desc.max_anisotropy;
    sd.ComparisonFunc = ToD3D12ComparisonFunc(desc.comparison_func);
    sd.MinLOD         = desc.min_lod;
    sd.MaxLOD         = desc.max_lod;
    switch (desc.border_color)
    {
    case RHI::SamplerBorderColor::OPAQUE_BLACK:
        sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=0.0f; sd.BorderColor[3]=1.0f; break;
    case RHI::SamplerBorderColor::OPAQUE_WHITE:
        sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=sd.BorderColor[3]=1.0f; break;
    default: break; // transparent black (all zero)
    }

    m_device->CreateSampler(&sd, allocation.GetCpuHandle());
    allocation.CopyToGpu();

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    outDescriptorIndex = static_cast<int>(m_samplerPool.size());
    m_samplerPool.push_back(std::move(allocation));

    LOG_SUCCESS("CreateSampler: descriptor index=%d", outDescriptorIndex);
    return true;
}

// ===========================================================================
// Resource creation — CreatePipelineState
// ===========================================================================

bool GraphicsDX12::CreatePipelineState(const RHI::PipelineStateDesc& desc,
                                       RHI::PipelineState& outPSO)
{
    PipelineState_DX12 internal;

    // If a compute shader is set and no vertex shader, create a compute PSO.
    if (desc.cs && !desc.vs)
    {
        if (!m_computeRootSignature)
        { LOG_ERROR("CreatePipelineState: compute root signature not initialized"); return false; }
        internal.rootSignature = m_computeRootSignature;
        internal.isCompute     = true;

        const Shader_DX12& csBytecode = m_shaderPool[desc.cs->handle_id];
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc{};
        cpsoDesc.pRootSignature = m_computeRootSignature.Get();
        cpsoDesc.CS             = { csBytecode.bytecode.data(), csBytecode.bytecode.size() };

        const HRESULT hr = m_device->CreateComputePipelineState(&cpsoDesc, IID_PPV_ARGS(&internal.pso));
        if (FAILED(hr))
        {
            LOG_ERROR("CreatePipelineState: CreateComputePipelineState failed 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        outPSO.handle_id = static_cast<uint32_t>(m_psoPool.size());
        outPSO.type      = RHI::GPUResource::Type::PipelineState;
        m_psoPool.push_back(std::make_unique<PipelineState_DX12>(std::move(internal)));
        LOG_SUCCESS("CreatePipelineState (compute): OK");
        return true;
    }

    // Reuse the root signature built once in LoadAssets().
    if (!m_defaultRootSignature)
    { LOG_ERROR("CreatePipelineState: default root signature not initialised"); return false; }
    internal.rootSignature = m_defaultRootSignature;

    // Helper to extract bytecode from a Shader handle
    auto getByteCode = [this](const RHI::Shader* s) -> D3D12_SHADER_BYTECODE
    {
        if (!s || !s->IsValid()) return { nullptr, 0 };
        const Shader_DX12& p = m_shaderPool[s->handle_id];
        return { p.bytecode.data(), p.bytecode.size() };
    };

    // Translate blend state
    D3D12_BLEND_DESC blendDesc{};
    if (desc.bs)
    {
        blendDesc.AlphaToCoverageEnable  = desc.bs->alpha_to_coverage_enable;
        blendDesc.IndependentBlendEnable = desc.bs->independent_blend_enable;
        for (int i = 0; i < 8; ++i)
        {
            const auto& s = desc.bs->render_target[i];
            auto& d = blendDesc.RenderTarget[i];
            d.BlendEnable           = s.blend_enable;
            d.SrcBlend              = ToD3D12Blend(s.src_blend);
            d.DestBlend             = ToD3D12Blend(s.dest_blend);
            d.BlendOp               = ToD3D12BlendOp(s.blend_op);
            d.SrcBlendAlpha         = ToD3D12Blend(s.src_blend_alpha);
            d.DestBlendAlpha        = ToD3D12Blend(s.dest_blend_alpha);
            d.BlendOpAlpha          = ToD3D12BlendOp(s.blend_op_alpha);
            // D3D12 allows only the least significant 4 bits (R/G/B/A); RHI::ColorWrite::ENABLE_ALL is ~0u
            d.RenderTargetWriteMask = static_cast<UINT8>(s.render_target_write_mask) & 0x0Fu;
        }
    }
    else
    {
        for (auto& rt : blendDesc.RenderTarget)
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Translate rasterizer state
    D3D12_RASTERIZER_DESC rasterDesc{};
    if (desc.rs)
    {
        rasterDesc.FillMode              = ToD3D12FillMode(desc.rs->fill_mode);
        rasterDesc.CullMode              = ToD3D12CullMode(desc.rs->cull_mode);
        rasterDesc.FrontCounterClockwise = desc.rs->front_counter_clockwise;
        rasterDesc.DepthBias             = desc.rs->depth_bias;
        rasterDesc.DepthBiasClamp        = desc.rs->depth_bias_clamp;
        rasterDesc.SlopeScaledDepthBias  = desc.rs->slope_scaled_depth_bias;
        rasterDesc.DepthClipEnable       = desc.rs->depth_clip_enable;
        rasterDesc.MultisampleEnable     = desc.rs->multisample_enable;
        rasterDesc.AntialiasedLineEnable = desc.rs->antialiased_line_enable;
        rasterDesc.ConservativeRaster    = desc.rs->conservative_rasterization_enable
                                          ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                          : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
    else
    {
        rasterDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    }

    // Translate depth-stencil state
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    if (desc.dss)
    {
        auto toStencilOp = [](RHI::StencilOp o) -> D3D12_STENCIL_OP {
            switch(o) {
            case RHI::StencilOp::KEEP:     return D3D12_STENCIL_OP_KEEP;
            case RHI::StencilOp::ZERO:     return D3D12_STENCIL_OP_ZERO;
            case RHI::StencilOp::REPLACE:  return D3D12_STENCIL_OP_REPLACE;
            case RHI::StencilOp::INCR_SAT: return D3D12_STENCIL_OP_INCR_SAT;
            case RHI::StencilOp::DECR_SAT: return D3D12_STENCIL_OP_DECR_SAT;
            case RHI::StencilOp::INVERT:   return D3D12_STENCIL_OP_INVERT;
            case RHI::StencilOp::INCR:     return D3D12_STENCIL_OP_INCR;
            case RHI::StencilOp::DECR:     return D3D12_STENCIL_OP_DECR;
            default:                       return D3D12_STENCIL_OP_KEEP;
            }
        };
        dsDesc.DepthEnable      = desc.dss->depth_enable;
        dsDesc.DepthWriteMask   = (desc.dss->depth_write_mask == RHI::DepthWriteMask::ALL)
                                  ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc        = ToD3D12ComparisonFunc(desc.dss->depth_func);
        dsDesc.StencilEnable    = desc.dss->stencil_enable;
        dsDesc.StencilReadMask  = desc.dss->stencil_read_mask;
        dsDesc.StencilWriteMask = desc.dss->stencil_write_mask;
        dsDesc.FrontFace = { toStencilOp(desc.dss->front_face.stencil_fail_op),
                             toStencilOp(desc.dss->front_face.stencil_depth_fail_op),
                             toStencilOp(desc.dss->front_face.stencil_pass_op),
                             ToD3D12ComparisonFunc(desc.dss->front_face.stencil_func) };
        dsDesc.BackFace  = { toStencilOp(desc.dss->back_face.stencil_fail_op),
                             toStencilOp(desc.dss->back_face.stencil_depth_fail_op),
                             toStencilOp(desc.dss->back_face.stencil_pass_op),
                             ToD3D12ComparisonFunc(desc.dss->back_face.stencil_func) };
    }

    // -----------------------------------------------------------------------
    // Mesh-shader path: AS + MS (+ optional PS) via stream pipeline desc.
    // Uses ID3D12Device2::CreatePipelineState. PSO library bypassed for now —
    // ID3D12PipelineLibrary1::LoadPipeline is needed for stream descs and
    // can be added later once warm-start is observed to be slow.
    // -----------------------------------------------------------------------
    if (desc.ms || desc.as)
    {
        ComPtr<ID3D12Device2> device2;
        if (FAILED(m_device.As(&device2)))
        {
            LOG_ERROR("CreatePipelineState (mesh): ID3D12Device2 unavailable");
            return false;
        }

        struct MeshPSOStream
        {
            CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        rootSig;
            CD3DX12_PIPELINE_STATE_STREAM_AS                    as;
            CD3DX12_PIPELINE_STATE_STREAM_MS                    ms;
            CD3DX12_PIPELINE_STATE_STREAM_PS                    ps;
            CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            blend;
            CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            raster;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL         depth;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT  dsFormat;
            CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS rtFormats;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC           sample;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK           sampleMask;
        } stream{};

        stream.rootSig  = internal.rootSignature.Get();
        stream.as       = getByteCode(desc.as);
        stream.ms       = getByteCode(desc.ms);
        stream.ps       = getByteCode(desc.ps);
        stream.blend    = CD3DX12_BLEND_DESC(blendDesc);
        stream.raster   = CD3DX12_RASTERIZER_DESC(rasterDesc);
        stream.depth    = CD3DX12_DEPTH_STENCIL_DESC(dsDesc);
        stream.dsFormat = ToDxgiFormat(desc.dsv_format);

        D3D12_RT_FORMAT_ARRAY rtfa{};
        rtfa.NumRenderTargets = desc.rtv_count;
        for (UINT i = 0; i < desc.rtv_count && i < 8u; ++i)
            rtfa.RTFormats[i] = ToDxgiFormat(desc.rtv_formats[i]);
        stream.rtFormats = rtfa;

        DXGI_SAMPLE_DESC sd{};
        sd.Count   = desc.sample_count ? desc.sample_count : 1;
        sd.Quality = 0;
        stream.sample     = sd;
        stream.sampleMask = desc.sample_mask;

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
        const HRESULT hr = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&internal.pso));
        if (FAILED(hr))
        {
            LOG_ERROR("CreatePipelineState (mesh): CreatePipelineState failed 0x%08X",
                      static_cast<unsigned>(hr));
            return false;
        }

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        outPSO.handle_id = static_cast<uint32_t>(m_psoPool.size());
        outPSO.type      = RHI::GPUResource::Type::PipelineState;
        m_psoPool.push_back(std::make_unique<PipelineState_DX12>(std::move(internal)));
        LOG_SUCCESS("CreatePipelineState (mesh): OK");
        return true;
    }

    // Translate input layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    if (desc.il)
    {
        for (const auto& e : desc.il->elements)
        {
            D3D12_INPUT_ELEMENT_DESC ied{};
            ied.SemanticName         = e.semantic_name;
            ied.SemanticIndex        = e.semantic_index;
            ied.Format               = ToDxgiFormat(e.format);
            ied.InputSlot            = e.input_slot;
            ied.AlignedByteOffset    = (e.aligned_byte_offset == RHI::InputLayout::APPEND_ALIGNED_ELEMENT)
                                       ? D3D12_APPEND_ALIGNED_ELEMENT : e.aligned_byte_offset;
            ied.InputSlotClass       = (e.input_slot_class == RHI::InputClassification::PER_INSTANCE_DATA)
                                       ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                       : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            ied.InstanceDataStepRate = (ied.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ? 1 : 0;
            inputElements.push_back(ied);
        }
    }

    // Map RHI topology to D3D12 topology type
    auto toTopologyType = [](RHI::PrimitiveTopology t) -> D3D12_PRIMITIVE_TOPOLOGY_TYPE {
        switch (t) {
        case RHI::PrimitiveTopology::POINTLIST:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case RHI::PrimitiveTopology::LINELIST:
        case RHI::PrimitiveTopology::LINESTRIP:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case RHI::PrimitiveTopology::PATCHLIST:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        default:                                  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = internal.rootSignature.Get();
    psoDesc.VS                    = getByteCode(desc.vs);
    psoDesc.PS                    = getByteCode(desc.ps);
    psoDesc.HS                    = getByteCode(desc.hs);
    psoDesc.DS                    = getByteCode(desc.ds);
    psoDesc.GS                    = getByteCode(desc.gs);
    psoDesc.BlendState            = blendDesc;
    psoDesc.SampleMask            = desc.sample_mask;
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.InputLayout           = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
    psoDesc.PrimitiveTopologyType = toTopologyType(desc.pt);
    psoDesc.NumRenderTargets      = desc.rtv_count;
    for (UINT i = 0; i < 8u; ++i)
        psoDesc.RTVFormats[i] = (i < desc.rtv_count) ? ToDxgiFormat(desc.rtv_formats[i]) : DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat             = ToDxgiFormat(desc.dsv_format);
    const UINT sampleCount = desc.sample_count ? desc.sample_count : 1;
    psoDesc.SampleDesc.Count      = sampleCount;
    psoDesc.SampleDesc.Quality    = 0;  // 0 required when Count==1; for MSAA use device CheckFeatureSupport

    // Compute a stable name for this PSO (used by PipelineLibrary cache)
    wchar_t psoName[64] = {};
    ComputePSOName(desc, psoName, 64);

    // Try loading from PSO library (avoids ISA recompilation on warm start)
    bool loadedFromLibrary = false;
    if (m_psoLibrary)
    {
        HRESULT hrLoad = m_psoLibrary->LoadGraphicsPipeline(psoName, &psoDesc,
                                                             IID_PPV_ARGS(&internal.pso));
        if (SUCCEEDED(hrLoad))
            loadedFromLibrary = true;
        // E_INVALIDARG = not in cache (miss or bytecode mismatch), E_FAIL = incompatible —
        // both are expected on first use; fall through to CreateGraphicsPipelineState.
    }

    if (!loadedFromLibrary)
    {
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&internal.pso));
        if (FAILED(hr))
        {
            LOG_ERROR("CreatePipelineState: CreateGraphicsPipelineState failed, HRESULT=0x%08X (see D3D12 docs / WinError.h)", static_cast<unsigned>(hr));
            return false;
        }

        // Store in library for next warm start
        if (m_psoLibrary)
        {
            HRESULT hrStore = m_psoLibrary->StorePipeline(psoName, internal.pso.Get());
            if (FAILED(hrStore))
                LOG_WARNING("CreatePipelineState: StorePipeline failed (HRESULT=0x%08X)", static_cast<unsigned>(hrStore));
        }
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    outPSO.handle_id = static_cast<uint32_t>(m_psoPool.size());
    outPSO.type      = RHI::GPUResource::Type::PipelineState;
    m_psoPool.push_back(std::make_unique<PipelineState_DX12>(std::move(internal)));
    LOG_SUCCESS("CreatePipelineState: OK");
    return true;
}

// ===========================================================================
// Command recording — state
// ===========================================================================

void GraphicsDX12::BindPipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    if (!pso.IsValid()) return;
    PipelineState_DX12& p = *m_psoPool[pso.handle_id];
    auto& entry = GetPoolEntry(cmd);
    auto* cl    = entry.GetCommandList();
    cl->SetPipelineState(p.pso.Get());
    if (p.isCompute)
    {
        cl->SetComputeRootSignature(p.rootSignature.Get());
        entry.active_rootsig_compute  = p.rootSignature.Get();
    }
    else
    {
        cl->SetGraphicsRootSignature(p.rootSignature.Get());
        entry.active_rootsig_graphics = p.rootSignature.Get();
    }
    entry.active_pso = &pso;
}

void GraphicsDX12::SetPrimitiveTopology(RHI::PrimitiveTopology topology, RHI::CommandList cmd)
{
    auto d3dTopo = ToD3D12Topology(topology);
    GetPoolEntry(cmd).GetCommandList()->IASetPrimitiveTopology(d3dTopo);
    GetPoolEntry(cmd).prev_pt = d3dTopo;
}

void GraphicsDX12::SetViewport(const RHI::Viewport& vp, RHI::CommandList cmd)
{
    D3D12_VIEWPORT d{ vp.top_left_x, vp.top_left_y, vp.width, vp.height, vp.min_depth, vp.max_depth };
    GetPoolEntry(cmd).GetCommandList()->RSSetViewports(1, &d);
}

void GraphicsDX12::SetScissorRect(uint32_t left, uint32_t top,
                                  uint32_t right, uint32_t bottom,
                                  RHI::CommandList cmd)
{
    D3D12_RECT r{ static_cast<LONG>(left), static_cast<LONG>(top),
                  static_cast<LONG>(right), static_cast<LONG>(bottom) };
    GetPoolEntry(cmd).GetCommandList()->RSSetScissorRects(1, &r);
}

// ===========================================================================
// Command recording — resource binding
// ===========================================================================

void GraphicsDX12::BindConstantBuffer(const RHI::GPUBuffer& buffer,
                                      uint32_t slot, RHI::CommandList cmd)
{
    if (!buffer.IsValid() || slot >= kCBVSlotCount) return;
    if (buffer.handle_id >= m_bufferPool.size())
    {
        LOG_ERROR("BindConstantBuffer: handle_id %u out of range (pool size %zu)",
                  buffer.handle_id, m_bufferPool.size());
        return;
    }
    GPUBuffer_DX12& p = m_bufferPool[buffer.handle_id];
    if (!p.resource)
    {
        LOG_ERROR("BindConstantBuffer: buffer handle_id %u has null resource", buffer.handle_id);
        return;
    }
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slot, p.resource->GetGPUVirtualAddress());
}

// Slot-offset variant of BindConstantBuffer — lets ring-buffer owners advance
// through 256-byte aligned slots within a single CB instead of recreating one
// per frame. Used by the reflection probe capture path to upload one CB per
// face inside a 6-slot ring.
void GraphicsDX12::BindConstantBufferAtOffset(uint32_t slot,
                                               const RHI::GPUBuffer& buffer,
                                               uint64_t byteOffset,
                                               RHI::CommandList cmd)
{
    if (!buffer.IsValid() || buffer.handle_id >= m_bufferPool.size()) return;
    GPUBuffer_DX12& p = m_bufferPool[buffer.handle_id];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slot, p.resource->GetGPUVirtualAddress() + byteOffset);
}

// Phase E — bind a raw GPU VA to the per-material custom CBV slot (b8 space0).
// Separate from BindConstantBuffer because the upload-ring owner tracks VAs
// directly, not GPUBuffer handles. Safe to pass 0 as a no-op.
void GraphicsDX12::BindCustomMaterialCBV(uint64_t gpuVA, RHI::CommandList cmd)
{
    if (gpuVA == 0) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        35 /* kCustomMatCBVSlot */, gpuVA);
}

// Phase F — bind a shader-visible GPU handle to the per-material custom
// texture table (t0-t3 space3). Pass a zero handle as a no-op.
void GraphicsDX12::BindCustomMaterialTextureTable(uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(
        36 /* kCustomMatTexSlot */, h);
}

void GraphicsDX12::BindResource(const RHI::GPUResource& resource,
                                uint32_t slot, RHI::CommandList cmd)
{
    if (!resource.IsValid() || slot >= kSRVSlotCount) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();

    if (resource.type == RHI::GPUResource::Type::Texture)
    {
        Texture_DX12& tex = m_texturePool[resource.handle_id];
        if (tex.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slot, tex.srv.GetGpuHandle());
    }
    else if (resource.type == RHI::GPUResource::Type::Buffer)
    {
        GPUBuffer_DX12& buf = m_bufferPool[resource.handle_id];
        if (buf.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slot, buf.srv.GetGpuHandle());
    }
}

void GraphicsDX12::BindSampler(int descriptorIndex, uint32_t slot, RHI::CommandList cmd)
{
    if (descriptorIndex < 0 || static_cast<uint32_t>(descriptorIndex) >= m_samplerPool.size()
        || slot >= kSamplerSlotCount) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(
        kSamplerSlotBase + slot,
        m_samplerPool[static_cast<uint32_t>(descriptorIndex)].GetGpuHandle());
}

// ===========================================================================
// PVF root bindings
// ===========================================================================

void GraphicsDX12::SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                                    RHI::CommandList cmd)
{
    uint32_t values[3] = { v0, v1, v2 };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstants(
        kRootConstantsSlot, 3, values, 0);
}

void GraphicsDX12::SetRootBufferSRV(const RHI::GPUBuffer& buf,
                                    uint32_t rootSlot, RHI::CommandList cmd)
{
    if (!buf.IsValid() || buf.handle_id >= m_bufferPool.size()) return;
    GPUBuffer_DX12& p = m_bufferPool[buf.handle_id];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootShaderResourceView(
        rootSlot, p.resource->GetGPUVirtualAddress());
}

void GraphicsDX12::BindDescriptorTableGpuHandle(uint32_t rootSlot,
                                                uint64_t gpuHandle,
                                                RHI::CommandList cmd)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(rootSlot, h);
}

// ===========================================================================
// Command recording — draws + barriers
// ===========================================================================

void GraphicsDX12::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                                 uint32_t startVertex,  uint32_t startInstance,
                                 RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DrawInstanced(
        vertexCount, instanceCount, startVertex, startInstance);
}

void GraphicsDX12::DrawIndexedInstanced(uint32_t indexCount,  uint32_t instanceCount,
                                        uint32_t startIndex,  int32_t  baseVertex,
                                        uint32_t startInstance, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DrawIndexedInstanced(
        indexCount, instanceCount, startIndex, baseVertex, startInstance);
}

void GraphicsDX12::ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                                        uint64_t argBufferOffset,
                                        uint32_t maxCommands,
                                        const RHI::GPUBuffer* countBuffer,
                                        uint64_t countBufferOffset,
                                        RHI::CommandList cmd)
{
    if (!m_indirectCommandSignature || !argBuffer.IsValid()) return;
    if (argBuffer.handle_id >= m_bufferPool.size()) return;

    ID3D12Resource* argRes = m_bufferPool[argBuffer.handle_id].resource.Get();
    if (!argRes) return;

    ID3D12Resource* countRes = nullptr;
    if (countBuffer && countBuffer->IsValid() && countBuffer->handle_id < m_bufferPool.size())
        countRes = m_bufferPool[countBuffer->handle_id].resource.Get();

    GetPoolEntry(cmd).GetCommandList()->ExecuteIndirect(
        m_indirectCommandSignature.Get(),
        maxCommands,
        argRes,
        argBufferOffset,
        countRes,
        countRes ? countBufferOffset : 0);
}

void GraphicsDX12::CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                               uint64_t size, RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handle_id >= m_bufferPool.size() || dst.handle_id >= m_bufferPool.size()) return;
    ID3D12Resource* srcRes = m_bufferPool[src.handle_id].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handle_id].resource.Get();
    if (!srcRes || !dstRes || size == 0) return;
    GetPoolEntry(cmd).GetCommandList()->CopyBufferRegion(dstRes, 0, srcRes, 0, size);
}

void GraphicsDX12::CopyTextureSubresource(const RHI::Texture& src,
                                           uint32_t srcMip, uint32_t srcArraySlice,
                                           const RHI::Texture& dst,
                                           uint32_t dstMip, uint32_t dstArraySlice,
                                           RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handle_id >= m_texturePool.size() || dst.handle_id >= m_texturePool.size()) return;

    const Texture_DX12& srcEntry = m_texturePool[src.handle_id];
    const Texture_DX12& dstEntry = m_texturePool[dst.handle_id];
    ID3D12Resource* srcRes = srcEntry.resource.Get();
    ID3D12Resource* dstRes = dstEntry.resource.Get();
    if (!srcRes || !dstRes) return;

    const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dstRes->GetDesc();

    // D3D12CalcSubresource(mip, arraySlice, planeSlice, mipLevels, arraySize)
    const UINT srcSub = D3D12CalcSubresource(
        srcMip, srcArraySlice, 0, srcDesc.MipLevels, srcDesc.DepthOrArraySize);
    const UINT dstSub = D3D12CalcSubresource(
        dstMip, dstArraySlice, 0, dstDesc.MipLevels, dstDesc.DepthOrArraySize);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcRes;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = srcSub;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = dstRes;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = dstSub;

    GetPoolEntry(cmd).GetCommandList()->CopyTextureRegion(
        &dstLoc, 0, 0, 0, &srcLoc, /*pSrcBox*/ nullptr);
}

// ===========================================================================
// Compute dispatch
// ===========================================================================

void GraphicsDX12::BindComputePipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    BindPipelineState(pso, cmd);
}

void GraphicsDX12::DispatchCompute(uint32_t x, uint32_t y, uint32_t z, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->Dispatch(x, y, z);
}

void GraphicsDX12::DispatchMesh(uint32_t x, uint32_t y, uint32_t z, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DispatchMesh(x, y, z);
}

void GraphicsDX12::SetComputeRootCBV(uint32_t rootSlot, const RHI::GPUBuffer& cb, uint32_t byteOffset, RHI::CommandList cmd)
{
    if (!cb.IsValid()) return;
    const GPUBuffer_DX12& p = m_bufferPool[cb.handle_id];
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootConstantBufferView(
        rootSlot, p.resource->GetGPUVirtualAddress() + byteOffset);
}

void GraphicsDX12::SetComputeDescriptorTable(uint32_t rootSlot, uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootDescriptorTable(rootSlot, h);
}

uint64_t GraphicsDX12::GetTextureUAVGpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid()) return 0;
    const Texture_DX12& t = m_texturePool[tex.handle_id];
    if (!t.uav.IsValid()) return 0;
    return t.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBuffer_DX12& b = m_bufferPool[buf.handle_id];
    if (!b.uav.IsValid()) return 0;
    return b.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBuffer_DX12& b = m_bufferPool[buf.handle_id];
    if (!b.srv.IsValid()) return 0;
    return b.srv.GetGpuHandle().ptr;
}

void GraphicsDX12::SetHdrTextureState(RHI::ResourceState newState, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget) return;
    const D3D12_RESOURCE_STATES target = ToD3D12ResourceState(newState);
    if (m_hdrResourceState == target) return;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_hdrRenderTarget.Get(), m_hdrResourceState, target);
    GetPoolEntry(cmd).GetCommandList()->ResourceBarrier(1, &barrier);
    m_hdrResourceState = target;
}

void GraphicsDX12::PushBarrier(const RHI::GPUBarrier& barrier, RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    // Helper: resolve a generic GPUResource* → ID3D12Resource*
    auto resolveResource = [this](const RHI::GPUResource* res) -> ID3D12Resource*
    {
        if (!res || !res->IsValid()) return nullptr;
        if (res->type == RHI::GPUResource::Type::Texture)
            return m_texturePool[res->handle_id].resource.Get();
        if (res->type == RHI::GPUResource::Type::Buffer)
            return m_bufferPool[res->handle_id].resource.Get();
        return nullptr;
    };

    switch (barrier.type)
    {
    case RHI::GPUBarrier::Type::MEMORY:
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::UAV(
            resolveResource(barrier.memory.resource));
        cl->ResourceBarrier(1, &b);
        break;
    }
    case RHI::GPUBarrier::Type::IMAGE:
    {
        Texture_DX12& t = m_texturePool[barrier.image.texture->handle_id];
        D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.image.layout_before);
        D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.image.layout_after);
        UINT subresource = (barrier.image.mip >= 0 && barrier.image.slice >= 0)
            ? D3D12CalcSubresource(barrier.image.mip, barrier.image.slice,
                                   0, barrier.image.texture->desc.mip_levels,
                                   barrier.image.texture->desc.array_size)
            : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after, subresource);
        cl->ResourceBarrier(1, &b);
        t.state = after;
        break;
    }
    case RHI::GPUBarrier::Type::BUFFER:
    {
        GPUBuffer_DX12& buf = m_bufferPool[barrier.buffer.buffer->handle_id];
        D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.buffer.state_before);
        D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.buffer.state_after);
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(buf.resource.Get(), before, after);
        cl->ResourceBarrier(1, &b);
        buf.state = after;
        break;
    }
    case RHI::GPUBarrier::Type::ALIASING:
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Aliasing(
            resolveResource(barrier.aliasing.resource_before),
            resolveResource(barrier.aliasing.resource_after));
        cl->ResourceBarrier(1, &b);
        break;
    }
    }
}

// ===========================================================================
// New IGraphicsDevice methods — render targets, heap binding, buffer mapping,
// resource destruction
// ===========================================================================

void GraphicsDX12::SetRenderTargets(uint32_t numRTs,
                                     const RHI::Texture* const* rtvs,
                                     const RHI::Texture*        dsv,
                                     RHI::CommandList           cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    for (uint32_t i = 0; i < numRTs && i < 8; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[rtvs[i]->handle_id].rtv.GetCpuHandle();
    }

    if (dsv && dsv->IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::SetRenderTargetsAndHdr(uint32_t numRTs,
                                           const RHI::Texture* const* rtvs,
                                           const RHI::Texture*        dsv,
                                           RHI::CommandList           cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;

    // Transition HDR to RENDER_TARGET if needed (mirrors SetRenderTargetToHdr).
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Build RTV array: caller-supplied RTs first, HDR appended last. Cap at
    // D3D12's 8-RT limit (numRTs+1 must be ≤ 8).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    const uint32_t capped = (numRTs > 7) ? 7u : numRTs;
    for (uint32_t i = 0; i < capped; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[rtvs[i]->handle_id].rtv.GetCpuHandle();
    }
    rtvHandles[capped] = m_hdrRtvAllocation.GetCpuHandle();
    const uint32_t totalRTs = capped + 1;

    if (dsv && dsv->IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::ClearRenderTarget(const RHI::Texture& texture,
                                      const float         color[4],
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearRenderTargetView(
        m_texturePool[texture.handle_id].rtv.GetCpuHandle(), color, 0, nullptr);
}

void GraphicsDX12::ClearHdrRenderTarget(const float      color[4],
                                         RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    // ClearRenderTargetView requires the resource in RENDER_TARGET state.
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Match the texture's clear-value contract (set at creation: 0,0,0,0).
    // Pass user-supplied colour through; D3D validation will warn if it
    // diverges from the optimised clear value but won't fail.
    cl->ClearRenderTargetView(m_hdrRtvAllocation.GetCpuHandle(), color, 0, nullptr);
}

void GraphicsDX12::ClearDepthStencil(const RHI::Texture& texture,
                                      float               depth,
                                      uint8_t             stencil,
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearDepthStencilView(
        m_texturePool[texture.handle_id].dsv.GetCpuHandle(),
        D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetDepthStencilSlice(const RHI::Texture& depthTex,
                                         uint32_t            arraySlice,
                                         RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handle_id];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = tex.sliceDsvs[arraySlice].GetCpuHandle();
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
}

void GraphicsDX12::ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                           uint32_t            arraySlice,
                                           float               depth,
                                           uint8_t             stencil,
                                           RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handle_id];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearDepthStencilView(tex.sliceDsvs[arraySlice].GetCpuHandle(),
                              D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetStencilRef(uint32_t ref, RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->OMSetStencilRef(ref);
}

void GraphicsDX12::SetGraphicsRootConstant(uint32_t rootSlot,
                                            uint32_t value,
                                            uint32_t offsetInWords,
                                            RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->SetGraphicsRoot32BitConstant(rootSlot, value, offsetInWords);
}

uint64_t GraphicsDX12::GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                                  uint32_t mip) const
{
    if (!tex.IsValid()) return 0;
    const auto& entry = m_texturePool[tex.handle_id];
    if (mip >= entry.mipUavs.size()) return 0;
    if (!entry.mipUavs[mip].IsValid()) return 0;
    return entry.mipUavs[mip].GetGpuHandle().ptr;
}

// ---- Lazy per-cube descriptor views (reflection probe capture / prefilter) --
// These methods mutate the texture entry on first request to allocate a CPU
// descriptor in the appropriate heap. Returning a handle is constant-time on
// subsequent calls.
uint64_t GraphicsDX12::GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                                       uint32_t cubeIdx,
                                                       uint32_t face,
                                                       uint32_t mip)
{
    if (!tex.IsValid() || face >= 6) return 0;
    auto& entry = m_texturePool[tex.handle_id];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 16) | (face << 8) | mip;
    auto it = entry.cubeFaceRtvs.find(key);
    if (it != entry.cubeFaceRtvs.end())
        return it->second.GetCpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t arraySlice = cubeIdx * 6u + face;
    if (arraySlice >= rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_rtvAllocator.Allocate(1);
    if (!alloc.IsValid()) return 0;

    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = rd.Format;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvd.Texture2DArray.MipSlice        = mip;
    rtvd.Texture2DArray.FirstArraySlice = arraySlice;
    rtvd.Texture2DArray.ArraySize       = 1;
    rtvd.Texture2DArray.PlaneSlice      = 0;
    m_device->CreateRenderTargetView(entry.resource.Get(), &rtvd, alloc.GetCpuHandle());

    auto [emplaceIt, _ok] = entry.cubeFaceRtvs.emplace(key, alloc);
    return emplaceIt->second.GetCpuHandle().ptr;
}

uint64_t GraphicsDX12::GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                                      uint32_t cubeIdx,
                                                      uint32_t mip)
{
    if (!tex.IsValid()) return 0;
    auto& entry = m_texturePool[tex.handle_id];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 8) | mip;
    auto it = entry.cubeMipUavs.find(key);
    if (it != entry.cubeMipUavs.end())
        return it->second.GetGpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t firstSlice = cubeIdx * 6u;
    if (firstSlice + 6u > rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(1);
    if (!alloc.IsValid()) return 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = rd.Format;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uavd.Texture2DArray.MipSlice        = mip;
    uavd.Texture2DArray.FirstArraySlice = firstSlice;
    uavd.Texture2DArray.ArraySize       = 6;
    uavd.Texture2DArray.PlaneSlice      = 0;
    m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd, alloc.GetCpuHandle());
    alloc.CopyToGpu();

    auto [emplaceIt, _ok] = entry.cubeMipUavs.emplace(key, alloc);
    return emplaceIt->second.GetGpuHandle().ptr;
}


uint32_t GraphicsDX12::BeginGPUTimestamp(RHI::CommandList cmd, const char* name)
{
    if (!m_gpuProfiler.enabled) return ~0u;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return ~0u;
    return m_gpuProfiler.BeginTimestamp(cl, name);
}

void GraphicsDX12::EndGPUTimestamp(RHI::CommandList cmd, uint32_t regionIndex)
{
    if (regionIndex == ~0u) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    m_gpuProfiler.EndTimestamp(cl, regionIndex);
}

void GraphicsDX12::BindDescriptorHeaps(RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    ID3D12DescriptorHeap* heaps[] =
    {
        m_cbvSrvUavAllocator.GetHeap(),
        m_samplerAllocator.GetHeap()
    };
    cl->SetDescriptorHeaps(2, heaps);
}

void* GraphicsDX12::MapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return nullptr;
    void* ptr = nullptr;
    D3D12_RANGE rr{ 0, 0 };
    m_bufferPool[buffer.handle_id].resource->Map(0, &rr, &ptr);
    return ptr;
}

void GraphicsDX12::UnmapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return;
    m_bufferPool[buffer.handle_id].resource->Unmap(0, nullptr);
}

void GraphicsDX12::CopyTexturePixelToBuffer(const RHI::Texture& src,
                                             uint32_t srcX, uint32_t srcY,
                                             RHI::GPUBuffer& dst,
                                             RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    Texture_DX12&  srcEntry = m_texturePool[src.handle_id];
    GPUBuffer_DX12& dstEntry = m_bufferPool[dst.handle_id];

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcEntry.resource.Get();
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource                          = dstEntry.resource.Get();
    dstLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset             = 0;
    dstLoc.PlacedFootprint.Footprint.Format   = ToDxgiFormat(src.desc.format);
    dstLoc.PlacedFootprint.Footprint.Width    = 1;
    dstLoc.PlacedFootprint.Footprint.Height   = 1;
    dstLoc.PlacedFootprint.Footprint.Depth    = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    const D3D12_BOX srcBox = { srcX, srcY, 0u, srcX + 1u, srcY + 1u, 1u };
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
}

void GraphicsDX12::DestroyBuffer(RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return;
    const uint32_t id = buffer.handle_id;
    auto& entry = m_bufferPool[id];
    // Queue resource + descriptors for release once the GPU is done with this
    // backbuffer slot (FrameCount frames from now, processed in BeginFrame).
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& dr = m_deferredRelease[m_frameIndex];
    if (entry.cbv.IsValid()) { dr.descriptors.push_back(entry.cbv); entry.cbv = {}; }
    if (entry.srv.IsValid()) { dr.descriptors.push_back(entry.srv); entry.srv = {}; }
    if (entry.resource)      { dr.resources.push_back(std::move(entry.resource)); }
    dr.bufferSlots.push_back(id);
    buffer.Reset();
}

void GraphicsDX12::DestroyTexture(RHI::Texture& texture)
{
    if (!texture.IsValid()) return;
    const uint32_t id = texture.handle_id;
    auto& entry = m_texturePool[id];
    // Queue everything for deferred release (GPU may still be reading this
    // texture / its descriptors for up to FrameCount frames).
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& dr = m_deferredRelease[m_frameIndex];
    auto steal = [&](DescriptorAllocation& a) {
        if (a.IsValid()) { dr.descriptors.push_back(a); a = {}; }
    };
    steal(entry.rtv);
    steal(entry.dsv);
    steal(entry.srv);
    steal(entry.uav);
    steal(entry.previewSrv);
    for (auto& a : entry.mipUavs)   steal(a);
    for (auto& a : entry.sliceDsvs) steal(a);
    for (auto& kv : entry.cubeFaceRtvs) steal(kv.second);
    for (auto& kv : entry.cubeMipUavs)  steal(kv.second);
    entry.mipUavs.clear();
    entry.sliceDsvs.clear();
    entry.cubeFaceRtvs.clear();
    entry.cubeMipUavs.clear();
    if (entry.resource) dr.resources.push_back(std::move(entry.resource));
    dr.textureSlots.push_back(id);
    texture.Reset();
}

// ===========================================================================
// PSO Library — disk-based ISA cache
// ===========================================================================

bool GraphicsDX12::InitPSOLibrary(const char* cacheFilePath)
{
    // Try to get ID3D12Device1 (required for CreatePipelineLibrary)
    ComPtr<ID3D12Device1> device1;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        LOG_WARNING("InitPSOLibrary: ID3D12Device1 not available — PSO library disabled");
        return false;
    }

    // Read existing cache from disk (optional — graceful if missing)
    const void* pData    = nullptr;
    SIZE_T      dataSize = 0;

    if (cacheFilePath && cacheFilePath[0] != '\0')
    {
        std::ifstream f(cacheFilePath, std::ios::binary | std::ios::ate);
        if (f.is_open())
        {
            const auto sz = f.tellg();
            f.seekg(0, std::ios::beg);
            m_psoLibraryData.resize(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(m_psoLibraryData.data()), sz);
            if (f.good())
            {
                pData    = m_psoLibraryData.data();
                dataSize = m_psoLibraryData.size();
            }
            else
            {
                m_psoLibraryData.clear();
            }
        }
    }

    HRESULT hr = device1->CreatePipelineLibrary(pData, dataSize, IID_PPV_ARGS(&m_psoLibrary));
    if (FAILED(hr))
    {
        // D3D12_ERROR_DRIVER_VERSION_MISMATCH or D3D12_ERROR_ADAPTER_NOT_FOUND
        // means the cached data is stale — start fresh.
        LOG_WARNING("InitPSOLibrary: CreatePipelineLibrary failed (0x%08X), starting empty library",
                    static_cast<unsigned>(hr));
        m_psoLibraryData.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_psoLibrary));
        if (FAILED(hr))
        {
            LOG_WARNING("InitPSOLibrary: could not create empty library (0x%08X) — PSO library disabled",
                        static_cast<unsigned>(hr));
            m_psoLibrary = nullptr;
            return false;
        }
    }

    LOG_INFO("InitPSOLibrary: PSO library ready (%zu bytes from disk)", dataSize);
    return true;
}

void GraphicsDX12::SavePSOLibrary(const char* cacheFilePath)
{
    if (!m_psoLibrary || !cacheFilePath || cacheFilePath[0] == '\0')
        return;

    const SIZE_T serializedSize = m_psoLibrary->GetSerializedSize();
    if (serializedSize == 0)
        return;

    std::vector<uint8_t> buf(serializedSize);
    HRESULT hr = m_psoLibrary->Serialize(buf.data(), serializedSize);
    if (FAILED(hr))
    {
        LOG_ERROR("SavePSOLibrary: Serialize failed (0x%08X)", static_cast<unsigned>(hr));
        return;
    }

    std::ofstream f(cacheFilePath, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
    {
        LOG_ERROR("SavePSOLibrary: cannot open '%s' for writing", cacheFilePath);
        return;
    }
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    if (f.good())
        LOG_INFO("SavePSOLibrary: saved %zu bytes to '%s'", serializedSize, cacheFilePath);
    else
        LOG_ERROR("SavePSOLibrary: write error for '%s'", cacheFilePath);
}

// ===========================================================================
// CaptureTextureToPNG — synchronous read-back of a 2D R8G8B8A8 texture into
// a PNG file. Targets ShaderLab's "Capture Frame" button.
// ===========================================================================

#include "DirectXTex.h"

bool GraphicsDX12::CaptureTextureToPNG(const RHI::Texture& tex,
                                       RHI::ResourceState  currentState,
                                       const char*         path)
{
    if (!path || !*path)
    {
        LOG_ERROR("CaptureTextureToPNG: null/empty path");
        return false;
    }

    ID3D12Resource* res = GetTextureResource(tex);
    if (!res)
    {
        LOG_ERROR("CaptureTextureToPNG: source texture has no D3D12 resource");
        return false;
    }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        LOG_ERROR("CaptureTextureToPNG: only Texture2D supported (got dim=%d)",
                  desc.Dimension);
        return false;
    }
    // Limit to formats DirectXTex SaveToWICFile + PNG codec accept directly.
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        LOG_ERROR("CaptureTextureToPNG: unsupported format %d", desc.Format);
        return false;
    }

    // Compute footprint for a single-subresource readback.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   numRows      = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalSize    = 0;
    m_device->GetCopyableFootprints(&desc, 0, 1, 0,
                                    &footprint, &numRows, &rowSizeBytes, &totalSize);

    // READBACK buffer for the GPU → CPU copy.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width              = totalSize;
        bd.Height             = 1;
        bd.DepthOrArraySize   = 1;
        bd.MipLevels          = 1;
        bd.Format             = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count   = 1;
        bd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readback))))
        {
            LOG_ERROR("CaptureTextureToPNG: readback buffer alloc failed");
            return false;
        }
    }

    // Make sure prior submissions touching the source are done so the state
    // we transition from matches what the renderer last left.
    FlushAndWait();

    // One-shot command list dedicated to the copy. Cheap (no allocator reuse
    // since this only runs on a tools button press).
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>  cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))))
    {
        LOG_ERROR("CaptureTextureToPNG: command allocator alloc failed");
        return false;
    }
    if (FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    {
        LOG_ERROR("CaptureTextureToPNG: command list alloc failed");
        return false;
    }

    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);

    // Transition: from caller-provided state → COPY_SOURCE
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource         = res;
        src.Type              = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex  = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource         = readback.Get();
        dst.Type              = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint   = footprint;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Transition back so the renderer's tracker stays consistent.
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = fromState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Block until the copy is done — synchronous read-back is the whole point.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        LOG_ERROR("CaptureTextureToPNG: fence alloc failed");
        return false;
    }
    m_commandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    // Map readback + densify rows (footprint.RowPitch is 256-aligned, the
    // DirectXTex Image we hand to SaveToWICFile wants a tight rowPitch).
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalSize) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        LOG_ERROR("CaptureTextureToPNG: readback Map failed");
        return false;
    }

    const uint64_t denseRowPitch = rowSizeBytes;
    const uint64_t denseSize     = denseRowPitch * static_cast<uint64_t>(numRows);
    std::vector<uint8_t> dense(static_cast<size_t>(denseSize));
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped);
    for (UINT row = 0; row < numRows; ++row)
    {
        std::memcpy(dense.data() + row * denseRowPitch,
                    srcBytes + row * footprint.Footprint.RowPitch,
                    static_cast<size_t>(denseRowPitch));
    }

    D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    // Save via DirectXTex. Convert path UTF-8 → wide for SaveToWICFile.
    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) { wpath.resize(wlen - 1); MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen); }
    }

    // Make sure parent dir exists — most users will pick captures/foo.png.
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(wpath).parent_path(), ec);
    }

    DirectX::Image img{};
    img.width      = footprint.Footprint.Width;
    img.height     = footprint.Footprint.Height;
    img.format     = footprint.Footprint.Format;
    img.rowPitch   = static_cast<size_t>(denseRowPitch);
    img.slicePitch = static_cast<size_t>(denseSize);
    img.pixels     = dense.data();

    const HRESULT hr = DirectX::SaveToWICFile(
        img, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
        wpath.c_str());
    if (FAILED(hr))
    {
        LOG_ERROR("CaptureTextureToPNG: SaveToWICFile failed (hr=0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }

    LOG_SUCCESS("CaptureTextureToPNG: saved '%s' (%ux%u)",
                path, img.width, img.height);
    return true;
}

void GraphicsDX12::ComputePSOName(const RHI::PipelineStateDesc& desc,
                                   wchar_t* outName, size_t maxChars) const
{
    // If a stable cache_key was provided (set by PSOCache using ShaderID-based hash),
    // use it directly — this is deterministic across runs regardless of pool indices.
    if (desc.cache_key != 0)
    {
        swprintf_s(outName, maxChars, L"PSO_%016llX",
                   static_cast<unsigned long long>(desc.cache_key));
        return;
    }

    // Fallback for callers that didn't set cache_key (compute / post
    // pipelines, DebugWirePass, etc.): per-field hash over shader BYTECODE
    // hashes + render-state SCALAR fields. Prior versions did
    // `mix(desc.rs, sizeof(*desc.rs))`, which folded in struct padding bytes
    // — those are NOT zero-initialised on PSO descs constructed on the stack,
    // so cache_key was effectively random and pso_cache.bin grew unboundedly.
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = 14695981039346656037ull;

    auto mixU8  = [&h](uint8_t v)  { h ^= v; h *= kPrime; };
    auto mixU32 = [&](uint32_t v) {
        mixU8(static_cast<uint8_t>(v));
        mixU8(static_cast<uint8_t>(v >>  8));
        mixU8(static_cast<uint8_t>(v >> 16));
        mixU8(static_cast<uint8_t>(v >> 24));
    };
    auto mixU64 = [&](uint64_t v) {
        mixU32(static_cast<uint32_t>(v));
        mixU32(static_cast<uint32_t>(v >> 32));
    };
    auto mixI32  = [&](int32_t v) { mixU32(static_cast<uint32_t>(v)); };
    auto mixF32  = [&](float   v) { uint32_t u; std::memcpy(&u, &v, 4); mixU32(u); };
    auto mixBool = [&](bool    v) { mixU8(v ? 1 : 0); };
    auto mixEnum = [&](auto    v) { mixU32(static_cast<uint32_t>(v)); };

    auto mixShader = [&](const RHI::Shader* s)
    {
        if (!s) { mixU64(0); return; }
        if (s->handle_id < m_shaderPool.size())
            mixU64(m_shaderPool[s->handle_id].bytecodeHash);
        else
            mixU64(0);
    };
    mixShader(desc.vs);
    mixShader(desc.ps);
    mixShader(desc.cs);
    mixShader(desc.gs);
    mixShader(desc.hs);
    mixShader(desc.ds);

    if (desc.rs)
    {
        const auto& rs = *desc.rs;
        mixEnum(rs.fill_mode);
        mixEnum(rs.cull_mode);
        mixBool(rs.front_counter_clockwise);
        mixI32 (rs.depth_bias);
        mixF32 (rs.depth_bias_clamp);
        mixF32 (rs.slope_scaled_depth_bias);
        mixBool(rs.depth_clip_enable);
        mixBool(rs.multisample_enable);
        mixBool(rs.antialiased_line_enable);
        mixBool(rs.conservative_rasterization_enable);
        mixU32 (rs.forced_sample_count);
    }
    if (desc.dss)
    {
        const auto& ds = *desc.dss;
        mixBool(ds.depth_enable);
        mixEnum(ds.depth_write_mask);
        mixEnum(ds.depth_func);
        mixBool(ds.stencil_enable);
        mixU8  (ds.stencil_read_mask);
        mixU8  (ds.stencil_write_mask);
        auto mixOp = [&](const RHI::DepthStencilState::DepthStencilOp& op) {
            mixEnum(op.stencil_fail_op);
            mixEnum(op.stencil_depth_fail_op);
            mixEnum(op.stencil_pass_op);
            mixEnum(op.stencil_func);
        };
        mixOp  (ds.front_face);
        mixOp  (ds.back_face);
        mixBool(ds.depth_bounds_test_enable);
    }
    if (desc.bs)
    {
        const auto& bs = *desc.bs;
        mixBool(bs.alpha_to_coverage_enable);
        mixBool(bs.independent_blend_enable);
        for (uint32_t i = 0; i < 8; ++i)
        {
            const auto& rt = bs.render_target[i];
            mixBool(rt.blend_enable);
            mixEnum(rt.src_blend);
            mixEnum(rt.dest_blend);
            mixEnum(rt.blend_op);
            mixEnum(rt.src_blend_alpha);
            mixEnum(rt.dest_blend_alpha);
            mixEnum(rt.blend_op_alpha);
            mixEnum(rt.render_target_write_mask);
        }
    }

    mixU32 (desc.rtv_count);
    mixEnum(desc.dsv_format);
    for (uint32_t i = 0; i < desc.rtv_count && i < 8; ++i)
        mixEnum(desc.rtv_formats[i]);
    mixU32 (desc.sample_count);

    swprintf_s(outName, maxChars, L"PSO_%016llX", static_cast<unsigned long long>(h));
}
