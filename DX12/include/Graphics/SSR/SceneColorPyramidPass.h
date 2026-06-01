#pragma once

// SceneColorPyramidPass — pre-filtered HDR mip chain for SSR cone fetch.
//
// Built each frame from the race-free HDR snapshot (owned by SSRResolvePass).
// The trace shader samples this at a mip level derived from roughness × ray
// length so glossy reflections get a pre-integrated radiance — the single
// highest-ROI cure for rough-surface SSR noise.
//
// Owned by SSRSubsystem; sits between the HDR→snapshot copy and the
// trace dispatch. Root sig: shared compute space2.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class SceneColorPyramidPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    void EnsureTexture(uint32_t w, uint32_t h);

    // Generate the full mip chain from the HDR snapshot. Mip 0 is populated
    // via CopyTextureSubresource (D3D12 native copy) — the CS-based mip 0
    // path silently dropped writes despite a valid PSO, texture and binding
    // (likely a per-mip UAV descriptor / heap-slot routing issue).
    // Snapshot must be in SHADER_RESOURCE on entry; this method transitions
    // it through COPY_SRC and back. Pyramid starts in UAV; ends in UAV
    // (caller transitions to SR before sampling, back to UAV after).
    void Execute(RHI::CommandList cl, const RHI::Texture* snapshotTex);

    uint64_t            GetSrvHandle() const;
    const RHI::Texture* GetTexture()  const { return &m_texture; }
    uint32_t            GetWidth()    const { return m_width; }
    uint32_t            GetHeight()   const { return m_height; }
    uint32_t            GetMipCount() const { return m_mipCount; }

private:
    IGraphicsDevice*   m_gfx = nullptr;

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_mip0PSO;
    RHI::PipelineState m_reducePSO;

    RHI::Texture       m_texture;        // RGBA16F, screen size, full mip chain
    uint32_t           m_width    = 0;
    uint32_t           m_height   = 0;
    uint32_t           m_mipCount = 0;

    struct HierCB { uint32_t srcW, srcH, dstW, dstH; };

    // See SSRDepthHierarchyPass — multi-CB-per-frame slot pattern; each mip
    // dispatch must read its own 256-byte slot to avoid the in-flight CPU
    // overwrite race.
    static constexpr uint32_t kCBStride  = 256;
    static constexpr uint32_t kMaxMipsCB = 16;
    struct alignas(256) HierCBArray { uint8_t bytes[kCBStride * kMaxMipsCB]; };

    FrameCB<HierCBArray> m_cb;
};
