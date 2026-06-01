#pragma once

// ParticleSimPass — runs the particle emit + update compute shaders.
// ParticleRenderPass — draws the particle pool as billboards.
//
// Both live in one header to keep the particle module cohesive; they share
// the same ParticleSystem pointer and lifecycle.
//
// Execution order in Renderer::Render:
//   1. ParticleSimPass (compute, runs before color passes via a separate
//      BeginCommandList on the same graphics queue — barriers ensure the
//      render pass sees the updated pool).
//   2. ParticleRenderPass (graphics, runs inside the RenderGraph after
//      TransparentPass so particles composit over the scene.)

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class ParticleSystem;

// ---------------------------------------------------------------------------
// ParticleSimPass — compute-only; not in the RenderGraph.
// ---------------------------------------------------------------------------
// Integrates outside the RenderGraph (manual BeginCommandList / Execute
// from Renderer::Render, same pattern as SkinningPass). Runs two dispatches:
//   (a) ParticleEmitCS — one per active emitter, threadGroups = ceil(spawn/64)
//   (b) ParticleUpdateCS — pool/64 threadGroups, advances every slot
class ParticleSimPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) { (void)gfx; }

    void SetSystem(ParticleSystem* sys) { m_sys = sys; }

    // Sources for mesh-shape emitter sampling — consumed by the emit
    // shader via compute root slots 10 (MeshDescriptors SRV) and 11
    // (bindless buffer table). Renderer sets these each frame.
    void SetMeshDescriptorBinding(const RHI::GPUBuffer* meshDescBuf,
                                  uint64_t              bindlessTableHandle)
    {
        m_meshDescBuffer       = meshDescBuf;
        m_bindlessTableHandle  = bindlessTableHandle;
    }

    // Records the compute work into cl.
    void Execute(RHI::CommandList cl);

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_emitPSO;
    RHI::PipelineState m_updatePSO;

    ParticleSystem*    m_sys = nullptr;

    // For mesh-shape emitter sampling (optional; null = mesh shape produces
    // nothing, but other shapes still work).
    const RHI::GPUBuffer* m_meshDescBuffer      = nullptr;
    uint64_t              m_bindlessTableHandle = 0;
};

// ---------------------------------------------------------------------------
// ParticleRenderPass — graphics; goes into the RenderGraph after Transparent.
// ---------------------------------------------------------------------------
class ParticleRenderPass : public RG::RenderPass
{
public:
    explicit ParticleRenderPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "ParticleRenderPass"; }
    void Setup  (RG::RenderGraphBuilder& b) override;
    void Init   (IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    void SetSystem(ParticleSystem* sys) { m_sys = sys; }

    // Per-frame camera state. Called by Renderer::Render before graph exec.
    // viewProj: jittered VP (SV_POSITION); camRight/camUp: world-space axes
    // for billboard expansion; particleSize: global size multiplier.
    void SetFrameData(const DirectX::XMFLOAT4X4& viewProj,
                      const DirectX::XMFLOAT3&   camRight,
                      const DirectX::XMFLOAT3&   camUp,
                      float                      particleSize = 1.0f)
    {
        m_viewProj     = viewProj;
        m_camRight     = camRight;
        m_camUp        = camUp;
        m_particleSize = particleSize;
    }

private:
    // Per-frame constants pushed via root CBV (slot 1 → b2 space0).
    struct alignas(16) RenderCB
    {
        float    viewProj[16];
        float    camRight[3];
        float    particleSize;
        float    camUp[3];
        float    _pad;
    };

    // Per-frame data set by Renderer.
    DirectX::XMFLOAT4X4 m_viewProj{};
    DirectX::XMFLOAT3   m_camRight{ 1, 0, 0 };
    DirectX::XMFLOAT3   m_camUp   { 0, 1, 0 };
    float               m_particleSize = 1.0f;

    RG::RGTextureHandle m_depth;

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;    // pre-built additive-blend PSO for MVP

    FrameCB<RenderCB>   m_renderCB;

    int                 m_linearSamplerIdx = -1;

    ParticleSystem*     m_sys = nullptr;
};
