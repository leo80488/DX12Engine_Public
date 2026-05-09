#pragma once

#include "RenderGraph/RenderGraph.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl.h>

class IGraphicsDevice;

namespace RG
{
    // Test pass that draws a single RGB triangle to a builtin target.
    // Useful for smoke-testing the render graph without scene data.
    //
    // NOTE: This pass owns its own vertex buffer as a self-contained test
    // fixture. Production passes should receive geometry views from the Renderer.
    class TestTrianglePass final : public RenderPass
    {
    public:
        explicit TestTrianglePass(BuiltinTexture target);

        const char* GetName() const override { return "TestTrianglePass"; }
        void Setup(RenderGraphBuilder& builder) override;
        void Init(IGraphicsDevice& gfx) override;
        RHI::CommandList Execute(RHI::CommandList cl) override;

    private:
        BuiltinTexture m_target;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
        Microsoft::WRL::ComPtr<ID3D12Resource>      m_vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW                    m_vbv{};
    };
}
