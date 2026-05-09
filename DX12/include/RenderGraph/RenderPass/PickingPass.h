#pragma once

// PickingPass — renders per-instance slot IDs to an R32_UINT offscreen texture.
//
// Each pixel stores (instanceSlot + 1) for the frontmost visible object.
// Value 0 = background (clear value).  Renderer translates slot -> Entity.
//
// Usage:
//   RequestPick(pixelX, pixelY)  -- before the frame's Execute call.
//   [frame renders + EndFrame submits]
//   IGraphicsDevice::FlushAndWait()
//   ReadPickResult()             -- returns the raw slot+1 value (0 = no hit).
//   ClearPickPending()           -- consume the result.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

#include <cstdint>

class IGraphicsDevice;

class PickingPass : public RG::RenderPass
{
public:
    PickingPass() = default;
    ~PickingPass() override;

    const char* GetName() const override { return "PickingPass"; }
    void Setup    (RG::RenderGraphBuilder& b)                   override;
    void Init     (IGraphicsDevice& gfx)                        override;
    void OnCompile(IGraphicsDevice& gfx)                        override;
    RHI::CommandList Execute(RHI::CommandList cl)               override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Request a pick at viewport pixel (pixelX, pixelY).
    // The copy is recorded during the next Execute() call.
    void RequestPick(int pixelX, int pixelY);

    // After IGraphicsDevice::FlushAndWait(), read the readback buffer.
    // Returns the raw (instanceSlot + 1) value; 0 = background.
    uint32_t ReadPickResult() const;

    bool IsPickPending()  const { return m_pickPending; }
    void ClearPickPending()     { m_pickPending = false; }

private:
    PSODesc BuildPSODesc() const;

    // Recreate ID/depth textures when the viewport changes.
    void EnsureTextures(IGraphicsDevice& gfx, uint32_t w, uint32_t h);

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Owned GPU resources (RHI handles — no DX12 types here)
    RHI::Texture   m_idTexture;        // R32_UINT render target
    RHI::Texture   m_depthTexture;     // D32_FLOAT depth buffer
    RHI::GPUBuffer m_readbackBuffer;   // READBACK heap, single pixel

    RHI::ResourceState m_idState{ RHI::ResourceState::RENDERTARGET };

    uint32_t m_texWidth  = 0;
    uint32_t m_texHeight = 0;

    // Pick request state
    // m_pickRequested: set by RequestPick(), consumed by Execute() to record the GPU copy.
    // m_pickPending:   set by Execute() once copy is recorded; cleared by ClearPickPending().
    //                  IsPickPending() / ResolvePick() only check this flag.
    bool m_pickRequested = false;
    bool m_pickPending   = false;
    int  m_pickX         = 0;
    int  m_pickY         = 0;

    // Back-pointer to device for destructor cleanup
    IGraphicsDevice* m_gfx = nullptr;
};
