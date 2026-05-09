#pragma once

// Raytracing.h — DXR helper layer.
//
// Wraps the verbose D3D12 raytracing APIs (acceleration structure build,
// state-object creation, shader binding table assembly) behind a small
// RHI-style facade. Used by the DDGI probe trace pass; reusable for any
// future DXR-based work (RT reflections, RT shadows, etc.).
//
// Layering note:
//   This file lives in the DX12 backend layer because raytracing uses
//   API-specific resource types (D3D12_RAYTRACING_*) that the abstract
//   IGraphicsDevice purposefully does not expose. Passes that use this
//   helper acquire the GraphicsDX12& via static_cast (same pattern as
//   GBufferPass and friends).
//
//  Lifecycle:
//   1. Build BLAS for each static mesh you want raytraceable
//      (one-shot at scene load, cached on the mesh).
//   2. Build TLAS from instances pointing at those BLASes
//      (rebuild when static topology changes; refit when transforms move).
//   3. Create an RaytracingPipeline once, then bind + DispatchRays per frame.
//
//  Important DXR quirks the helper handles for you:
//   - Scratch buffers must be UAV-bindable, in COMMON or UAV state.
//   - AS result buffers must have initial state RAYTRACING_ACCELERATION_STRUCTURE.
//   - SBT records require a 64-byte alignment (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
//     and shader identifiers are 32 bytes each.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include <string>

class GraphicsDX12;

namespace RT
{

// One raytraceable triangle geometry (passed to BuildBLAS).
//
// Source data layout maps directly onto D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC:
//   - Position buffer is read as 3 floats per vertex with caller-supplied stride.
//   - Index buffer is optional; pass kInvalidIndexBuffer to use sequential triangle list.
struct GeometryDesc
{
    D3D12_GPU_VIRTUAL_ADDRESS positionVA = 0;     // address of vertex 0 position
    uint32_t                  positionStride = 0; // bytes between vertices
    uint32_t                  vertexCount = 0;

    // Index data — optional. When indexBufferVA == 0 the geometry is treated
    // as a non-indexed triangle list (vertexCount must be a multiple of 3).
    D3D12_GPU_VIRTUAL_ADDRESS indexBufferVA = 0;
    uint32_t                  indexCount    = 0;
    DXGI_FORMAT               indexFormat   = DXGI_FORMAT_R16_UINT; // R16 or R32

    // Marks the geometry as opaque (no any-hit). DDGI uses opaque for static
    // geometry and gets a meaningful any-hit-skip optimisation from this.
    bool opaque = true;
};

// Built BLAS — caller keeps the resource alive, passes the GPU VA into TLAS instances.
struct BLAS
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; // result buffer
    uint64_t                                sizeBytes = 0;
    D3D12_GPU_VIRTUAL_ADDRESS               GPUAddress() const
    { return resource ? resource->GetGPUVirtualAddress() : 0; }
};

// One TLAS instance (one entity in the static-only DDGI scene). Maps to
// D3D12_RAYTRACING_INSTANCE_DESC. The `transform` is row-major 3x4 (the trailing
// row [0,0,0,1] is implied).
struct TLASInstance
{
    float                       transform[3][4] = { {1,0,0,0},{0,1,0,0},{0,0,1,0} };
    uint32_t                    instanceID  = 0;        // shader-visible (24 bits)
    uint32_t                    instanceMask = 0xFF;    // ray mask — 0xFF = all
    uint32_t                    hitGroupIndex = 0;      // contribution to hit group index
    uint32_t                    flags = 0;              // D3D12_RAYTRACING_INSTANCE_FLAGS
    D3D12_GPU_VIRTUAL_ADDRESS   blasVA = 0;             // BLAS::GPUAddress()
};

// Built TLAS — owns the result buffer. Pass GPUAddress() to a SRV view of
// type RAYTRACING_ACCELERATION_STRUCTURE for shader access (slot in g_DDGITLAS).
struct TLAS
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> instanceUploadBuffer; // staged instance descs
    uint64_t                                resultSize = 0;
    uint64_t                                scratchSize = 0;
    uint32_t                                instanceCapacity = 0;
    uint32_t                                instanceCount = 0;

    D3D12_GPU_VIRTUAL_ADDRESS GPUAddress() const
    { return resource ? resource->GetGPUVirtualAddress() : 0; }
};

// Shared scratch buffer for AS builds. AS build scratch is single-use per
// queue submission; reuse the same buffer across frames as long as Build()
// finishes before the next BuildBLAS/BuildTLAS call (the helper waits for
// the previous build's fence on the build-list path).
struct ScratchBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint64_t                                sizeBytes = 0;
    D3D12_GPU_VIRTUAL_ADDRESS               GPUAddress() const
    { return resource ? resource->GetGPUVirtualAddress() : 0; }
};

// =============================================================================
// AS Build helpers (record onto a graphics or compute command list)
// =============================================================================

// Compute prebuild info for a BLAS containing @p geoms; returns false if
// the device is not DXR-capable. Populates outResultSize / outScratchSize.
bool QueryBLASBuildSize(GraphicsDX12& gfx,
                        const GeometryDesc* geoms, uint32_t geomCount,
                        uint64_t& outResultSize, uint64_t& outScratchSize);

