#pragma once

// Trail passes — parallels ParticlePasses.
//
// TrailUpdatePass: compute, runs standalone (manual Execute in Renderer::Render).
// TrailRenderPass: graphics, placed in RenderGraph after ParticleRenderPass.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>

class TrailSystem;

// ---------------------------------------------------------------------------
class TrailUpdatePass
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) { (void)gfx; }

    void SetSystem(TrailSystem* sys) { m_sys = sys; }

    void Execute(RHI::CommandList cl);

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    TrailSystem*       m_sys = nullptr;
};

// ---------------------------------------------------------------------------
class TrailRenderPass : public RG::RenderPass
{
public:
    explicit TrailRenderPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "TrailRenderPass"; }
    void Setup  (RG::RenderGraphBuilder& b) override;
    void Init   (IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    void SetSystem(TrailSystem* sys) { m_sys = sys; }

    // Per-frame camera state (set from Renderer::Render before graph exec).
    void SetFrameData(const DirectX::XMFLOAT4X4& viewProj,
                      const DirectX::XMFLOAT3&   camForward)
    {
        m_viewProj   = viewProj;
        m_camForward = camForward;
    }

private:
    struct alignas(16) RenderCB
    {
        float    viewProj[16];
        float    camForward[3];
        uint32_t maxSegments;
    };

    RG::RGTextureHandle m_depth;

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;

    FrameCB<RenderCB>   m_renderCB;

    DirectX::XMFLOAT4X4 m_viewProj{};
    DirectX::XMFLOAT3   m_camForward{ 0, 0, 1 };

    TrailSystem*        m_sys = nullptr;
};
