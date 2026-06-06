#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/DxcCompiler.h"
#include "System/Log.h"
#include "d3dx12.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// Resource creation + destruction for GraphicsDX12 — Create{Buffer,Texture,
// Shader,Sampler,PipelineState} + Destroy{Buffer,Texture} + the batch-upload
// helpers that amortise per-buffer FlushAndWait roundtrips. Split off so the
// command-recording side of GraphicsDX12.cpp stays readable.

// ===========================================================================
// Batch-upload scope helpers (CreateBuffer for DEFAULT heap defers waits)
// ===========================================================================
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

    // DPB-only NV12: REFERENCE_ONLY lets the driver use an opaque tiled
    // layout (memory saving) but forbids SRV/RTV/UAV — texture is decoder-
    // internal only. Used for back-reference frames the renderer never
    // samples directly.
    const bool isNV12 = (dxgiFmt == DXGI_FORMAT_NV12 || dxgiFmt == DXGI_FORMAT_P010);
    if (RHI::HasFlag(desc.misc_flags, RHI::ResourceMiscFlag::VIDEO_DECODE_DPB_ONLY))
    {
        rd.Flags |= D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY
                  | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }

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

    // Skip the driver's zero-init for RENDER TARGETS — they're cleared (or
    // fully written) before first read, so the zero-fill at allocation is
    // wasted bandwidth. Debug-layer validated clean across this engine's RTs.
    //
    // Deliberately NOT for DEPTH_STENCIL: a D24_S8/D32_S8 target's STENCIL plane
    // (subresource 1) is often never explicitly cleared, so CREATE_NOT_ZEROED
    // there triggers a debug-layer "subresource not initialized but used" error
    // (observed on GBuffer_Depth). Also NOT for SRV/UAV-only textures, which may
    // be sampled before their first write (history / accumulation buffers).
    //
    // Also NOT for textures that ALSO carry UNORDERED_ACCESS: these are usually
    // written first by a COMPUTE UAV pass and only later bound as an RT (e.g.
    // ToneMapPass.FinalOutput — the tone-map dispatch writes it via UAV, then
    // UIPass draws the HUD onto it as a render target). A UAV write does NOT
    // satisfy the CREATE_NOT_ZEROED rule that an RT/DS subresource be initialized
    // via Clear/Discard/Copy, so the validator flags the later RT DrawInstanced
    // as "not initialized but is used". Excluding RT+UAV textures costs only a
    // one-time zero-fill at allocation (no per-frame cost) and is debug clean.
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT
        && RHI::HasFlag(desc.bind_flags, RHI::BindFlag::RENDER_TARGET)
        && !RHI::HasFlag(desc.bind_flags, RHI::BindFlag::DEPTH_STENCIL)
        && !RHI::HasFlag(desc.bind_flags, RHI::BindFlag::UNORDERED_ACCESS))
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    if (FAILED(m_device->CreateCommittedResource(
            &hp, heapFlags, &rd, initState, pCv,
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

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE) && isNV12)
    {
        // NV12 (or P010 for 10-bit HDR) is a planar YUV format: plane 0 is
        // luma (Y), plane 1 is half-res interleaved chroma (UV). D3D12 does
        // not allow a TYPED SRV with the NV12 format itself — the caller
        // samples each plane through its own typed view. We allocate two
        // SRV descriptors back-to-back so YUV→RGB shaders can bind them as
        // a contiguous table if desired.
        entry.srv        = m_cbvSrvUavAllocator.Allocate(1); // Y plane
        entry.uvPlaneSrv = m_cbvSrvUavAllocator.Allocate(1); // UV plane

        const DXGI_FORMAT yFmt  = (dxgiFmt == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16_UNORM
                                                                : DXGI_FORMAT_R8_UNORM;
        const DXGI_FORMAT uvFmt = (dxgiFmt == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16G16_UNORM
                                                                : DXGI_FORMAT_R8G8_UNORM;

        D3D12_SHADER_RESOURCE_VIEW_DESC ySrvd{};
        ySrvd.Format                  = yFmt;
        ySrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ySrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        ySrvd.Texture2D.MipLevels     = 1;
        ySrvd.Texture2D.PlaneSlice    = 0;
        m_device->CreateShaderResourceView(entry.resource.Get(), &ySrvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();

        D3D12_SHADER_RESOURCE_VIEW_DESC uvSrvd{};
        uvSrvd.Format                  = uvFmt;
        uvSrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        uvSrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        uvSrvd.Texture2D.MipLevels     = 1;
        uvSrvd.Texture2D.PlaneSlice    = 1;
        m_device->CreateShaderResourceView(entry.resource.Get(), &uvSrvd, entry.uvPlaneSrv.GetCpuHandle());
        entry.uvPlaneSrv.CopyToGpu();
    }
    else if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE))
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

        // Stencil-plane SRV for D24_UNORM_S8_UINT depth — extra descriptor
        // viewing the same resource as DXGI_FORMAT_X24_TYPELESS_G8_UINT so
        // compute shaders (e.g. TAA) can read stencil values alongside depth.
        // Created here, never destroyed independently — freed with the texture.
        if (needsDepthSRV && dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT
            && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
            && desc.sample_count <= 1
            && desc.array_size <= 1)
        {
            entry.stencilSrv = m_cbvSrvUavAllocator.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvd{};
            stencilSrvd.Format                  = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            // D24_S8 is a multi-plane resource: plane 0 = depth (24 bits),
            // plane 1 = stencil (8 bits). Targeting the stencil view onto an
            // R24G8_TYPELESS resource therefore requires PlaneSlice = 1 — the
            // default 0 triggers an InvalidCall (device removal) at view
            // creation. The shader samples as Texture2D<uint2> and reads .y,
            // so leave Shader4ComponentMapping at the identity default.
            stencilSrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            stencilSrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            stencilSrvd.Texture2D.MipLevels     = desc.mip_levels;
            stencilSrvd.Texture2D.PlaneSlice    = 1;
            m_device->CreateShaderResourceView(entry.resource.Get(), &stencilSrvd,
                                               entry.stencilSrv.GetCpuHandle());
            entry.stencilSrv.CopyToGpu();
        }

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

        // Inside a batch scope: defer the fence wait until EndBufferUploadBatch()
        // (cap accumulated staging so a many-texture load burst doesn't OOM).
        // Textures are typically larger than vertex streams, so batching the
        // per-resource FlushAndWait stall matters even more here than on the
        // CreateBuffer path. Outside a batch, fall back to the synchronous flush.
        if (m_batchUploadDepth > 0)
        {
            m_batchStagingBytes += uploadSize;
            m_batchStagingKeepAlive.push_back(std::move(staging));
            FlushBatchStagingIfNeeded();
        }
        else
        {
            FlushUploadAndWait();
        }
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
// CreateTexturePlaced — transient 2D texture backed by a caller-owned heap
// (CreatePlacedResource) so lifetime-disjoint textures can alias heap bytes.
// Deliberately narrow: single mip/slice, SR (+optional UAV) only, no upload.
// ===========================================================================
bool GraphicsDX12::CreateTexturePlaced(const RHI::TextureDesc& desc,
                                       ID3D12Heap* heap, UINT64 heapOffset,
                                       RHI::Texture& outTexture)
{
    if (!heap) { LOG_ERROR("CreateTexturePlaced: null heap"); return false; }

    Texture_DX12 entry;
    const DXGI_FORMAT dxgiFmt = ToDxgiFormat(desc.format);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = desc.width;
    rd.Height           = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = dxgiFmt;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = ToD3D12ResourceFlags(desc.bind_flags);

    const D3D12_RESOURCE_STATES initState = ToD3D12ResourceState(desc.layout);

    if (FAILED(m_device->CreatePlacedResource(
            heap, heapOffset, &rd, initState, nullptr, IID_PPV_ARGS(&entry.resource))))
    {
        LOG_ERROR("CreateTexturePlaced: CreatePlacedResource failed (off=%llu %ux%u)",
                  (unsigned long long)heapOffset, desc.width, desc.height);
        return false;
    }
    entry.state = initState;

    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::SHADER_RESOURCE))
    {
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = dxgiFmt;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();
    }
    if (RHI::HasFlag(desc.bind_flags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format               = dxgiFmt;
        uavd.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavd.Texture2D.MipSlice   = 0;
        m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
        entry.uav.CopyToGpu();
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_textureFreeList.empty())
    {
        outTexture.handle_id = m_textureFreeList.back();
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

    // Mirror CreateTexture: publish the SRV into the bindless table slot.
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
    return true;
}

// ===========================================================================
// UpdateTexture — re-upload bytes into an existing DEFAULT-heap texture.
// Mirrors CreateTexture's initialData path; suitable for per-frame streaming
// (video playback, dynamic UI atlas). Synchronous: FlushUploadAndWait inside.
// ===========================================================================
bool GraphicsDX12::UpdateTexture(RHI::Texture& texture,
                                  const RHI::SubresourceData* planes,
                                  uint32_t subresourceCount)
{
    if (!texture.IsValid() || !planes || subresourceCount == 0) return false;
    if (texture.handle_id >= m_texturePool.size()) return false;
    Texture_DX12& entry = m_texturePool[texture.handle_id];
    if (!entry.resource) return false;

    D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const UINT actualSubresources = static_cast<UINT>(
        rd.MipLevels * rd.DepthOrArraySize);
    // For multi-plane formats (NV12/P010) D3D12 stores each plane as its own
    // subresource — total = mip_levels * array_size * planeCount. We accept
    // either subresourceCount == actual or == planeCount (NV12: 2) when the
    // caller is uploading the whole resource at mip 0 array 0.
    UINT planeCount = 1;
    if (rd.Format == DXGI_FORMAT_NV12 || rd.Format == DXGI_FORMAT_P010) planeCount = 2;
    const UINT expected = actualSubresources * planeCount;
    if (subresourceCount != expected && subresourceCount != planeCount)
    {
        LOG_ERROR("UpdateTexture: subresourceCount=%u, expected %u (or %u for plane-only)",
                  subresourceCount, expected, planeCount);
        return false;
    }

    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0,
                                     nullptr, nullptr, nullptr, &uploadSize);
    ComPtr<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
    const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_NONE, &stagingDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&staging))))
    {
        LOG_ERROR("UpdateTexture: staging buffer alloc failed");
        return false;
    }

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
        const uint8_t* srcRow = static_cast<const uint8_t*>(planes[sub].data_ptr);
        if (!srcRow) continue;
        for (UINT row = 0; row < numRows[sub]; ++row)
        {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizes[sub]));
            dstRow += fp.Footprint.RowPitch;
            srcRow += planes[sub].row_pitch;
        }
    }
    staging->Unmap(0, nullptr);

    auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->ResourceBarrier(1, &toCopy);
    for (UINT sub = 0; sub < subresourceCount; ++sub)
    {
        CD3DX12_TEXTURE_COPY_LOCATION dst(entry.resource.Get(), sub);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[sub]);
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto fromCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
    m_commandList->ResourceBarrier(1, &fromCopy);
    FlushUploadAndWait();
    return true;
}

