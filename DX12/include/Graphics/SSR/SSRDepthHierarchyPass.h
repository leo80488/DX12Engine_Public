#pragma once

// SSRDepthHierarchyPass — 2-channel depth pyramid for SSR Hi-Z ray march.
//
// Runs after GBuffer, before SSRTrace. Produces a power-of-2 R16G16_FLOAT
// pyramid where .r = max depth (nearest surface, reverse-Z) and .g = min
// depth (farthest). Parallel to HiZPass but separate: HiZPass is a single-
// channel min-depth pyramid sized for occlusion culling; SSR needs the
// 2-channel variant to pick the right tile wall per ray direction.
//
// Owned by SSRSubsystem; root sig: shared compute space2.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class SSRDepthHierarchyPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    // Rebuild the pyramid texture when render dimensions change. Keeps the
    // existing resource if dimensions haven't changed.
    void EnsureTexture(uint32_t depthW, uint32_t depthH);

    // Generate the full mip chain from the GBuffer depth SRV.
    // depthSrvHandle must be in SHADER_RESOURCE state.
    void Execute(RHI::CommandList cl, uint64_t depthSrvHandle);

    // SRV of the full pyramid (all mips). SSRTrace samples different mips
    // via Load(int3(xy, mip)).
    uint64_t GetSrvHandle() const;

    const RHI::Texture* GetTexture() const { return &m_texture; }
    uint32_t            GetWidth()   const { return m_width; }
    uint32_t            GetHeight()  const { return m_height; }
    uint32_t            GetMipCount()const { return m_mipCount; }

private:
    IGraphicsDevice*   m_gfx = nullptr;

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_mip0PSO;      // depth SRV → mip 0
    RHI::PipelineState m_reducePSO;    // mip N = reduce(mip N-1)

    RHI::Texture       m_texture;      // R16G16_FLOAT, pow2, full mip chain
    uint32_t           m_width    = 0;
    uint32_t           m_height   = 0;
    uint32_t           m_mipCount = 0;

    struct HierCB { uint32_t srcW, srcH, dstW, dstH; };

    // 16 HierCB entries × 256-byte alignment (one per mip). Dispatches into the
    // same CL must use distinct CB byte-offsets, otherwise every reduce reads
    // the LAST CPU write — see the in-file race comment.
    static constexpr uint32_t kCBStride  = 256;
    static constexpr uint32_t kMaxMipsCB = 16;
    struct alignas(256) HierCBArray { uint8_t bytes[kCBStride * kMaxMipsCB]; };

    FrameCB<HierCBArray> m_cb;
};