// Allocate the BLAS result buffer (DEFAULT heap, UAV, RAYTRACING_ACCELERATION_STRUCTURE
// initial state). Does NOT record any GPU work. Caller passes the same scratch
// buffer to BuildBLAS — it must be at least outScratchSize bytes.
bool AllocateBLAS(GraphicsDX12& gfx, uint64_t resultSize, BLAS& outBlas);

// Allocate the scratch buffer (DEFAULT heap, UAV, COMMON state). Reusable.
bool AllocateScratch(GraphicsDX12& gfx, uint64_t sizeBytes, ScratchBuffer& outScratch);

// Record the BLAS build into @p cmdList (must implement ID3D12GraphicsCommandList4).
// cmdList must be a ID3D12GraphicsCommandList4 obtained from the device.
// Inserts a UAV barrier on the result buffer at the end.
void BuildBLAS(GraphicsDX12& gfx,
               ID3D12GraphicsCommandList4* cmdList,
               const GeometryDesc* geoms, uint32_t geomCount,
               BLAS& blas,
               const ScratchBuffer& scratch);

// Compute / allocate TLAS sized for @p maxInstances. Caller fills the
// instance buffer each frame and calls BuildTLAS(). Reusing the same TLAS
// resource across frames is fine — DXR only needs an explicit rebuild when
// the instance count or BLAS contents change; for transform-only updates
// the FAST_TRACE flag with PERFORM_UPDATE achieves a refit.
bool AllocateTLAS(GraphicsDX12& gfx, uint32_t maxInstances, TLAS& outTlas);

// Map-and-fill the TLAS upload buffer with up to maxInstances entries.
// Flushes the mapped range; safe to call once per frame before BuildTLAS.
// Returns the count actually written (clamped to capacity).
uint32_t WriteTLASInstances(TLAS& tlas, const TLASInstance* instances, uint32_t count);

// Record the TLAS build into @p cmdList. Pass perform_update=false for the
// first build and after any topology change, true for subsequent transform-
// only updates.
void BuildTLAS(GraphicsDX12& gfx,
               ID3D12GraphicsCommandList4* cmdList,
               TLAS& tlas,
               const ScratchBuffer& scratch,
               bool perform_update);

// =============================================================================
// Raytracing PSO + Shader Binding Table
// =============================================================================

// Compiled state-object containing one raygen, one miss, and one closest-hit
// shader. Sufficient for DDGI's single ray type. SBT is allocated alongside
// and pre-populated with the three identifiers.
//
// Local root signatures are not used (raygen / hit groups read all data via
// the global root signature which is bound separately on the command list).
struct Pipeline
{
    Microsoft::WRL::ComPtr<ID3D12StateObject>           stateObject;
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> properties;

    // Shader binding table — one record per shader, 64-byte aligned.
    // Layout:
    //   [0]                                                 raygen record
    //   [kSBTRecordStride]                                  miss record
    //   [2 * kSBTRecordStride]                              hit-group record
    Microsoft::WRL::ComPtr<ID3D12Resource>             sbtBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS                          rayGenVA   = 0;
    D3D12_GPU_VIRTUAL_ADDRESS                          missVA     = 0;
    D3D12_GPU_VIRTUAL_ADDRESS                          hitGroupVA = 0;
    uint32_t                                           sbtRecordStride = 0;

    // Mirror of the global root signature handed to the state object — needed
    // by callers to SetComputeRootSignature before DispatchRays.
    Microsoft::WRL::ComPtr<ID3D12RootSignature>        globalRootSig;
};

// PSO description. The DXIL bytecode is compiled offline by DxcCompiler with
// SM 6.5 lib_6_3 (or later) and target "lib_6_5". rayGenName / missName /
// closestHitName are the wide-string entry-point names declared in HLSL.
struct PipelineDesc
{
    const void*  dxilBytecode  = nullptr;
    size_t       dxilSize      = 0;
    const wchar_t* rayGenName  = L"DDGIRayGen";
    const wchar_t* missName    = L"DDGIMiss";
    const wchar_t* closestHitName = L"DDGIClosestHit";
    const wchar_t* hitGroupName   = L"DDGIHitGroup";
    uint32_t     maxPayloadSizeBytes  = 16; // float3 radiance + float distance
    uint32_t     maxAttribSizeBytes   = 8;  // float2 barycentrics
    uint32_t     maxRecursionDepth    = 1;  // primary rays only

    // Global root signature shared with the calling pass. The DDGI pass
    // reuses the engine's compute root signature so the same descriptor
    // tables (SRVs/UAVs/CBs) bind cleanly via SetComputeRootSignature.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> globalRootSig;
};

// Compile + link the state object, allocate + populate the SBT.
bool CreatePipeline(GraphicsDX12& gfx, const PipelineDesc& desc, Pipeline& outPipeline);

// Issue DispatchRays — caller must have already bound the compute root
// signature + descriptor tables, and the state-object via SetPipelineState1.
void DispatchRays(GraphicsDX12& gfx,
                  ID3D12GraphicsCommandList4* cmdList,
                  const Pipeline& pipeline,
                  uint32_t width, uint32_t height, uint32_t depth);

// Convenience: full bind + dispatch in one call. Sets the state object and
// inserts the dispatch.
void BindAndDispatchRays(GraphicsDX12& gfx,
                         ID3D12GraphicsCommandList4* cmdList,
                         const Pipeline& pipeline,
                         uint32_t width, uint32_t height, uint32_t depth);

} // namespace RT
