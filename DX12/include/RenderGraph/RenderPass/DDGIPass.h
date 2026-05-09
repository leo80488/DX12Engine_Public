#pragma once

// DDGIPass — owns every per-frame DDGI dispatch:
//
//   1. (TLAS build is performed by the Renderer before this pass runs.)
//   2. DispatchRays(raysPerProbe × probeCount) writing per-probe rays into
//      the volume's RayData buffer.
//   3. Two CS dispatches relighting the irradiance + depth atlases via EMA
//      blend.
//   4. Two CS dispatches updating the +1-texel atlas border.
//
// Standalone pass (manual Execute) — DXR DispatchRays / SetPipelineState1
// require an `ID3D12GraphicsCommandList4`; the RG lambda model can drive the
// dispatch but not the state-object creation, so the pass is owned by the
// Renderer directly (mirroring SkinningPass / ParticleSimPass pattern).
//
// The pass operates on one volume per call. Renderer iterates active
// volumes after the manager has uploaded constants for this frame.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"

#include <wrl.h>
#include <d3d12.h>
#include <cstdint>

namespace DDGI { class DDGIVolumeManager; }

class DDGIPass
{
public:
    // Initialise root signatures, compile shaders, build PSOs. Returns false
    // (with a logged error) if the device is not DXR-capable. Callers should
    // skip Execute when Init returned false.
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Reload the DXC-compiled shaders + rebuild PSOs. Used by the engine's
    // hot-reload loop. Must be called between frames (GPU idle).
    bool ReloadShaders(IGraphicsDevice& gfx);

    // Per-volume execution. The TLAS GPU VA is passed in directly because the
    // pass is decoupled from how the AS is built — Renderer builds a single
    // DDGI-only TLAS shared across volumes (same static geometry).
    //
    // Caller must have:
    //   - Uploaded volume constants this frame (DDGIVolumeManager::Tick).
    //   - Built / refit the DDGI TLAS before this call.
    //   - Acquired a graphics or compute command list (DXR works on either).
    //
    // @p matBufVA is the GPU virtual address of the per-instance albedo buffer
    // published by DDGISceneAS; closest-hit indexes it via InstanceID().
    // 0 → closest-hit defaults to grey.
    void Execute(IGraphicsDevice& gfx,
                 RHI::CommandList cmd,
                 DDGI::DDGIVolumeManager& mgr,
                 uint32_t volumeSlot,
                 D3D12_GPU_VIRTUAL_ADDRESS tlasVA,
                 uint64_t skyIBLSrv,
                 D3D12_GPU_VIRTUAL_ADDRESS matBufVA,
                 uint64_t bindlessBufferTable,
                 uint64_t lightsSrvHandle,
                 bool     onComputeQueue = false);

    bool IsReady() const { return m_ready; }

private:
    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreateCSPipelines(IGraphicsDevice& gfx);

    // The DDGI-only compute/raytracing root signature. Layout:
    //   [0] ROOT_CBV          b0 space0  (DDGIVolumeGPU)
    //   [1] ROOT_SRV          t0 space0  (TLAS)
    //   [2] DESC_TABLE 1 SRV  t1 space0  (sky IBL)
    //   [3] DESC_TABLE 1 UAV  u0 space0  (atlas / ray data — interpretation
    //                                    differs per dispatch)
    //   [4] DESC_TABLE 1 SRV  t2 space0  (ray data SRV — relight read path)
    //   [5] ROOT_SRV          t3 space0  (per-instance data — DDGISceneAS)
    //   [6] DESC_TABLE 4096 SRV t0 space1 (bindless ByteAddressBuffer g_DDGIBuffers[]
    //                                    — VB/IB lookup at hit time for face-normal)
    //   [7] DESC_TABLE 1 UAV  u1 space0  (variance buffer — irradiance relight only)
    //   [8] DESC_TABLE 1 SRV  t4 space0  (irradiance atlas SRV — multi-bounce trace read)
    //   [9] DESC_TABLE 1 SRV  t5 space0  (depth atlas SRV — multi-bounce trace read)
    //   [10] DESC_TABLE 1 SRV t6 space0  (probe data SRV — multi-bounce trace read)
    //   [11] DESC_TABLE 1 SRV t7 space0  (cluster GPULight buffer — direct light loop)
    //   [12] DESC_TABLE 1 UAV u2 space0  (per-probe adaptive ray count)
    //   [13] DESC_TABLE 1 UAV u3 space0  (ray allocation buffer — total + descriptors)
    //   [14] DESC_TABLE 1 UAV u4 space0  (dispatch args buffer — written by finalize CS,
    //                                    read by ExecuteIndirect in INDIRECT_ARGUMENT state)
    //   [15] static sampler s0 space0  (linear wrap)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;

    // Trace CS — replaces the old RTPSO/SBT path. Inline RayQuery (DXR 1.1).
    // Dispatched via ExecuteIndirect using m_traceCmdSig.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_tracePSO;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_traceCmdSig;

    // Adaptive ray-count + indirect-args pipeline.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_prepareRayCountPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_rayAllocationPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_finalizeIndirectPSO;

    // Probe relocation — pushes probes out of nearby surfaces using ray hit
    // distances. Skips the dispatch when the volume's enableRelocation flag
    // is unset; otherwise updates DDGIProbeData.offset each frame (5% EMA).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_relocatePSO;

    // Compute PSOs from DDGIRelight.cs.hlsl + DDGIBorderUpdate.cs.hlsl.
    // (Irradiance border is gone — SH probes have no octahedral seam.)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_relightIrradiancePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_relightDepthPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_borderDepthPSO;

    bool m_ready = false;
};
