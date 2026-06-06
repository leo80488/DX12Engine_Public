#include "RenderGraph/RenderPass/DDGIPass.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/MeshDescriptorHeap.h"   // kMaxBuffers — root-sig bindless table size
#include "Graphics/DxcCompiler.h"
#include "Resource/AssetFS.h"   // pak-first read so DDGI shaders load in packed builds
#include "System/Log.h"

#include <vector>
#include <string>

namespace
{
// Read a shader source file via AssetFS (game.ipak first, then loose disk).
// Using raw std::ifstream here used to make DDGI silently disabled in PACKED
// builds — the .hlsl lives only inside game.ipak, so the direct open failed and
// every DDGI shader compile bailed. Returns empty vector on failure.
std::vector<uint8_t> ReadFileBytes(const std::string& path)
{
    std::vector<std::uint8_t> buf;
    if (Resource::AssetFS::Get().ReadFile(path, buf))
        return buf;
    return {};
}

// Compile one shader source file via DXC. Returns the DXIL container bytes.
std::vector<uint8_t> CompileShaderFile(const std::string& path,
                                       RHI::ShaderStage stage,
                                       const std::vector<DxcCompiler::Define>& defines = {},
                                       const std::string& entry = "main")
{
    auto src = ReadFileBytes(path);
    if (src.empty())
    {
        LOG_ERROR("DDGIPass: cannot read shader %s", path.c_str());
        return {};
    }
    DxcCompiler::CompileOptions opts;
    opts.sourceName = path;
    opts.entry      = entry;
    opts.stage      = stage;
    opts.defines    = defines;
    auto res = DxcCompiler::Compile(src.data(), src.size(), opts);
    if (!res.ok)
    {
        LOG_ERROR("DDGIPass: compile %s failed: %s", path.c_str(), res.errorMsg.c_str());
        return {};
    }
    if (!res.errorMsg.empty())
        LOG_INFO("DDGIPass: %s warnings: %s", path.c_str(), res.errorMsg.c_str());
    return res.dxil;
}
} // namespace

// =============================================================================
// Init / Shutdown
// =============================================================================

bool DDGIPass::Init(IGraphicsDevice& gfx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    if (!dx12.SupportsDXR())
    {
        LOG_INFO("DDGIPass: device does not support DXR — pass disabled");
        m_ready = false;
        return false;
    }

    if (!BuildRootSignature(gfx))    return false;
    if (!CreateCSPipelines(gfx))     return false;
    m_ready = true;
    LOG_INFO("DDGIPass: initialised (inline RayQuery CS path)");
    return true;
}

void DDGIPass::Shutdown(IGraphicsDevice& /*gfx*/)
{
    m_tracePSO.Reset();
    m_prepareRayCountPSO.Reset();
    m_rayAllocationPSO.Reset();
    m_finalizeIndirectPSO.Reset();
    m_relocatePSO.Reset();
    m_relightIrradiancePSO.Reset();
    m_relightDepthPSO.Reset();
    m_borderDepthPSO.Reset();
    m_traceCmdSig.Reset();
    m_rootSig.Reset();
    m_ready = false;
}

bool DDGIPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_ready) return false;
    m_tracePSO.Reset();
    m_prepareRayCountPSO.Reset();
    m_rayAllocationPSO.Reset();
    m_finalizeIndirectPSO.Reset();
    m_relocatePSO.Reset();
    m_relightIrradiancePSO.Reset();
    m_relightDepthPSO.Reset();
    m_borderDepthPSO.Reset();
    if (!CreateCSPipelines(gfx))  return false;
    return true;
}

// =============================================================================
// Root signature
// =============================================================================

