#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DxcCompiler.h"
#include "ECS/ECS.h"
#include "ECS/DDGIComponents.h"
#include "System/Log.h"

#include <fstream>
#include <vector>
#include <string>

namespace
{
std::vector<uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> buf((size_t)sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::vector<uint8_t> CompileShaderFile(const std::string& path,
                                       RHI::ShaderStage stage,
                                       const std::string& entry = "main")
{
    auto src = ReadFileBytes(path);
    if (src.empty())
    {
        LOG_ERROR("DDGIProbeDebugPass: cannot read shader %s", path.c_str());
        return {};
    }
    DxcCompiler::CompileOptions opts;
    opts.sourceName = path;
    opts.entry      = entry;
    opts.stage      = stage;
    auto res = DxcCompiler::Compile(src.data(), src.size(), opts);
    if (!res.ok)
    {
        LOG_ERROR("DDGIProbeDebugPass: compile %s failed: %s", path.c_str(), res.errorMsg.c_str());
        return {};
    }
    return res.dxil;
}
} // namespace

bool DDGIProbeDebugPass::Init(IGraphicsDevice& gfx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12Device* dev = dx12.GetDevice();
    if (!dev) return false;

    // ---- Root signature ----
    D3D12_ROOT_PARAMETER params[5]{};
    // [0] PerView CB at b0
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
    // [1] Volume CB at b1 (used by both VS and PS)
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace  = 0;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    // [2] Root constants (sphereRadius + debugMode) at b2 — visible to BOTH
    // VS (uses sphereRadius) and PS (uses debugMode). VERTEX-only visibility
    // would silently break PSO creation when PS references b2.
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister  = 2;
    params[2].Constants.RegisterSpace   = 0;
    params[2].Constants.Num32BitValues  = 4;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    // [3] Irradiance atlas SRV table at t0 — PS only
    static D3D12_DESCRIPTOR_RANGE atlasRange{};
    atlasRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    atlasRange.NumDescriptors   = 1;
    atlasRange.BaseShaderRegister = 0;
    atlasRange.RegisterSpace      = 0;
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &atlasRange;
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [4] ProbeData SRV at t1 — VS reads pd.offset to draw probe at its
    // relocation-adjusted world position (matches what trace + sampling use).
    static D3D12_DESCRIPTOR_RANGE probeDataRange{};
    probeDataRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    probeDataRange.NumDescriptors   = 1;
    probeDataRange.BaseShaderRegister = 1;
    probeDataRange.RegisterSpace      = 0;
    params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges   = &probeDataRange;
    params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister   = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = 5;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &samp;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        LOG_ERROR("DDGIProbeDebugPass: serialize root sig failed: %s",
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&m_rootSig))))
    {
        LOG_ERROR("DDGIProbeDebugPass: CreateRootSignature failed");
        return false;
    }

    // ---- Compile shaders + build PSO ----
    auto vs = CompileShaderFile("shaders/DDGIProbeDebug.vs.hlsl", RHI::ShaderStage::VS);
    auto ps = CompileShaderFile("shaders/DDGIProbeDebug.ps.hlsl", RHI::ShaderStage::PS);
    if (vs.empty() || ps.empty()) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                    = m_rootSig.Get();
    pd.VS.pShaderBytecode                = vs.data(); pd.VS.BytecodeLength = vs.size();
    pd.PS.pShaderBytecode                = ps.data(); pd.PS.BytecodeLength = ps.size();
    pd.RasterizerState.FillMode          = D3D12_FILL_MODE_SOLID;
    // Debug viz — cull NONE so the user sees the sphere from any angle even
    // if the procedural UV-sphere winding ends up backwards on either pole.
    pd.RasterizerState.CullMode          = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable   = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                        = UINT_MAX;
    pd.SampleDesc.Count                  = 1;
    pd.PrimitiveTopologyType             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // Reversed-Z scene — we want "closer than scene" to PASS so probes occlude
    // properly. GREATER_EQUAL matches the engine's convention.
    pd.DepthStencilState.DepthEnable     = TRUE;
    pd.DepthStencilState.DepthWriteMask  = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc       = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pd.DSVFormat                         = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.NumRenderTargets                  = 1;
    pd.RTVFormats[0]                     = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    {
        LOG_ERROR("DDGIProbeDebugPass: CreateGraphicsPipelineState failed");
        return false;
    }

    m_ready = true;
    LOG_INFO("DDGIProbeDebugPass: ready");
    return true;
}

void DDGIProbeDebugPass::Shutdown(IGraphicsDevice& /*gfx*/)
{
    m_pso.Reset();
    m_rootSig.Reset();
    m_ready = false;
}

