#include "RenderGraph/TestTrianglePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DxcCompiler.h"
#include "System/Log.h"

#include <cstring>
#include <stdexcept>

namespace
{
    void ThrowIfFailedHR(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            LOG_ERROR("TestTrianglePass: %s (hr=0x%08X)", what, static_cast<unsigned>(hr));
            throw std::runtime_error("TestTrianglePass failed");
        }
    }
}

namespace RG
{
    TestTrianglePass::TestTrianglePass(BuiltinTexture target)
        : m_target(target)
    {
    }

    void TestTrianglePass::Setup(RenderGraphBuilder& builder)
    {
        builder.SetColorTarget(m_target);
    }

    void TestTrianglePass::Init(IGraphicsDevice& gfx)
    {
        // This pass is DX12-only; cast to concrete type to access device/command list.
        auto& dx12 = static_cast<GraphicsDX12&>(gfx);
        ID3D12Device* device = dx12.GetDevice();
        if (!device)
            return;

        // Root signature (no parameters)
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        ThrowIfFailedHR(
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob),
            "D3D12SerializeRootSignature");
        ThrowIfFailedHR(
            device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)),
            "CreateRootSignature");

        const char* vsSource = R"vs(
struct VSInput { float3 pos : POSITION; float3 col : COLOR; };
struct PSInput { float4 pos : SV_POSITION; float3 col : COLOR; };
PSInput main(VSInput i) { PSInput o; o.pos = float4(i.pos, 1.0f); o.col = i.col; return o; }
)vs";

        const char* psSource = R"ps(
struct PSInput { float4 pos : SV_POSITION; float3 col : COLOR; };
float4 main(PSInput i) : SV_TARGET { return float4(i.col, 1.0f); }
)ps";

        DxcCompiler::CompileOptions vsOpt;
        vsOpt.sourceName = "TestTriangle.vs";
        vsOpt.entry      = "main";
        vsOpt.stage      = RHI::ShaderStage::VS;
        const auto vsRes = DxcCompiler::Compile(vsSource, std::strlen(vsSource), vsOpt);
        if (!vsRes.ok)
        {
            LOG_ERROR("TestTrianglePass: VS compile failed\n%s", vsRes.errorMsg.c_str());
            throw std::runtime_error("TestTrianglePass VS compile failed");
        }

        DxcCompiler::CompileOptions psOpt;
        psOpt.sourceName = "TestTriangle.ps";
        psOpt.entry      = "main";
        psOpt.stage      = RHI::ShaderStage::PS;
        const auto psRes = DxcCompiler::Compile(psSource, std::strlen(psSource), psOpt);
        if (!psRes.ok)
        {
            LOG_ERROR("TestTrianglePass: PS compile failed\n%s", psRes.errorMsg.c_str());
            throw std::runtime_error("TestTrianglePass PS compile failed");
        }

        D3D12_INPUT_ELEMENT_DESC inputElements[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode              = D3D12_FILL_MODE_SOLID;
        rasterDesc.CullMode              = D3D12_CULL_MODE_BACK;
        rasterDesc.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
        rasterDesc.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        rasterDesc.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterDesc.DepthClipEnable       = TRUE;
        rasterDesc.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        D3D12_BLEND_DESC blendDesc{};
        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.SrcBlend              = D3D12_BLEND_ONE;
        rtBlend.DestBlend             = D3D12_BLEND_ZERO;
        rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rtBlend.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blendDesc.RenderTarget[0]     = rtBlend;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencilDesc.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.InputLayout        = { inputElements, _countof(inputElements) };
        psoDesc.pRootSignature     = m_rootSignature.Get();
        psoDesc.VS                 = { vsRes.dxil.data(), vsRes.dxil.size() };
        psoDesc.PS                 = { psRes.dxil.data(), psRes.dxil.size() };
        psoDesc.RasterizerState    = rasterDesc;
        psoDesc.BlendState         = blendDesc;
        psoDesc.DepthStencilState  = depthStencilDesc;
        psoDesc.SampleMask         = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets   = 1;
        psoDesc.RTVFormats[0]      = (m_target == BuiltinTexture::HdrSceneColor)
                                     ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                     : DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count   = 1;
        ThrowIfFailedHR(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)),
            "CreateGraphicsPipelineState");

        // Vertex buffer (upload heap)
        struct Vertex { float position[3]; float color[3]; };
        const Vertex triangleVertices[] =
        {
            { {  0.0f,  0.25f,  0.0f }, { 1.0f, 0.0f, 0.0f } },
            { {  0.25f, -0.25f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { { -0.25f, -0.25f, 0.0f }, { 0.0f, 0.0f, 1.0f } }
        };
        const UINT vertexBufferSize = static_cast<UINT>(sizeof(triangleVertices));

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = vertexBufferSize;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailedHR(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertexBuffer)),
            "CreateCommittedResource (VB)");

        void* mappedData = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailedHR(m_vertexBuffer->Map(0, &readRange, &mappedData), "VB Map");
        std::memcpy(mappedData, triangleVertices, vertexBufferSize);
        m_vertexBuffer->Unmap(0, nullptr);

        m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vbv.SizeInBytes    = vertexBufferSize;
        m_vbv.StrideInBytes  = sizeof(Vertex);
    }

    RHI::CommandList TestTrianglePass::Execute(RHI::CommandList cl)
    {
        if (!m_pso || !m_rootSignature)
            return cl;

        auto& dx12 = static_cast<GraphicsDX12&>(cl.GetDevice());
        ID3D12GraphicsCommandList* nativeCL = dx12.GetNativeCommandList(cl);
        if (!nativeCL)
            return cl;

        nativeCL->SetPipelineState(m_pso.Get());
        nativeCL->SetGraphicsRootSignature(m_rootSignature.Get());
        nativeCL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        nativeCL->IASetVertexBuffers(0, 1, &m_vbv);
        nativeCL->DrawInstanced(3, 1, 0, 0);
        return cl;
    }
}