bool DDGIPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12Device* dev = dx12.GetDevice();
    if (!dev) return false;

    // Layout (DDGI-only):
    //   [0]  ROOT_CBV          b0 space0
    //   [1]  ROOT_SRV          t0 space0  (TLAS)
    //   [2]  DESC_TABLE 1 SRV  t1 space0  (sky IBL cube)
    //   [3]  DESC_TABLE 1 UAV  u0 space0  (atlas / ray data)
    //   [4]  DESC_TABLE 1 SRV  t2 space0  (ray data SRV)
    //   [5]  ROOT_SRV          t3 space0  (per-instance data buffer)
    //   [6]  DESC_TABLE 16384 SRV t0 space1 (bindless g_DDGIBuffers[], matches kMaxBindlessBuffers)
    //   [7]  DESC_TABLE 1 UAV  u1 space0  (variance buffer — irradiance relight only)
    //   [8]  DESC_TABLE 1 SRV  t4 space0  (irradiance atlas SRV — trace's multi-bounce read)
    //   [9]  DESC_TABLE 1 SRV  t5 space0  (depth atlas SRV — trace's multi-bounce read)
    //   [10] DESC_TABLE 1 SRV  t6 space0  (probe data SRV — trace's multi-bounce read)
    //   [11] DESC_TABLE 1 SRV  t7 space0  (cluster GPULight buffer — direct light loop)
    //   [12] DESC_TABLE 1 UAV  u2 space0  (per-probe adaptive ray count)
    //   [13] DESC_TABLE 1 UAV  u3 space0  (ray allocation buffer — total + descriptors)
    //   [14] DESC_TABLE 1 UAV  u4 space0  (dispatch args — written by finalize CS)

    D3D12_ROOT_PARAMETER params[16] = {};

    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0; // t0 — TLAS
    params[1].Descriptor.RegisterSpace  = 0;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    static D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors   = 1;
    ranges[0].BaseShaderRegister = 1; // t1
    ranges[0].RegisterSpace      = 0;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &ranges[0];
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    ranges[1].RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors   = 1;
    ranges[1].BaseShaderRegister = 0; // u0
    ranges[1].RegisterSpace      = 0;
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &ranges[1];
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    ranges[2].RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors   = 1;
    ranges[2].BaseShaderRegister = 2; // t2
    ranges[2].RegisterSpace      = 0;
    params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges   = &ranges[2];
    params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [5] Per-instance data (StructuredBuffer<DDGIInstanceData>) bound by raw GPU VA.
    params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 3; // t3
    params[5].Descriptor.RegisterSpace  = 0;
    params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [6] Bindless ByteAddressBuffer table — same convention as the engine's
    // main rasterizer root sig. Closest-hit indexes it via DDGIInstanceData::
    // vbBindlessIdx / ibBindlessIdx to fetch positions for the geometric
    // face normal.
    static D3D12_DESCRIPTOR_RANGE bindlessRange{};
    bindlessRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    // Must match MeshDescriptorHeap::kMaxBuffers (the shader sees this size as
    // `g_DDGIBuffers[N]` in DDGIRayTrace.cs.hlsl). DDGIPass has its own root
    // signature so the count is inlined here rather than cross-included.
    bindlessRange.NumDescriptors   = MeshDescriptorHeap::kMaxBuffers;
    bindlessRange.BaseShaderRegister = 0;
    bindlessRange.RegisterSpace      = 1;
    params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges   = &bindlessRange;
    params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [7] Variance buffer UAV at u1 — only meaningful for the irradiance
    // relight CS; other dispatches still satisfy the root sig with a dummy
    // bind (the shader simply doesn't reference it).
    static D3D12_DESCRIPTOR_RANGE varianceRange{};
    varianceRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    varianceRange.NumDescriptors     = 1;
    varianceRange.BaseShaderRegister = 1; // u1
    varianceRange.RegisterSpace      = 0;
    params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges   = &varianceRange;
    params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [8..10] Multi-bounce SRV tables — t4 irradiance atlas, t5 depth atlas,
    // t6 probe data. Bound by the trace CS only; relight / border / debug
    // dispatches dummy-bind to keep the root sig satisfied.
    static D3D12_DESCRIPTOR_RANGE mbRanges[3] = {};
    mbRanges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    mbRanges[0].NumDescriptors     = 1;
    mbRanges[0].BaseShaderRegister = 4; // t4
    mbRanges[0].RegisterSpace      = 0;
    mbRanges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    mbRanges[1].NumDescriptors     = 1;
    mbRanges[1].BaseShaderRegister = 5; // t5
    mbRanges[1].RegisterSpace      = 0;
    mbRanges[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    mbRanges[2].NumDescriptors     = 1;
    mbRanges[2].BaseShaderRegister = 6; // t6
    mbRanges[2].RegisterSpace      = 0;
    for (int i = 0; i < 3; ++i)
    {
        params[8 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[8 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[8 + i].DescriptorTable.pDescriptorRanges   = &mbRanges[i];
        params[8 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [11] Cluster GPULight buffer at t7 — used by the trace CS for direct
    // lighting. Other dispatches don't reference t7 but still satisfy the
    // root sig with the same handle.
    static D3D12_DESCRIPTOR_RANGE lightsRange{};
    lightsRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightsRange.NumDescriptors     = 1;
    lightsRange.BaseShaderRegister = 7; // t7
    lightsRange.RegisterSpace      = 0;
    params[11].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges   = &lightsRange;
    params[11].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [12..14] Adaptive ray dispatch UAVs — u2 raycount, u3 rayalloc, u4 dispatchargs.
    static D3D12_DESCRIPTOR_RANGE adaptRanges[3] = {};
    adaptRanges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    adaptRanges[0].NumDescriptors     = 1;
    adaptRanges[0].BaseShaderRegister = 2; // u2
    adaptRanges[0].RegisterSpace      = 0;
    adaptRanges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    adaptRanges[1].NumDescriptors     = 1;
    adaptRanges[1].BaseShaderRegister = 3; // u3
    adaptRanges[1].RegisterSpace      = 0;
    adaptRanges[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    adaptRanges[2].NumDescriptors     = 1;
    adaptRanges[2].BaseShaderRegister = 4; // u4
    adaptRanges[2].RegisterSpace      = 0;
    for (int i = 0; i < 3; ++i)
    {
        params[12 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[12 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[12 + i].DescriptorTable.pDescriptorRanges   = &adaptRanges[i];
        params[12 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [15] Bindless texture table (engine-wide `g_AllTextures[]` at t0 space2,
    // matches GraphicsDX12::kMaxBindlessTextures). Used by closest-hit
    // to sample emissive textures for emission-into-DDGI.
    static D3D12_DESCRIPTOR_RANGE bindlessTexRange{};
    bindlessTexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessTexRange.NumDescriptors   = GraphicsDX12::kMaxBindlessTextures;
    bindlessTexRange.BaseShaderRegister = 0;
    bindlessTexRange.RegisterSpace      = 2;
    params[15].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[15].DescriptorTable.NumDescriptorRanges = 1;
    params[15].DescriptorTable.pDescriptorRanges   = &bindlessTexRange;
    params[15].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Static linear-wrap sampler at s0 space0.
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = (UINT)std::size(params);
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &samp;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        LOG_ERROR("DDGIPass: serialize root sig failed: %s",
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                         IID_PPV_ARGS(&m_rootSig))))
    {
        LOG_ERROR("DDGIPass: CreateRootSignature failed");
        return false;
    }
    return true;
}

// =============================================================================
// Compute PSOs (trace + relight + border)
// =============================================================================

bool DDGIPass::CreateCSPipelines(IGraphicsDevice& gfx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12Device* dev = dx12.GetDevice();

    auto buildCS = [&](const std::string& path,
                       const std::vector<DxcCompiler::Define>& defines,
                       Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO) -> bool
    {
        auto dxil = CompileShaderFile(path, RHI::ShaderStage::CS, defines, "main");
        if (dxil.empty()) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = m_rootSig.Get();
        cd.CS.pShaderBytecode = dxil.data();
        cd.CS.BytecodeLength  = dxil.size();
        cd.NodeMask = 0;
        if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&outPSO))))
        {
            LOG_ERROR("DDGIPass: CreateComputePipelineState failed for %s", path.c_str());
            return false;
        }
        return true;
    };

    using D = DxcCompiler::Define;
    const std::vector<D> defIrr = { D{"DDGI_RELIGHT_TARGET_IRRADIANCE", "1"},
                                    D{"DDGI_RELIGHT_TARGET_DEPTH",      "0"} };
    const std::vector<D> defDep = { D{"DDGI_RELIGHT_TARGET_IRRADIANCE", "0"},
                                    D{"DDGI_RELIGHT_TARGET_DEPTH",      "1"} };

    if (!buildCS("shaders/DDGIRayTrace.cs.hlsl",          {}, m_tracePSO))             return false;
    if (!buildCS("shaders/DDGIPrepareRayCount.cs.hlsl",   {}, m_prepareRayCountPSO))   return false;
    if (!buildCS("shaders/DDGIRayAllocation.cs.hlsl",     {}, m_rayAllocationPSO))     return false;
    if (!buildCS("shaders/DDGIFinalizeIndirect.cs.hlsl",  {}, m_finalizeIndirectPSO))  return false;
    if (!buildCS("shaders/DDGIProbeRelocate.cs.hlsl",     {}, m_relocatePSO))          return false;
    if (!buildCS("shaders/DDGIRelight.cs.hlsl",      defIrr, m_relightIrradiancePSO)) return false;
    if (!buildCS("shaders/DDGIRelight.cs.hlsl",      defDep, m_relightDepthPSO))      return false;
    if (!buildCS("shaders/DDGIBorderUpdate.cs.hlsl", defDep, m_borderDepthPSO))       return false;

    // Command signature for the trace's ExecuteIndirect dispatch — args layout
    // is the standard D3D12_DISPATCH_ARGUMENTS (3 uints), read from offset 0
    // of m_rayAllocBuffer (which is also a UAV elsewhere in the pipeline).
    if (!m_traceCmdSig)
    {
        D3D12_INDIRECT_ARGUMENT_DESC iad{};
        iad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC csd{};
        csd.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs   = &iad;
        csd.NodeMask         = 0;
        if (FAILED(dev->CreateCommandSignature(&csd, nullptr,
                                                IID_PPV_ARGS(&m_traceCmdSig))))
        {
            LOG_ERROR("DDGIPass: CreateCommandSignature failed");
            return false;
        }
    }
    return true;
}

// =============================================================================
// Per-frame execute
// =============================================================================

void DDGIPass::Execute(IGraphicsDevice& gfx,
                       RHI::CommandList cmd,
                       DDGI::DDGIVolumeManager& mgr,
                       uint32_t volumeSlot,
                       D3D12_GPU_VIRTUAL_ADDRESS tlasVA,
                       uint64_t skyIBLSrv,
                       D3D12_GPU_VIRTUAL_ADDRESS matBufVA,
                       uint64_t bindlessBufferTable,
                       uint64_t lightsSrvHandle,
                       bool     onComputeQueue)
{
    if (!m_ready) return;
    if (mgr.GetProbeCount(volumeSlot) == 0) return;

    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12GraphicsCommandList*  base = dx12.GetNativeCommandList(cmd);
    if (!base) return;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd4;
    if (FAILED(base->QueryInterface(IID_PPV_ARGS(&cmd4))))
    {
        LOG_ERROR("DDGIPass: command list does not support DXR (CommandList4)");
        return;
    }

    const uint32_t probeCount   = mgr.GetProbeCount(volumeSlot);
    const uint32_t raysPerProbe = mgr.GetRaysPerProbe(volumeSlot);
    if (probeCount == 0 || raysPerProbe == 0) return;

    cmd4->SetComputeRootSignature(m_rootSig.Get());

    // CBV — volume CB. Resolve GPU VA from the underlying ID3D12Resource via
    // the DX12 backend (IGraphicsDevice does not expose CB GPU VA queries
    // directly).
    {
        // Per-frame ring slot — Tick() writes the matching index above.
        const RHI::GPUBuffer* cb = mgr.GetVolumeCB(gfx, volumeSlot);
        if (!cb || !cb->IsValid())
        {
            LOG_INFO("DDGIPass: skipping volume %u — CB not allocated", volumeSlot);
            return;
        }
        ID3D12Resource* cbRes = dx12.GetBufferResource(*cb);
        if (!cbRes)
        {
            LOG_ERROR("DDGIPass: cannot resolve CB resource for volume %u", volumeSlot);
            return;
        }
        cmd4->SetComputeRootConstantBufferView(0, cbRes->GetGPUVirtualAddress());
    }

    // TLAS.
    cmd4->SetComputeRootShaderResourceView(1, tlasVA);

    // Sky IBL SRV.
    if (skyIBLSrv != 0)
        cmd4->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{ skyIBLSrv });

    // Per-instance data SRV (root param 5, t3 space0). Closest-hit uses this;
    // the relight CS shaders ignore it.
    if (matBufVA != 0)
        cmd4->SetComputeRootShaderResourceView(5, matBufVA);

    // Bindless ByteAddressBuffer table (root param 6, t0 space1). Closest-
    // hit uses it to fetch vertex positions for the geometric face normal.
    // The relight CS / border CS dispatches don't touch it, but keeping the
    // root sig satisfied is cheap.
    if (bindlessBufferTable != 0)
        cmd4->SetComputeRootDescriptorTable(6,
            D3D12_GPU_DESCRIPTOR_HANDLE{ bindlessBufferTable });

    // Cluster GPULight buffer (root param 11, t7 space0) — trace CS picks a
    // random light per ray. lightsSrvHandle == 0 is OK: the trace shader
    // bails when g_Vol.lightCount == 0.
    if (lightsSrvHandle != 0)
        cmd4->SetComputeRootDescriptorTable(11,
            D3D12_GPU_DESCRIPTOR_HANDLE{ lightsSrvHandle });

    // Bind UAVs that persist across all dispatches in this frame.
    if (uint64_t v = mgr.GetVarianceUav(volumeSlot))
        cmd4->SetComputeRootDescriptorTable(7, D3D12_GPU_DESCRIPTOR_HANDLE{ v });
    if (uint64_t rc = mgr.GetRayCountUav(volumeSlot))
        cmd4->SetComputeRootDescriptorTable(12, D3D12_GPU_DESCRIPTOR_HANDLE{ rc });
    if (uint64_t ra = mgr.GetRayAllocUav(volumeSlot))
        cmd4->SetComputeRootDescriptorTable(13, D3D12_GPU_DESCRIPTOR_HANDLE{ ra });
    if (uint64_t da = mgr.GetDispatchArgsUav(volumeSlot))
        cmd4->SetComputeRootDescriptorTable(14, D3D12_GPU_DESCRIPTOR_HANDLE{ da });

    // Bindless texture table (slot 15, t0 space2) — closest-hit samples
    // emissive textures via DDGIInstanceData.emissiveTexIdx.
    if (auto h = dx12.GetBindlessTextureTableHandle(); h.ptr != 0)
        cmd4->SetComputeRootDescriptorTable(15, h);

    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = nullptr;
        cmd4->ResourceBarrier(1, &uavb);
    };

    // Per-step GPU timestamps — sub-steps appear nested under the outer
    // "DDGI" region wrapped by Renderer. Names match the SkyIBL / VolFog
    // convention; multi-volume scenes will see one entry per volume per
    // step (typical case is a single volume).
    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(cmd, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cmd, r);
    };

    // ---- 1a. Prepare per-probe ray count from variance ---------------------
    {
        uint32_t r = pBegin("DDGI.PrepareRayCount");
        cmd4->SetPipelineState(m_prepareRayCountPSO.Get());
        const uint32_t groups = (probeCount + 63u) / 64u;
        cmd4->Dispatch(groups, 1, 1);
        uavBarrier();
        pEnd(r);
    }

    // ---- 1b. Pack ray descriptors via atomic InterlockedAdd ----------------
    {
        uint32_t r = pBegin("DDGI.RayAllocation");
        cmd4->SetPipelineState(m_rayAllocationPSO.Get());
        const uint32_t groups = (probeCount + 63u) / 64u;
        cmd4->Dispatch(groups, 1, 1);
        uavBarrier();
        pEnd(r);
    }

    // ---- 1c. Finalize indirect dispatch args -------------------------------
    {
        uint32_t r = pBegin("DDGI.FinalizeIndirect");
        cmd4->SetPipelineState(m_finalizeIndirectPSO.Get());
        cmd4->Dispatch(1, 1, 1);
        uavBarrier();
        pEnd(r);
    }

    // The dispatch-args buffer is about to be read by ExecuteIndirect in
    // INDIRECT_ARGUMENT state. The descriptor buffer (rayAlloc) stays UAV
    // because the trace shader reads its descriptors via UAV.
    const RHI::GPUBuffer* argsGB = mgr.GetDispatchArgsBuffer(volumeSlot);
    ID3D12Resource*       argsRes = argsGB
        ? dx12.GetBufferResource(*argsGB) : nullptr;
    if (argsRes)
    {
        D3D12_RESOURCE_BARRIER toIndir{};
        toIndir.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toIndir.Transition.pResource   = argsRes;
        toIndir.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toIndir.Transition.StateAfter  = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        toIndir.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &toIndir);
    }

    // Depth atlas state on entry: SRV (last frame's lighting read) or UAV
    // (first frame after creation). The trace CS samples the previous-frame
    // depth atlas for multi-bounce chebyshev visibility — needs SRV. When
    // running on a compute queue, the SRV state is restricted to NON_PIXEL
    // (the only SRV state valid on D3D12_COMMAND_LIST_TYPE_COMPUTE).
    mgr.TransitionVolumeAtlases(gfx, cmd, volumeSlot,
        DDGI::DDGIVolumeManager::AtlasState::SRV, onComputeQueue);

    // SH probe buffer: validator tracks the last EXPLICIT state, not the
    // documented auto-decay-to-COMMON behaviour. So we keep the buffer in a
    // shader-read state at every Execute boundary, and only flip it to UAV
    // inside the relight-write window.
    //
    // The "shader-read" state mask depends on the queue this CL targets:
    //   graphics queue → NON_PIXEL | PIXEL  (matches Lighting.ps's t29 PS read)
    //   compute  queue → NON_PIXEL only     (PIXEL state is invalid on compute)
    //
    // When DDGI runs on the compute queue, Renderer is responsible for issuing
    // an explicit NON_PIXEL → NON_PIXEL|PIXEL promotion barrier on the graphics
    // queue (via DDGIVolumeManager::PromoteAtlasesForGraphicsQueue) before
    // LightingPass binds the SH SRV. The cross-queue fence covers ordering.
    //
    // Initial state (after CreateBuffer's upload-CL transition) is the full
    // NON_PIXEL|PIXEL mask for any SHADER_RESOURCE-bound buffer — see
    // GraphicsDX12::CreateBuffer's `finalState` selection — so the first
    // frame's UAV write barrier (issued below after the trace) starts from
    // the correct state on day one EVEN on the compute queue, because
    // NON_PIXEL is a valid subset of NON_PIXEL|PIXEL for transition purposes
    // (D3D12 accepts the explicit transition NPSR|PSR → UAV on compute as
    // long as the StateBefore reflects what was actually set last).
    const RHI::GPUBuffer* shGB  = mgr.GetProbeSHBuffer(volumeSlot);
    ID3D12Resource*       shRes = shGB ? dx12.GetBufferResource(*shGB) : nullptr;
    const D3D12_RESOURCE_STATES kSHReadState = onComputeQueue
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
         | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // No barrier at the start — the buffer is already in kSHReadState (either
    // from CreateBuffer or from the previous Execute's end barrier below),
    // and that state is compatible with the trace CS's t4 SRV read.

    // ---- 2. Trace ExecuteIndirect ------------------------------------------
    {
        uint32_t r = pBegin("DDGI.Trace");
        uint64_t rayUav = mgr.GetRayDataUav(volumeSlot);
        if (rayUav)
            cmd4->SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{ rayUav });

        // Multi-bounce SRVs — probe SH / depth / probe-data of THIS volume.
        if (uint64_t shSrv = mgr.GetProbeSHSrv(volumeSlot))
            cmd4->SetComputeRootDescriptorTable(8, D3D12_GPU_DESCRIPTOR_HANDLE{ shSrv });
        if (uint64_t dep = mgr.GetDepthAtlasSrv(volumeSlot))
            cmd4->SetComputeRootDescriptorTable(9, D3D12_GPU_DESCRIPTOR_HANDLE{ dep });
        if (uint64_t pd  = mgr.GetProbeDataSrv(volumeSlot))
            cmd4->SetComputeRootDescriptorTable(10, D3D12_GPU_DESCRIPTOR_HANDLE{ pd });

        cmd4->SetPipelineState(m_tracePSO.Get());
        if (m_traceCmdSig && argsRes)
            cmd4->ExecuteIndirect(m_traceCmdSig.Get(), 1, argsRes, 0,
                                  nullptr, 0);
        uavBarrier();
        pEnd(r);
    }

    // Args buffer back to UAV for the next frame's dispatches.
    if (argsRes)
    {
        D3D12_RESOURCE_BARRIER fromIndir{};
        fromIndir.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        fromIndir.Transition.pResource   = argsRes;
        fromIndir.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        fromIndir.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        fromIndir.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &fromIndir);
    }

    // Trace consumed the depth atlas as SRV; relight + border need it as UAV.
    mgr.TransitionVolumeAtlases(gfx, cmd, volumeSlot,
        DDGI::DDGIVolumeManager::AtlasState::UAV);

    // SH probe buffer: trace read it via t4 (kSHReadState matches); relight
    // writes via u0 — flip to UAV.
    if (shRes)
    {
        D3D12_RESOURCE_BARRIER toUav{};
        toUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource   = shRes;
        toUav.Transition.StateBefore = kSHReadState;
        toUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &toUav);
    }

    // ---- 2.5. Probe relocation (push probes out of nearby surfaces) -------
    // ProbeData buffer is kept in kSHReadState (NPSR | PSR on graphics queue,
    // NPSR-only on compute) between Execute calls — same convention as the SH
    // probe buffer — so LightingPass can sample it without a per-frame
    // transition. Inside relocation we flip to UAV, dispatch, restore.
    //
    // Probe relocation reads g_RayData (t2) and g_RayCount (u2) plus the
    // current frame's ProbeData (u0); it computes an EMA-blended offset that
    // pushes each probe out of nearby front-facing surfaces. The volume's
    // enableRelocation flag (bit 1 in g_Vol.flags) gates whether the shader
    // does meaningful work; we always run the dispatch since the gating is
    // shader-side.
    const RHI::GPUBuffer* pdGB  = mgr.GetProbeDataBuffer(volumeSlot);
    ID3D12Resource*       pdRes = pdGB ? dx12.GetBufferResource(*pdGB) : nullptr;
    if (pdRes)
    {
        D3D12_RESOURCE_BARRIER toUav{};
        toUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource   = pdRes;
        toUav.Transition.StateBefore = kSHReadState;
        toUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &toUav);
    }

    if (uint64_t pdUav = mgr.GetProbeDataUav(volumeSlot))
    {
        uint32_t r = pBegin("DDGI.Relocate");
        cmd4->SetPipelineState(m_relocatePSO.Get());
        cmd4->SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{ pdUav });
        if (uint64_t raySrv = mgr.GetRayDataSrv(volumeSlot))
            cmd4->SetComputeRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{ raySrv });
        const uint32_t groups = (probeCount + 63u) / 64u;
        cmd4->Dispatch(groups, 1, 1);
        uavBarrier();
        pEnd(r);
    }

    // Restore ProbeData to kSHReadState so LightingPass + next-frame trace's
    // multi-bounce SRV read just work without further transitions.
    if (pdRes)
    {
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource   = pdRes;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toRead.Transition.StateAfter  = kSHReadState;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &toRead);
    }

    // ---- 3 + 4. Relight — irradiance then depth ----------------------------
    // Variance / raycount / rayalloc bindings persist from the trace setup
    // above; we only need to swap the per-dispatch atlas UAV + ray SRV.
    auto runRelight = [&](Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso,
                          uint64_t atlasUav, uint64_t raySrv)
    {
        cmd4->SetPipelineState(pso.Get());
        if (raySrv)   cmd4->SetComputeRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{ raySrv });
        if (atlasUav) cmd4->SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{ atlasUav });
        cmd4->Dispatch(probeCount, 1, 1);

        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = nullptr;
        cmd4->ResourceBarrier(1, &uavb);
    };
    // Irradiance path now writes to the per-probe SH structured buffer (no
    // longer a Texture2D atlas).
    {
        uint32_t r = pBegin("DDGI.Relight.Irr");
        runRelight(m_relightIrradiancePSO,
                   mgr.GetProbeSHUav(volumeSlot),
                   mgr.GetRayDataSrv(volumeSlot));
        pEnd(r);
    }
    {
        uint32_t r = pBegin("DDGI.Relight.Dep");
        runRelight(m_relightDepthPSO,
                   mgr.GetDepthAtlasUav(volumeSlot),
                   mgr.GetRayDataSrv(volumeSlot));
        pEnd(r);
    }

    // ---- 5 + 6. Border update ----------------------------------------------
    auto runBorder = [&](Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso, uint64_t atlasUav)
    {
        cmd4->SetPipelineState(pso.Get());
        if (atlasUav) cmd4->SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{ atlasUav });
        cmd4->Dispatch(probeCount, 1, 1);

        D3D12_RESOURCE_BARRIER uavb{};
        uavb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavb.UAV.pResource = nullptr;
        cmd4->ResourceBarrier(1, &uavb);
    };
    // Irradiance border CS is no longer needed — SH probes have no octahedral
    // seams. Only the depth atlas still needs border replication for
    // bilinear-correct chebyshev sampling.
    {
        uint32_t r = pBegin("DDGI.Border");
        runBorder(m_borderDepthPSO, mgr.GetDepthAtlasUav(volumeSlot));
        pEnd(r);
    }

    // SH buffer restoration — leave in kSHReadState so:
    //   (a) the lighting pass's t29 PSR read works without a transition,
    //   (b) the next Execute's after-trace barrier sees the StateBefore it
    //       expects (kSHReadState).
    // Without this, the validator carries the UAV state across frames and
    // the next frame's UAV→UAV transition mismatch hangs the device.
    if (shRes)
    {
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource   = shRes;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toRead.Transition.StateAfter  = kSHReadState;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd4->ResourceBarrier(1, &toRead);
    }
}