bool DDGIProbeDebugPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_ready) return false;
    m_pso.Reset();
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    auto vs = CompileShaderFile("shaders/DDGIProbeDebug.vs.hlsl", RHI::ShaderStage::VS);
    auto ps = CompileShaderFile("shaders/DDGIProbeDebug.ps.hlsl", RHI::ShaderStage::PS);
    if (vs.empty() || ps.empty()) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                    = m_rootSig.Get();
    pd.VS.pShaderBytecode                = vs.data(); pd.VS.BytecodeLength = vs.size();
    pd.PS.pShaderBytecode                = ps.data(); pd.PS.BytecodeLength = ps.size();
    pd.RasterizerState.FillMode          = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode          = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.DepthClipEnable   = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                        = UINT_MAX;
    pd.SampleDesc.Count                  = 1;
    pd.PrimitiveTopologyType             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.DepthStencilState.DepthEnable     = TRUE;
    pd.DepthStencilState.DepthWriteMask  = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc       = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pd.DSVFormat                         = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.NumRenderTargets                  = 1;
    pd.RTVFormats[0]                     = DXGI_FORMAT_R16G16B16A16_FLOAT;
    return SUCCEEDED(dx12.GetDevice()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)));
}

void DDGIProbeDebugPass::Execute(IGraphicsDevice&             gfx,
                                 RHI::CommandList             cmd,
                                 const RHI::Texture*          depthTex,
                                 const RHI::GPUBuffer&        perViewCB,
                                 DDGI::DDGIVolumeManager&     mgr,
                                 World&                       world)
{
    if (!m_ready || !enabled) return;

    auto* volPool = world.GetPool<DDGIVolumeComponent>();
    auto* runPool = world.GetPool<DDGIVolumeRuntimeComponent>();
    if (!volPool || !runPool) return;

    // Quick check: any volume actually wants debug draw? If not, bail without
    // touching the command list state.
    bool anyDebugDraw = false;
    {
        auto& d = volPool->Data();
        for (size_t i = 0; i < d.size(); ++i)
            if (d[i].debugDraw) { anyDebugDraw = true; break; }
    }
    if (!anyDebugDraw) return;

    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12GraphicsCommandList* base = dx12.GetNativeCommandList(cmd);
    if (!base) return;

    // Bind HDR colour + depth (read-only). Reuse the engine helper that
    // mirrors what LightingPass uses.
    if (depthTex)
        gfx.SetRenderTargetToHdrWithDepth(depthTex, cmd);

    base->SetGraphicsRootSignature(m_rootSig.Get());
    base->SetPipelineState(m_pso.Get());
    base->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx12.BindDescriptorHeaps(cmd);

    // PerView CB (b0) — same VA the rest of the engine uses.
    if (ID3D12Resource* perViewRes = dx12.GetBufferResource(perViewCB))
        base->SetGraphicsRootConstantBufferView(0, perViewRes->GetGPUVirtualAddress());

    // Root constants at b2: sphereRadius (float) + debugMode (uint).
    // Layout: [radius, debugMode, _pad, _pad].
    union { float f; uint32_t u; } slot[4];
    slot[0].f = sphereRadius;
    slot[1].u = debugMode;
    slot[2].u = 0u;
    slot[3].u = 0u;
    base->SetGraphicsRoot32BitConstants(2, 4, slot, 0);

    auto& volEnts = volPool->Entities();
    auto& volData = volPool->Data();
    for (size_t i = 0; i < volEnts.size(); ++i)
    {
        if (!world.IsAlive(volEnts[i])) continue;
        const DDGIVolumeComponent& vc = volData[i];
        if (!vc.debugDraw) continue;

        DDGIVolumeRuntimeComponent* rt = runPool->Get(volEnts[i]);
        if (!rt || rt->volumeSlot >= DDGI::kMaxVolumes) continue;
        const uint32_t slot = rt->volumeSlot;

        const uint32_t probeCount = mgr.GetProbeCount(slot);
        if (probeCount == 0) continue;

        // Volume CB (b1).
        const RHI::GPUBuffer* cb = mgr.GetVolumeCB(slot);
        if (!cb) continue;
        ID3D12Resource* cbRes = dx12.GetBufferResource(*cb);
        if (!cbRes) continue;
        base->SetGraphicsRootConstantBufferView(1, cbRes->GetGPUVirtualAddress());

        // Probe SH SRV (t0). Sampled at the surface normal of each procedural
        // sphere vertex via DDGI_SH_Irradiance.
        uint64_t shSrv = mgr.GetProbeSHSrv(slot);
        if (shSrv == 0) continue;
        base->SetGraphicsRootDescriptorTable(3,
            D3D12_GPU_DESCRIPTOR_HANDLE{ shSrv });

        // ProbeData SRV (t1, VS only). VS reads pd.offset to draw the sphere
        // at the relocation-adjusted world position so the visualization
        // matches where the probe actually traces from.
        if (uint64_t pdSrv = mgr.GetProbeDataSrv(slot))
            base->SetGraphicsRootDescriptorTable(4,
                D3D12_GPU_DESCRIPTOR_HANDLE{ pdSrv });

        // Procedural UV sphere: 16² quads × 6 verts/quad = 1536 verts per
        // probe. Must match `kSegments` in DDGIProbeDebug.vs.hlsl — change
        // both in lockstep if tessellation gets tuned.
        constexpr uint32_t kVertsPerSphere = 16u * 16u * 6u;
        base->DrawInstanced(kVertsPerSphere, probeCount, 0, 0);
    }
}
