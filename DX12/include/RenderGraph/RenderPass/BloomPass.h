#pragma once

// BloomPass — 13-tap Sledgehammer bloom (5 downsample + 5 upsample steps).
//
// First downsample uses Karis Average to suppress fireflies.
// Upsamples use a 3x3 tent filter blended additively.
// Reads the HDR scene texture (via SetHdrSrvHandle) and writes the final
// composited bloom result into bloom[0] (half resolution).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class BloomPass : public RG::RenderPass
{
public:
    static constexpr int kSteps = 5;

    const char* GetName() const override { return "BloomPass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Called by Renderer before Execute() each frame.
    void SetHdrSrvHandle(uint64_t handle)      { m_hdrSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // After Execute(), this is the final composited bloom SRV (bloom[0]).
    // Pass this to ToneMapPass::SetBloomSrvHandle().
    uint64_t GetBloomSrvHandle() const;

private:
    // Bloom mip-chain: bloom[0]=viewport/2 ... bloom[4]=viewport/32
    RHI::Texture        m_bloom[kSteps];
    RHI::ResourceState  m_bloomState[kSteps]{};
    uint32_t            m_bloomW[kSteps]{};
    uint32_t            m_bloomH[kSteps]{};

    RHI::PipelineState m_downsamplePSO;
    RHI::PipelineState m_upsamplePSO;
    ShaderLibrary       m_shaderLib;

    // Per-dispatch CB (UPLOAD, persistently mapped). 10 × 256-byte slots
    // (kSteps × 2: one per downsample, one per upsample) — each dispatch must
    // read its own slot, otherwise CPU writes for the next step clobber what
    // the GPU has yet to consume.
    static constexpr uint32_t kBloomCBStride = 256;
    static constexpr uint32_t kBloomCBSlots  = kSteps * 2;
    struct alignas(256) BloomCBPool { uint8_t bytes[kBloomCBStride * kBloomCBSlots]; };
    FrameCB<BloomCBPool> m_cb;

    uint64_t m_hdrSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    // Cached viewport size used for texture creation; recreate on change.
    uint32_t m_lastVpW = 0;
    uint32_t m_lastVpH = 0;

    IGraphicsDevice* m_gfx = nullptr;

    void RebuildTextures();
    void DestroyTextures();
};
