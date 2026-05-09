#pragma once

// SceneColorPyramidPass — pre-filtered HDR mip chain for SSR cone fetch.
//
// Built each frame from the race-free HDR snapshot (owned by SSRResolvePass
// — snapshot is the only read-safe version of the lit HDR while the composite
// pass will later write HDR as a UAV). The resolve shader samples this at a
// mip level derived from roughness × ray length so glossy reflections get a
// pre-integrated radiance — the single highest-ROI cure for rough-surface
// SSR noise.
//
// Owned by Renderer in the Phase 4.6 slot, between the HDR→snapshot copy
// and the resolve dispatch. Root sig: shared compute space2 layout.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"

class IGraphicsDevice;

class SceneColorPyramidPass
{
public:
    void Init(IGraphicsDevice& gfx);

    // Allocate / resize pyramid texture when render dimensions change.
    void EnsureTexture(uint32_t w, uint32_t h);

    // Generate the full mip chain from the HDR snapshot. Mip 0 is now
    // populated via CopyTextureSubresource (D3D12 native copy) — the
    // CS-based mip 0 path silently dropped writes despite a valid PSO,
    // texture and binding (likely a per-mip UAV descriptor / heap-slot
    // routing issue we couldn't isolate). Snapshot must be in SHADER_
    // RESOURCE on entry; this method transitions it through COPY_SRC
    // and back. Pyramid starts in UAV; ends in UAV (caller transitions
    // to SR before sampling, back to UAV after).
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

    RHI::GPUBuffer m_cb;
    void*          m_cbMapped = nullptr;

    struct HierCB { uint32_t srcW, srcH, dstW, dstH; };
};
