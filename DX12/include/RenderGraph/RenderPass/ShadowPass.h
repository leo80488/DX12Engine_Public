#pragma once

// ShadowPass — 3-cascade shadow map rendering pass.
//
// Renders all opaque DrawPackets 3 times (once per cascade) to a single
// 2048×2048×3 Texture2DArray depth render target (D32_FLOAT, 3 array slices).
// Per-slice DSVs are created manually via raw DX12 so each cascade can be
// cleared and rendered to independently.
//
// Produces one Texture2DArray<float> SRV bound at root parameter slot 22
// (t9 space0) for LightingPass.  The shader samples via float3(uv, sliceIdx).
//
// Per-cascade view-projection matrices are supplied each frame by the wired
// ShadowSystem (see SetShadowSystem). An UPLOAD constant buffer per cascade
// is bound at b1 space0 (same slot as PerViewCB) during shadow rendering.
//
// Root signature slots used (must match GraphicsDX12.cpp):
//   b0      PushConstants (meshDescIdx, instanceOffset, materialIndex)
//   b1      ShadowPerViewCB (per-cascade shadowViewProj)
//   t0      InstanceBuffer
//   t1      MeshDescriptors
//   t9      Shadow Texture2DArray SRV (produced by this pass, consumed by LightingPass)

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IndirectDrawCommand.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class ShadowSystem;
class TerrainPass;

class ShadowPass : public RG::RenderPass
{
public:
    ShadowPass();
    ~ShadowPass();

    const char* GetName() const override { return "ShadowPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx)      override;

    // Wire the TerrainPass that owns the active terrain's heightmap SRV +
    // dispatch tile count. Set once after Compile(); ShadowPass dispatches
    // a depth-only mesh-shader pass per cascade using these bindings.
    // Pass nullptr (or never call) to disable terrain shadow casting.
    void SetTerrainPass(TerrainPass* tp) { m_terrainPass = tp; }

    // Pointer to the per-frame TerrainParams CB owned by Renderer. Required
    // alongside SetTerrainPass for terrain shadow rendering — ShadowPass is
    // a standalone pass (not in the RenderGraph), so the graph-published
    // "TerrainParams" CB binding isn't visible here; we must bind it
    // directly via the raw device path the way the cascade CBs are bound.
    void SetTerrainParamsCB(const RHI::GPUBuffer* cb) { m_terrainParamsCB = cb; }

    // Cascades 0..2 are the standard near CSM cascades. Cascade 3 is the
    // dedicated ultra-far cascade — terrain-only, covers ShadowSystem::
    // kFarCascadeFar in view-Z. See ShadowSystem.cpp for the split layout
    // and ShadowPass.cpp::Execute for the terrain-only render path.
    static constexpr int      kCascadeCount  = 4;
    static constexpr int      kFarCascadeIdx = kCascadeCount - 1;
    // CSM atlas resolution per cascade. 2048 => 4096*4cascades*4B = 64 MB.
    // (Was 4096 = 256 MB; reverted — near-cascade fill + VRAM not justified.
    //  Receiver biases are resolution-adaptive so this is a safe knob.)
    static constexpr uint32_t kShadowMapSize = 2048;

    // Wire the ShadowSystem that supplies per-cascade lightVP matrices.
    // Must be called once after Init(), before Execute().
    void SetShadowSystem(const ShadowSystem* sys) { m_sys = sys; }

    // Returns the GPU SRV handle for the Texture2DArray shadow resource.
    // Bind at root slot 22 (kShadowSRVSlot) in LightingPass.
    uint64_t GetShadowArrayGpuHandle() const;

private:
    // Caller supplies both shader permutation (alpha-test on/off) and the
    // rasterizer cull mode. PSOCache hashes the full PSODesc so every
    // (perm × cull) combination gets its own cached PSO naturally.
    PSODesc BuildPSODesc(PermutationKey perm, RHI::CullMode cullMode) const;
    void    UploadCascadeCBs();    // writes system matrices to mapped UPLOAD CBs

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Single Texture2DArray depth resource (kShadowMapSize × kShadowMapSize × kCascadeCount).
    // Per-slice DSVs are auto-created by the RHI when array_size > 1 + DEPTH_STENCIL;
    // accessed via IGraphicsDevice::SetDepthStencilSlice / ClearDepthStencilSlice.
    RHI::Texture m_shadowArray;

    // Per-cascade constant buffers (UPLOAD heap, layout = float4x4
    // shadowViewProj + 6 frustum planes). The four cascades live as 256-byte-
    // aligned slots inside one FrameCB pool. The pool is triple-buffered so
    // CPU writes don't race the GPU draws still consuming last frame's data.
    static constexpr uint32_t kCascadeCBStride = 256; // D3D12 CBV alignment
    struct CascadeCBPool
    {
        uint8_t slots[kCascadeCount * kCascadeCBStride];
    };
    FrameCB<CascadeCBPool> m_cascadeCBs;

    // Source of cascade matrices. Non-owning; lifetime managed by Renderer.
    const ShadowSystem* m_sys = nullptr;

    // Reference to device (needed for cleanup in destructor).
    IGraphicsDevice* m_gfxPtr = nullptr;

    // State tracking: array is created in DEPTHSTENCIL; on frames 2+ it arrives as DEPTH_READ_SRV.
    bool m_firstExecution = true;

    // ExecuteIndirect buffer for batched shadow draws (UPLOAD heap, filled
    // per-frame). 512 was tuned for character-scale scenes; Bistro pushes
    // >20 k shadow casters and under-sizing here makes the write loop cap
    // at 512 while groupCounts track the full count, so ExecuteIndirect
    // reads past the buffer tail ("argument resource too small").
    // 32 k × 32 B = 1 MB per buffer — trivial memory cost. Triple-buffered
    // manual ring (UPLOAD heap, no CB bind flag → can't use FrameCB<>).
    // 3 == GraphicsDX12::FrameCount.
    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = 3;
    RHI::GPUBuffer m_indirectArgBuffer[kFrameCount];
    void*          m_indirectArgMapped[kFrameCount] = {};

    // ---- Terrain shadow casting (mesh-shader path) -----------------------
    // The terrain is dispatched per-cascade after the regular DrawPacket
    // pass. Uses a depth-only PSO (Terrain_Shadow_MS + null PS) and the
    // same per-cascade ShadowPerViewCB the rest of the pass binds at b1.
    TerrainPass*           m_terrainPass             = nullptr;   // non-owning
    const RHI::GPUBuffer*  m_terrainParamsCB         = nullptr;   // owned by Renderer
    RHI::PipelineState     m_terrainShadowPSO;
    int                    m_terrainShadowSamplerIdx = -1;        // s0 linear-clamp

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps).
    int                    m_alphaSamplerIdx         = -1;        // s0 linear-wrap

    // Build (or rebuild) the terrain shadow PSO. Called from Init and from
    // ReloadShaders so the depth-bias state stays in sync with the main
    // shadow PSOs.
    bool BuildTerrainShadowPSO(IGraphicsDevice& gfx);
    // Per-cascade terrain dispatch. Binds m_cascadeCBs[cascadeIdx] at b1
    // (in case the regular draw loop didn't bind it because no DrawPackets
    // existed) plus the terrain-specific resources, then DispatchMesh.
    // No-op when m_terrainPass is null or the active terrain has no
    // heightmap loaded.
    void RenderTerrainShadow(RHI::CommandList cl, int cascadeIdx);
};
