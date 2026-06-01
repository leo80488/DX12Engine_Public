#pragma once

// SpotShadowPass — renders a shared shadow-map atlas for spot lights that
// opt-in via LightData::castsShadow == true. The atlas is a
// Texture2DArray<D32_FLOAT> with one 512² slice per shadow-casting spot
// (max 8). Each frame:
//
//   1. Renderer collects spot lights with castsShadow flag, assigns them a
//      slice index, and builds a per-light view-projection matrix (light-
//      space perspective covering the cone).
//   2. This pass clears its atlas, binds each slice's DSV, and renders all
//      opaque DrawPackets depth-only from the light's POV.
//   3. Lighting.ps and FroxelLightInject.cs both sample the atlas with a
//      comparison sampler and multiply their per-light contribution by the
//      resulting visibility. Lights whose shadowSliceIdx == 0xFFFFFFFF
//      bypass the sampling entirely (no shadow — original behaviour).
//
// Reuses Shadow_VS / Shadow_PS (same depth-only pair that CSM uses).
//
// Root-sig layout (runtime):
//   b1 space0 — ShadowPerViewCB (shadowViewProj for the current slice)
//   t0 space0 — InstanceBuffer
//   t1 space0 — MeshDescriptors
//   t0 space1 — bindless g_Buffers[]

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

class SpotShadowPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kMaxCasters   = 8;
    static constexpr uint32_t kShadowMapSize = 2048;
    static constexpr uint32_t kInvalidSliceIdx = 0xFFFFFFFFu;

    SpotShadowPass();
    ~SpotShadowPass();

    const char* GetName() const override { return "SpotShadowPass"; }
    void Setup (RG::RenderGraphBuilder&) override {}
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute.
    // activeCount = number of spot lights that opted-in this frame (<= kMaxCasters).
    void SetActiveCasterCount(uint32_t n) { m_activeCount = (n > kMaxCasters) ? kMaxCasters : n; }
    void SetShadowMatrix(uint32_t slice, const DirectX::XMFLOAT4X4& viewProj)
    {
        if (slice < kMaxCasters) m_pendingVP[slice] = viewProj;
    }

    // Consumed by Lighting.ps / FroxelLightInject.cs.
    uint64_t                  GetAtlasSrvHandle() const;
    static constexpr uint32_t GetAtlasSize()       { return kShadowMapSize; }
    static constexpr uint32_t GetMaxCasters()      { return kMaxCasters; }

    // The atlas's per-slice DSVs are auto-created by the RHI (array DS + per
    // slice DSV). m_atlas is exposed for Renderer's optional state barriers.
    const RHI::Texture& GetAtlasTexture() const { return m_atlas; }

private:
    PSODesc BuildPSODesc(PermutationKey perm) const;
    void    UploadCasterCBs();

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RHI::Texture m_atlas;  // Texture2DArray<D32_FLOAT>, kMaxCasters slices

    // One small CB per caster, each holds a single transposed float4x4.
    // 256-byte-aligned-per-CBV layout — bound via BindConstantBufferAtOffset
    // with `i * kCasterCBStride`. Triple-buffered so frame N+1's CPU upload
    // can't race frame N's GPU read.
    static constexpr uint32_t kCasterCBStride = 256; // D3D12 CBV alignment
    struct CasterCBPool
    {
        // 256-byte slot per caster — pad to the full stride so the FrameCB<>
        // size math + the per-caster byteOffset bind line up cleanly.
        uint8_t slots[kMaxCasters * kCasterCBStride];
    };
    FrameCB<CasterCBPool> m_casterCBs;

    DirectX::XMFLOAT4X4 m_pendingVP[kMaxCasters]{};
    uint32_t            m_activeCount = 0;

    // Indirect draw arg buffer for batching opaque draws per slice (same
    // layout as ShadowPass / GBufferPass). 512 is too small for Bistro-
    // scale scenes where spot shadows need >20 k indirect draws; see
    // ShadowPass.h for the full explanation. Triple-buffered manual ring
    // (UPLOAD heap, no CB bind flag → can't use FrameCB<>).
    // 3 == GraphicsDX12::FrameCount.
    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = 3;
    RHI::GPUBuffer m_indirectArgBuffer[kFrameCount];
    void*          m_indirectArgMapped[kFrameCount] = {};

    IGraphicsDevice* m_gfxPtr = nullptr;

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps's g_LinearWrap).
    int              m_alphaSamplerIdx = -1;

    // Tracks first-execution state so we can transition from UAV / UNDEFINED
    // on the very first frame and from DEPTH_READ_SRV on subsequent ones.
    bool m_firstExecution = true;
};
