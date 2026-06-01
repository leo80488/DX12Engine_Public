#include "RenderGraph/RenderPass/CullingPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include <cstring>

// The culling pass reuses the existing compute root signature (space2) with these slots:
// [0] ROOT_CBV  b0 space2  — CullingCB
// [1] SRV  t0 space2       — GPUInstanceData[]
// [2] SRV  t1 space2       — MeshAABB[]
// [4] UAV  u0 space2       — IndirectDrawCommand[] (output)
// [5] UAV  u1 space2       — drawCount (output)
//
// Note: we reuse the skinning compute root sig slots. This works because culling
// and skinning don't run on the same CL simultaneously.

static constexpr uint32_t kCBSlot         = 0;  // b0 space2
static constexpr uint32_t kInstanceSRV    = 1;  // t0 space2
static constexpr uint32_t kMeshAABBSRV    = 2;  // t1 space2
static constexpr uint32_t kOutCommandsUAV = 4;  // u0 space2
static constexpr uint32_t kOutCountUAV    = 5;  // u1 space2

void CullingPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::InstanceCull_CS, RHI::ShaderStage::CS,
                         "InstanceCull.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::InstanceCull_CS);
    if (!cs) { LOG_ERROR("CullingPass: InstanceCull_CS not found"); return; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("CullingPass: PSO creation failed"); return; }

    // CB
    m_cb.Create(gfx, "Culling.CB");

    LOG_SUCCESS("CullingPass: initialized (GPU frustum culling)");
}

void CullingPass::SetFrustumPlanes(const float frustumPlanes[6][4])
{
    std::memcpy(m_cbData.frustumPlanes, frustumPlanes, sizeof(m_cbData.frustumPlanes));
}

void CullingPass::Execute(RHI::CommandList cl,
                           const RHI::GPUBuffer& instanceBuffer,
                           const RHI::GPUBuffer& meshAABBBuffer,
                           const RHI::GPUBuffer& outArgBuffer,
                           const RHI::GPUBuffer& outCountBuffer)
{
    if (!m_pso.IsValid() || m_instanceCount == 0) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Upload CB
    m_cbData.viewProj      = m_viewProj;
    m_cbData.instanceCount = m_instanceCount;
    if (auto* slot = m_cb.Current(gfx)) *slot = m_cbData;

    // Clear the draw count to 0 (GPU atomic counter).
    // We use a small upload buffer trick: write 0 to a temp and copy.
    // For simplicity, we'll use a UAV clear via a compute helper.
    // TODO: proper UAV clear. For now, the counter is expected to be zeroed externally.

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kInstanceSRV,    gfx.GetBufferSRVGpuHandle(instanceBuffer), cl);
    gfx.SetComputeDescriptorTable(kMeshAABBSRV,    gfx.GetBufferSRVGpuHandle(meshAABBBuffer), cl);
    gfx.SetComputeDescriptorTable(kOutCommandsUAV, gfx.GetBufferUAVGpuHandle(outArgBuffer), cl);
    gfx.SetComputeDescriptorTable(kOutCountUAV,    gfx.GetBufferUAVGpuHandle(outCountBuffer), cl);

    // Dispatch: 64 threads per group
    uint32_t groups = (m_instanceCount + 63) / 64;
    gfx.DispatchCompute(groups, 1, 1, cl);
}
