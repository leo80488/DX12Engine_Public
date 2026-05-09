#include "Graphics/Raytracing.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include "d3dx12_core.h"
#include "d3dx12_state_object.h"

#include <algorithm>
#include <cstring>

namespace RT
{

// =============================================================================
// Internal helpers
// =============================================================================

namespace
{

constexpr uint32_t kSBTRecordAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64
constexpr uint32_t kShaderIdentSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;       // 32

// Round @p value up to the next multiple of @p align (align must be POT).
constexpr uint64_t AlignTo(uint64_t value, uint64_t align)
{ return (value + align - 1) & ~(align - 1); }

// Allocate a DEFAULT-heap buffer with the given size, flags and initial state.
// Used internally to provision AS result/scratch buffers — these resources are
// not registered in the engine's RHI buffer pool because their lifecycle
// (frame-stable, GPU-only) doesn't match GPUBuffer's.
bool AllocateD3DBuffer(GraphicsDX12& gfx,
                      uint64_t sizeBytes,
                      D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES initialState,
                      Microsoft::WRL::ComPtr<ID3D12Resource>& out)
{
    ID3D12Device* device = gfx.GetDevice();
    if (!device || sizeBytes == 0) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment          = 0;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = flags;

    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        initialState, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr))
    {
        LOG_ERROR("RT::AllocateD3DBuffer: CreateCommittedResource failed (size=%llu, hr=0x%08X)",
                  (unsigned long long)sizeBytes, (unsigned)hr);
        return false;
    }
    return true;
}

// UPLOAD heap variant used for the per-frame TLAS instance buffer.
bool AllocateUploadBuffer(GraphicsDX12& gfx,
                          uint64_t sizeBytes,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& out)
{
    ID3D12Device* device = gfx.GetDevice();
    if (!device || sizeBytes == 0) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
    return SUCCEEDED(hr);
}

// Translate caller's GeometryDesc[] into D3D12_RAYTRACING_GEOMETRY_DESC[].
// The output vector references caller memory — keep it alive across the
// build call (the engine API is synchronous so the temp is fine).
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> TranslateGeometryArray(
    const GeometryDesc* geoms, uint32_t count)
{
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const GeometryDesc& g = geoms[i];
        D3D12_RAYTRACING_GEOMETRY_DESC d{};
        d.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                            : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

        auto& tri = d.Triangles;
        tri.VertexFormat                = DXGI_FORMAT_R32G32B32_FLOAT;
        tri.VertexCount                 = g.vertexCount;
        tri.VertexBuffer.StartAddress   = g.positionVA;
        tri.VertexBuffer.StrideInBytes  = g.positionStride;
        if (g.indexBufferVA && g.indexCount)
        {
            tri.IndexBuffer  = g.indexBufferVA;
            tri.IndexCount   = g.indexCount;
            tri.IndexFormat  = g.indexFormat;
        }
        else
        {
            tri.IndexBuffer = 0; tri.IndexCount = 0;
            tri.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
        // Transform3x4 left null — instance-level transforms come from TLAS.
        out.push_back(d);
    }
    return out;
}

} // anonymous namespace

// =============================================================================
// BLAS API
// =============================================================================

