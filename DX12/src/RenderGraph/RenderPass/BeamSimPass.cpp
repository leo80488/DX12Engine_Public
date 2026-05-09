#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/BeamSystem.h"
#include "System/Log.h"

// Compute root signature slots (must match GraphicsDX12::CreateComputeRootSignature).
// Mirrors ParticleSimPass / SkinningPass slot conventions.
//   slot 0 (b0 space2)  — BeamGenParams CBV
//   slot 1 (t0 space2)  — control points SRV
//   slot 4 (u0 space2)  — pos UAV
//   slot 5 (u1 space2)  — normal UAV
//   slot 14 (u2 space2) — tangent UAV
//   slot 15 (u3 space2) — uv UAV
static constexpr uint32_t kCSCBSlot         = 0;
static constexpr uint32_t kCSCPSlot         = 1;
static constexpr uint32_t kCSPosUavSlot     = 4;
static constexpr uint32_t kCSNormalUavSlot  = 5;
static constexpr uint32_t kCSTangentUavSlot = 14;
static constexpr uint32_t kCSUvUavSlot      = 15;

void BeamSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BeamTubeGen_CS, RHI::ShaderStage::CS,
                         "BeamTubeGen.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BeamTubeGen_CS);
    if (!cs)
    {
        LOG_ERROR("BeamSimPass: BeamTubeGen_CS shader lookup failed");
        return;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_genPSO))
        LOG_ERROR("BeamSimPass: gen PSO creation failed");

    LOG_SUCCESS("BeamSimPass: initialised");
}

void BeamSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_genPSO.IsValid()) return;
    const auto& active = m_sys->GetActiveBeams();
    if (active.empty()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    gfx.BindComputePipelineState(m_genPSO, cl);

    // SRV + 4 UAVs are constant across all beams; bind once.
    gfx.SetComputeDescriptorTable(kCSCPSlot,         m_sys->GetControlPointsSrv(), cl);
    gfx.SetComputeDescriptorTable(kCSPosUavSlot,     m_sys->GetPosUav(),     cl);
    gfx.SetComputeDescriptorTable(kCSNormalUavSlot,  m_sys->GetNormalUav(),  cl);
    gfx.SetComputeDescriptorTable(kCSTangentUavSlot, m_sys->GetTangentUav(), cl);
    gfx.SetComputeDescriptorTable(kCSUvUavSlot,      m_sys->GetUvUav(),      cl);

    // Per-beam CBV swap + dispatch. Threadgroup count = ceil(axialRingCount / 8).
    constexpr uint32_t kGroupCount =
        (BeamSystem::kAxialRingCount + 7) / 8;

    for (const auto& ab : active)
    {
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetParamsBuffer(), ab.cbOffset, cl);
        gfx.DispatchCompute(kGroupCount, 1, 1, cl);
    }

    // UAV→SRV barrier so the GBuffer/Transparent passes see the freshly
    // written vertex data when they fetch via the bindless SRV view of the
    // same buffers. (Same buffer is bound as both UAV and SRV in different
    // descriptor heaps; D3D12 needs the explicit barrier.)
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPosBuffer()),     cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetNormalBuffer()),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetTangentBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetUvBuffer()),      cl);
}