// ===========================================================================
// CopyD3D12ResourceToTexture — synchronous GPU copy from an external
// ID3D12Resource (e.g. FFmpeg D3D12VA hwaccel decoded frame) to a managed
// RHI::Texture. Used by Mp4FrameSource hwaccel path.
// ===========================================================================
bool GraphicsDX12::CopyD3D12ResourceToTexture(RHI::Texture& dst,
                                               ID3D12Resource* src,
                                               ID3D12Fence* waitFence,
                                               UINT64 waitFenceValue)
{
    if (!dst.IsValid() || !src) return false;
    if (dst.handle_id >= m_texturePool.size()) return false;
    Texture_DX12& dstEntry = m_texturePool[dst.handle_id];
    if (!dstEntry.resource) return false;

    // GPU-side wait for the producer's fence on the graphics queue. The
    // upload CL submission below will then block GPU-side until the
    // producer (FFmpeg's video decode) has written + signaled.
    if (waitFence && waitFenceValue > 0)
        m_commandQueue->Wait(waitFence, waitFenceValue);

    // FFmpeg's D3D12VA output sits in D3D12_RESOURCE_STATE_COMMON after the
    // video decode CL completes (their docs / impl observation). We
    // transition src COMMON→COPY_SOURCE and dst current→COPY_DEST around
    // the copy, then put both back.
    const D3D12_RESOURCE_STATES srcStateBefore = D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES dstStateBefore = dstEntry.state;

    D3D12_RESOURCE_BARRIER pre[2]{};
    pre[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition.pResource   = src;
    pre[0].Transition.StateBefore = srcStateBefore;
    pre[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[1].Transition.pResource   = dstEntry.resource.Get();
    pre[1].Transition.StateBefore = dstStateBefore;
    pre[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, pre);

    // Whole-resource copy — works for NV12 because CopyResource handles all
    // subresources (Y plane + UV plane) provided format + dims match.
    m_commandList->CopyResource(dstEntry.resource.Get(), src);

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[0].Transition.pResource   = src;
    post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[0].Transition.StateAfter  = srcStateBefore;
    post[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition.pResource   = dstEntry.resource.Get();
    post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post[1].Transition.StateAfter  = dstStateBefore;
    post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, post);

    FlushUploadAndWait();
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
// Destroy buffers / textures
// ===========================================================================

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
    steal(entry.stencilSrv);
    steal(entry.uvPlaneSrv);
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
// PSO library name derivation
// ===========================================================================


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
