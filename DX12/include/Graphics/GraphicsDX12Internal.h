#pragma once

// Internal types + root-signature slot constants shared between the
// GraphicsDX12 source split (GraphicsDX12.cpp / _Translation / _Resources /
// _Capture). Not part of the public IGraphicsDevice surface — kept out of
// GraphicsDX12.h so callers don't see DX12 pool internals.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wrl.h>
#include <vector>
#include <unordered_map>
#include "d3d12.h"
#include "Graphics/DescriptorHeapAllocator.h"

// ---- Pool-backed resource shells (index == GPUResource::handle_id) --------
struct Texture_DX12
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES   state = D3D12_RESOURCE_STATE_COMMON;
    DescriptorAllocation    srv;        // CBV_SRV_UAV shader-visible heap (native fmt incl. SRGB)
    DescriptorAllocation    previewSrv; // UNORM alias SRV for editor preview (SRGB only)
    DescriptorAllocation    stencilSrv; // stencil-plane view (X32_TYPELESS_G8X24 / X24_TYPELESS_G8)
    // NV12 UV-plane SRV (R8G8_UNORM, PlaneSlice=1). srv above holds the
    // Y-plane (R8_UNORM PlaneSlice=0) when desc.format == NV12. Empty for
    // every non-NV12 texture.
    DescriptorAllocation    uvPlaneSrv;
    DescriptorAllocation    uav;
    DescriptorAllocation    rtv;
    DescriptorAllocation    dsv;
    // Per-mip UAVs (only when mip_levels > 1 + UAV bind flag).
    std::vector<DescriptorAllocation> mipUavs;
    // Per-array-slice DSVs (only when array_size > 1 + DEPTH_STENCIL).
    std::vector<DescriptorAllocation> sliceDsvs;
    // Lazy per-(cube, face, mip) RTVs — key = (cubeIdx<<16)|(face<<8)|mip.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeFaceRtvs;
    // Lazy per-(cube, mip) UAVs — key = (cubeIdx<<8)|mip.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeMipUavs;
};

struct GPUBuffer_DX12
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES    state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    DescriptorAllocation     cbv;
    DescriptorAllocation     srv;   // structured / typed
    DescriptorAllocation     uav;
};

struct Shader_DX12
{
    std::vector<uint8_t> bytecode;
    // Content-addressed hash for ComputePSOName fallback — keeps compute/post
    // PSO disk-library names stable across runs so pso_cache.bin stops growing.
    uint64_t             bytecodeHash = 0;
};

struct PipelineState_DX12
{
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  pso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  rootSignature;
    bool                                          isCompute = false;
};


// ---- Default (PVF) root-signature slot constants --------------------------
// See GraphicsDX12.h header banner for the full slot map. These mirror the
// root-sig layout built by CreateDefaultRootSignature() and are consumed by
// command-recording functions in GraphicsDX12.cpp.
static constexpr UINT kRootConstantsSlot   =  0;
static constexpr UINT kCBVSlotBase         =  1;   // root 1-7 → b1-b7
static constexpr UINT kCBVSlotCount        =  7;
static constexpr UINT kInstanceBufSlot     =  8;   // root 8   → t0 space0
static constexpr UINT kMeshDescSlot        =  9;   // root 9   → t1 space0
static constexpr UINT kSRVSlotBase         = 10;   // root 10-13 → t2-t5 space0
static constexpr UINT kSRVSlotCount        =  4;
static constexpr UINT kBindlessSlot        = 14;   // root 14  → t0 space1
static constexpr UINT kSamplerSlotBase     = 15;   // root 15-18 → s0-s3
static constexpr UINT kSamplerSlotCount    =  4;
static constexpr UINT kIBLSRVSlotBase      = 19;   // root 19-21 → t6-t8 space0
static constexpr UINT kIBLSRVSlotCount     =  3;
static constexpr UINT kShadowSRVSlot       = 22;   // root 22  → t9-t11 space0
static constexpr UINT kShadowSRVCount      =  3;
static constexpr UINT kClusterSRVSlotBase  = 23;   // root 23-25 → t13-t15 space0
static constexpr UINT kClusterSRVSlotCount =  3;
// Must match kMaxBuffers in MeshDescriptorHeap.h, g_Buffers[] in
// pvf_fetch.hlsli, and g_DDGIBuffers[] in DDGIRayTrace.cs.hlsl.
static constexpr UINT kMaxBindlessBuffers  = 16384;