bool QueryBLASBuildSize(GraphicsDX12& gfx,
                        const GeometryDesc* geoms, uint32_t geomCount,
                        uint64_t& outResultSize, uint64_t& outScratchSize)
{
    outResultSize = 0; outScratchSize = 0;
    if (!gfx.SupportsDXR() || geomCount == 0) return false;

    ID3D12Device5* dev5 = gfx.GetDevice5();
    if (!dev5) return false;

    auto geoArr = TranslateGeometryArray(geoms, geomCount);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs       = geomCount;
    inputs.pGeometryDescs = geoArr.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    outResultSize  = AlignTo(info.ResultDataMaxSizeInBytes,
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    outScratchSize = AlignTo(info.ScratchDataSizeInBytes,
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    return outResultSize > 0;
}

bool AllocateBLAS(GraphicsDX12& gfx, uint64_t resultSize, BLAS& outBlas)
{
    if (!gfx.SupportsDXR() || resultSize == 0) return false;
    outBlas.sizeBytes = resultSize;
    return AllocateD3DBuffer(gfx, resultSize,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                             outBlas.resource);
}

bool AllocateScratch(GraphicsDX12& gfx, uint64_t sizeBytes, ScratchBuffer& outScratch)
{
    if (sizeBytes == 0) return false;
    outScratch.sizeBytes = sizeBytes;
    return AllocateD3DBuffer(gfx, sizeBytes,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COMMON,
                             outScratch.resource);
}

void BuildBLAS(GraphicsDX12& gfx,
               ID3D12GraphicsCommandList4* cmd,
               const GeometryDesc* geoms, uint32_t geomCount,
               BLAS& blas,
               const ScratchBuffer& scratch)
{
    if (!gfx.SupportsDXR() || !cmd || !blas.resource || !scratch.resource || geomCount == 0)
        return;

    auto geoArr = TranslateGeometryArray(geoms, geomCount);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData    = blas.resource->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch.resource->GetGPUVirtualAddress();
    desc.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.Flags                     = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    desc.Inputs.NumDescs                  = geomCount;
    desc.Inputs.pGeometryDescs            = geoArr.data();

    cmd->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    // Two UAV barriers: BLAS result (so the TLAS build can read it) AND the
    // scratch buffer (so the next BLAS build using the same scratch can't
    // race with this one's still-in-flight writes). Without the scratch
    // barrier, building N BLASes in a row produces corrupted geometry and
    // the eventual TLAS / DispatchRays hangs the GPU (TDR).
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = blas.resource.Get();
    barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = scratch.resource.Get();
    cmd->ResourceBarrier(2, barriers);
}

// =============================================================================
// TLAS API
// =============================================================================

bool AllocateTLAS(GraphicsDX12& gfx, uint32_t maxInstances, TLAS& outTlas)
{
    if (!gfx.SupportsDXR() || maxInstances == 0) return false;

    ID3D12Device5* dev5 = gfx.GetDevice5();
    if (!dev5) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // ALLOW_UPDATE permits transform refits; FAST_TRACE prioritises traversal.
    inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                       | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs    = maxInstances;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    outTlas.resultSize       = AlignTo(info.ResultDataMaxSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    outTlas.scratchSize      = AlignTo(info.ScratchDataSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    outTlas.instanceCapacity = maxInstances;
    outTlas.instanceCount    = 0;

    if (!AllocateD3DBuffer(gfx, outTlas.resultSize,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                           outTlas.resource))
        return false;

    const uint64_t instUploadBytes = uint64_t(maxInstances)
                                   * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!AllocateUploadBuffer(gfx, instUploadBytes, outTlas.instanceUploadBuffer))
        return false;

    return true;
}

uint32_t WriteTLASInstances(TLAS& tlas, const TLASInstance* instances, uint32_t count)
{
    if (!tlas.instanceUploadBuffer || tlas.instanceCapacity == 0) return 0;

    const uint32_t writeCount = std::min(count, tlas.instanceCapacity);

    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(tlas.instanceUploadBuffer->Map(0, &noRead, &mapped)) || !mapped)
        return 0;

    auto* dst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mapped);
    for (uint32_t i = 0; i < writeCount; ++i)
    {
        const TLASInstance& src = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        std::memcpy(d.Transform, src.transform, sizeof(d.Transform));
        d.InstanceID                          = src.instanceID & 0x00FFFFFFu;
        d.InstanceMask                        = src.instanceMask & 0xFFu;
        d.InstanceContributionToHitGroupIndex = src.hitGroupIndex & 0x00FFFFFFu;
        d.Flags                               = src.flags;
        d.AccelerationStructure               = src.blasVA;
        dst[i] = d;
    }
    tlas.instanceUploadBuffer->Unmap(0, nullptr);

    tlas.instanceCount = writeCount;
    return writeCount;
}

void BuildTLAS(GraphicsDX12& gfx,
               ID3D12GraphicsCommandList4* cmd,
               TLAS& tlas,
               const ScratchBuffer& scratch,
               bool perform_update)
{
    if (!gfx.SupportsDXR() || !cmd || !tlas.resource || !scratch.resource) return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData    = tlas.resource->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch.resource->GetGPUVirtualAddress();
    desc.Inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (perform_update)
    {
        desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        desc.SourceAccelerationStructureData = tlas.resource->GetGPUVirtualAddress();
    }
    desc.Inputs.NumDescs    = tlas.instanceCount;
    desc.Inputs.InstanceDescs = tlas.instanceUploadBuffer->GetGPUVirtualAddress();

    cmd->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = tlas.resource.Get();
    cmd->ResourceBarrier(1, &barrier);
}

// =============================================================================
// Pipeline (state object + SBT)
// =============================================================================

bool CreatePipeline(GraphicsDX12& gfx, const PipelineDesc& desc, Pipeline& outPipeline)
{
    if (!gfx.SupportsDXR()) return false;
    if (!desc.dxilBytecode || desc.dxilSize == 0)
    {
        LOG_ERROR("RT::CreatePipeline: empty DXIL bytecode");
        return false;
    }
    ID3D12Device5* dev5 = gfx.GetDevice5();
    if (!dev5) return false;

    CD3DX12_STATE_OBJECT_DESC pipelineDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    // ---- DXIL library subobject ---------------------------------------------
    auto* lib = pipelineDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE bc{};
    bc.pShaderBytecode = desc.dxilBytecode;
    bc.BytecodeLength  = desc.dxilSize;
    lib->SetDXILLibrary(&bc);
    // Export everything in the library so DDGIRayGen / DDGIMiss / DDGIClosestHit
    // can be referenced by name. We don't bother renaming exports.
    lib->DefineExport(desc.rayGenName);
    lib->DefineExport(desc.missName);
    lib->DefineExport(desc.closestHitName);

    // ---- Hit group ----------------------------------------------------------
    auto* hg = pipelineDesc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hg->SetClosestHitShaderImport(desc.closestHitName);
    hg->SetHitGroupExport(desc.hitGroupName);
    hg->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // ---- Shader config ------------------------------------------------------
    auto* sc = pipelineDesc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    sc->Config(desc.maxPayloadSizeBytes, desc.maxAttribSizeBytes);

    // ---- Pipeline config ----------------------------------------------------
    auto* pc = pipelineDesc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pc->Config(desc.maxRecursionDepth);

    // ---- Global root signature ----------------------------------------------
    if (!desc.globalRootSig)
    {
        LOG_ERROR("RT::CreatePipeline: missing global root signature");
        return false;
    }
    auto* rs = pipelineDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    rs->SetRootSignature(desc.globalRootSig.Get());

    HRESULT hr = dev5->CreateStateObject(pipelineDesc, IID_PPV_ARGS(&outPipeline.stateObject));
    if (FAILED(hr))
    {
        LOG_ERROR("RT::CreatePipeline: CreateStateObject failed (hr=0x%08X)", (unsigned)hr);
        return false;
    }
    if (FAILED(outPipeline.stateObject.As(&outPipeline.properties)))
    {
        LOG_ERROR("RT::CreatePipeline: ID3D12StateObjectProperties QI failed");
        return false;
    }

    outPipeline.globalRootSig = desc.globalRootSig;

    // ---- SBT — three records (raygen, miss, hit) -----------------------------
    const uint32_t recordStride = (uint32_t)AlignTo(kShaderIdentSize, kSBTRecordAlign);
    outPipeline.sbtRecordStride = recordStride;
    const uint64_t sbtBytes = uint64_t(recordStride) * 3;

    Microsoft::WRL::ComPtr<ID3D12Resource> sbtUpload;
    if (!AllocateUploadBuffer(gfx, sbtBytes, sbtUpload))
    {
        LOG_ERROR("RT::CreatePipeline: SBT upload buffer alloc failed");
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(sbtUpload->Map(0, &noRead, &mapped)) || !mapped)
    {
        LOG_ERROR("RT::CreatePipeline: SBT map failed");
        return false;
    }
    auto writeRec = [&](uint32_t slot, const wchar_t* exportName)
    {
        void* id = outPipeline.properties->GetShaderIdentifier(exportName);
        if (!id)
        {
            LOG_ERROR("RT::CreatePipeline: missing shader identifier for export");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(mapped) + slot * recordStride,
                    id, kShaderIdentSize);
    };
    writeRec(0, desc.rayGenName);
    writeRec(1, desc.missName);
    writeRec(2, desc.hitGroupName);
    sbtUpload->Unmap(0, nullptr);

    // SBT lives in UPLOAD heap. DispatchRays reads it through the GPU; this is
    // safe (well, supported) for SBTs because the data is small and read once
    // per dispatch. A dedicated DEFAULT-heap copy would shave a few cycles but
    // adds barrier ceremony; keep the upload buffer for now.
    outPipeline.sbtBuffer  = sbtUpload;
    outPipeline.rayGenVA   = sbtUpload->GetGPUVirtualAddress() + 0u * recordStride;
    outPipeline.missVA     = sbtUpload->GetGPUVirtualAddress() + 1u * recordStride;
    outPipeline.hitGroupVA = sbtUpload->GetGPUVirtualAddress() + 2u * recordStride;

    LOG_INFO("RT::CreatePipeline: state object + SBT ready (stride=%u, bytes=%llu)",
             recordStride, (unsigned long long)sbtBytes);
    return true;
}

void DispatchRays(GraphicsDX12& gfx,
                  ID3D12GraphicsCommandList4* cmd,
                  const Pipeline& pipeline,
                  uint32_t width, uint32_t height, uint32_t depth)
{
    if (!cmd || !pipeline.stateObject || width == 0 || height == 0 || depth == 0)
        return;

    D3D12_DISPATCH_RAYS_DESC desc{};
    desc.RayGenerationShaderRecord.StartAddress = pipeline.rayGenVA;
    desc.RayGenerationShaderRecord.SizeInBytes  = pipeline.sbtRecordStride;

    desc.MissShaderTable.StartAddress  = pipeline.missVA;
    desc.MissShaderTable.SizeInBytes   = pipeline.sbtRecordStride;
    desc.MissShaderTable.StrideInBytes = pipeline.sbtRecordStride;

    desc.HitGroupTable.StartAddress  = pipeline.hitGroupVA;
    desc.HitGroupTable.SizeInBytes   = pipeline.sbtRecordStride;
    desc.HitGroupTable.StrideInBytes = pipeline.sbtRecordStride;

    desc.Width  = width;
    desc.Height = height;
    desc.Depth  = depth;

    cmd->DispatchRays(&desc);
    (void)gfx;
}

void BindAndDispatchRays(GraphicsDX12& gfx,
                         ID3D12GraphicsCommandList4* cmd,
                         const Pipeline& pipeline,
                         uint32_t width, uint32_t height, uint32_t depth)
{
    if (!cmd || !pipeline.stateObject) return;
    cmd->SetPipelineState1(pipeline.stateObject.Get());
    DispatchRays(gfx, cmd, pipeline, width, height, depth);
}

} // namespace RT
