#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"   // kClusterCount
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

using namespace DirectX;

// GPU-side decal layout — must match GPUDecal in decal_common.hlsli.
// 192 bytes, 3 cache lines. 9 bindless indices + flags/sortLayer pack in
// the middle section, two float4 scalar packs line up on 16-byte borders.
#pragma pack(push, 4)
struct GPUDecalUpload
{
    float    worldToDecal[16];   // 64  offset 0
    float    boundsCenter[3];    // 12  offset 64
    float    boundsRadius;       // 4   offset 76
    int32_t  texBaseColor;       // 4   offset 80
    int32_t  texNormal;          // 4   offset 84
    int32_t  texOpacity;         // 4   offset 88
    int32_t  texRoughness;       // 4   offset 92
    int32_t  texSpecular;        // 4   offset 96
    int32_t  texAO;              // 4   offset 100
    int32_t  texBump;            // 4   offset 104
    int32_t  texCavity;          // 4   offset 108
    int32_t  texDisplacement;    // 4   offset 112
    uint32_t flags;              // 4   offset 116
    float    sortLayer;          // 4   offset 120
    float    _pad0;              // 4   offset 124 — align the next float4
    float    baseColorTint[4];   // 16  offset 128
    float    scalars0[4];        // 16  offset 144 (opacity, roughness, specular, ao)
    float    scalars1[4];        // 16  offset 160 (normalStrength, bumpStrength, cavityStrength, displacementScale)
    float    decalForwardWS[3];  // 12  offset 176
    float    angleFadeStart;     // 4   offset 188
};
#pragma pack(pop)
static_assert(sizeof(GPUDecalUpload) == 192, "GPUDecalUpload must be 192 bytes");

// Compute root-sig slot constants — must track GraphicsDX12::CreateComputeRootSignature.
// Cull CS uses these:
//   b0 space2 → kCBSlot
//   t0 space2 → cluster AABBs (shared with ClusterPass)
//   t1 space2 → decal buffer
//   u0 space2 → decal index list
//   u1 space2 → decal grid
// Apply CS uses these:
//   b0 space2 → kCBSlot (separate CB from cull)
//   t1 space2 → decal buffer
//   t2 space2 → scene depth
//   t3 space2 → decal grid (as SRV)   (bound through slot 7, t3 space2)
//   t4 space2 → decal index list (SRV) (slot 8, t4 space2)
//   u2/u3/u4 space2 → GBuffer0/1/2
//   t0 space3 → bindless textures (new param 17)
static constexpr uint32_t kCBSlot         = 0;   // b0  space2
static constexpr uint32_t kSRV0_ClusterAABB = 1; // t0  space2
static constexpr uint32_t kSRV1_Decals    = 2;   // t1  space2
static constexpr uint32_t kSRV2_SceneDepth = 3;  // t2  space2
static constexpr uint32_t kUAV0_IndexList = 4;   // u0  space2
static constexpr uint32_t kUAV1_Grid      = 5;   // u1  space2
static constexpr uint32_t kSRV3_Grid      = 7;   // t3  space2 (apply-side grid-as-SRV)
static constexpr uint32_t kSRV4_IndexList = 8;   // t4  space2 (apply-side index-list-as-SRV)
static constexpr uint32_t kUAV2_Albedo    = 14;  // u2  space2
static constexpr uint32_t kUAV3_Normal    = 15;  // u3  space2
static constexpr uint32_t kUAV4_Surface   = 16;  // u4  space2
static constexpr uint32_t kBindless       = 17;  // t0  space3 (new slot, added for decals)

// ---------------------------------------------------------------------------
DecalPass::DecalPass(RG::RGTextureHandle albedo,
                     RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle depth)
    : m_albedoH(albedo), m_normalH(normal), m_surfaceH(surface), m_depthH(depth)
{
}

// ---------------------------------------------------------------------------
void DecalPass::Setup(RG::RenderGraphBuilder& b)
{
    // GBuffer textures: declared as UAV writes (RG transitions to UNORDERED_ACCESS).
    b.WriteUAV(m_albedoH);
    b.WriteUAV(m_normalH);
    b.WriteUAV(m_surfaceH);
    // Scene depth: SRV read (RG transitions to DEPTH_READ_SRV which includes
    // NON_PIXEL_SHADER_RESOURCE — valid for compute).
    b.ReadSRV(m_depthH);
}

// ---------------------------------------------------------------------------
void DecalPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DecalClusterCull_CS, RHI::ShaderStage::CS,
                         "DecalClusterCull.cs.hlsl", "CSCullDecals");
    m_shaderLib.Register(ShaderID::DecalApply_CS, RHI::ShaderStage::CS,
                         "DecalApply.cs.hlsl", "CSApplyDecals");

    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::DecalClusterCull_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_cullPSO))
            LOG_ERROR("DecalPass: cull PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::DecalApply_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_applyPSO))
            LOG_ERROR("DecalPass: apply PSO creation failed");
    }

    // Decal buffer (UPLOAD)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxDecals) * sizeof(GPUDecalUpload);
        bd.stride     = sizeof(GPUDecalUpload);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_decalBuffer))
        {
            m_decalMapped = gfx.MapBuffer(m_decalBuffer);
            m_decalsSRV   = gfx.GetBufferSRVGpuHandle(m_decalBuffer);
        }
    }

    // Decal index list (DEFAULT) — sized to cluster count × per-cluster cap.
    const uint32_t kClusterCount = ClusterPass::kClusterCount;
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * kMaxPerCluster * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_decalIndexBuf))
        {
            m_decalIndexSRV = gfx.GetBufferSRVGpuHandle(m_decalIndexBuf);
            m_decalIndexUAV = gfx.GetBufferUAVGpuHandle(m_decalIndexBuf);
        }
    }

    // Decal grid (DEFAULT)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * 8;  // DecalGridEntry = 2 × uint
        bd.stride     = 8;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_decalGridBuf))
        {
            m_decalGridSRV = gfx.GetBufferSRVGpuHandle(m_decalGridBuf);
            m_decalGridUAV = gfx.GetBufferUAVGpuHandle(m_decalGridBuf);
        }
    }

    // Constant buffers (UPLOAD)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 256;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_cullCB))  m_cullCBMapped  = gfx.MapBuffer(m_cullCB);
        if (gfx.CreateBuffer(bd, m_applyCB)) m_applyCBMapped = gfx.MapBuffer(m_applyCB);
    }

    // Cache the bindless texture table GPU handle (persistent — allocated once
    // at GraphicsDX12 init). Used each frame for the new compute slot 17.
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    m_bindlessTexHandle = dx12.GetBindlessTextureTableHandle().ptr;

    LOG_SUCCESS("DecalPass: initialized (%u max decals, %u clusters, %u per-cluster cap)",
                kMaxDecals, kClusterCount, kMaxPerCluster);
}

// ---------------------------------------------------------------------------
void DecalPass::SetDecals(const std::vector<ResolvedDecal>& decals)
{
    m_decalCount = std::min<uint32_t>(static_cast<uint32_t>(decals.size()), kMaxDecals);
    if (!m_decalMapped || m_decalCount == 0) return;

    auto* dst = static_cast<GPUDecalUpload*>(m_decalMapped);
    for (uint32_t i = 0; i < m_decalCount; ++i)
    {
        const ResolvedDecal& src = decals[i];
        std::memcpy(dst[i].worldToDecal, &src.worldToDecal, 64);
        dst[i].boundsCenter[0]   = src.boundsCenter.x;
        dst[i].boundsCenter[1]   = src.boundsCenter.y;
        dst[i].boundsCenter[2]   = src.boundsCenter.z;
        dst[i].boundsRadius      = src.boundsRadius;
        dst[i].texBaseColor      = src.texBaseColor;
        dst[i].texNormal         = src.texNormal;
        dst[i].texOpacity        = src.texOpacity;
        dst[i].texRoughness      = src.texRoughness;
        dst[i].texSpecular       = src.texSpecular;
        dst[i].texAO             = src.texAO;
        dst[i].texBump           = src.texBump;
        dst[i].texCavity         = src.texCavity;
        dst[i].texDisplacement   = src.texDisplacement;
        dst[i].flags             = src.flags;
        dst[i].sortLayer         = src.sortLayer;
        dst[i]._pad0             = 0.f;
        dst[i].baseColorTint[0]  = src.baseColorTint.x;
        dst[i].baseColorTint[1]  = src.baseColorTint.y;
        dst[i].baseColorTint[2]  = src.baseColorTint.z;
        dst[i].baseColorTint[3]  = src.baseColorTint.w;
        dst[i].scalars0[0]       = src.scalars0.x;
        dst[i].scalars0[1]       = src.scalars0.y;
        dst[i].scalars0[2]       = src.scalars0.z;
        dst[i].scalars0[3]       = src.scalars0.w;
        dst[i].scalars1[0]       = src.scalars1.x;
        dst[i].scalars1[1]       = src.scalars1.y;
        dst[i].scalars1[2]       = src.scalars1.z;
        dst[i].scalars1[3]       = src.scalars1.w;
        dst[i].decalForwardWS[0] = src.decalForwardWS.x;
        dst[i].decalForwardWS[1] = src.decalForwardWS.y;
        dst[i].decalForwardWS[2] = src.decalForwardWS.z;
        dst[i].angleFadeStart    = src.angleFadeStart;
    }
}

// ---------------------------------------------------------------------------
void DecalPass::SetCamera(const DirectX::XMFLOAT4X4& invProj,
                          const DirectX::XMFLOAT4X4& invViewProj,
                          const DirectX::XMFLOAT4X4& viewMatrix,
                          const DirectX::XMFLOAT3&   cameraPosWS,
                          float nearZ, float farZ,
                          uint32_t screenW, uint32_t screenH)
{
    std::memcpy(m_cullCB_data.invProj,    &invProj,    64);
    std::memcpy(m_cullCB_data.viewMatrix, &viewMatrix, 64);
    m_cullCB_data.nearZ      = nearZ;
    m_cullCB_data.farZ       = farZ;
    m_cullCB_data.screenW    = screenW;
    m_cullCB_data.screenH    = screenH;
    m_cullCB_data.decalCount = m_decalCount;

    std::memcpy(m_applyCB_data.invViewProj, &invViewProj, 64);
    std::memcpy(m_applyCB_data.invProj,     &invProj,     64);
    m_applyCB_data.cameraPosWS[0] = cameraPosWS.x;
    m_applyCB_data.cameraPosWS[1] = cameraPosWS.y;
    m_applyCB_data.cameraPosWS[2] = cameraPosWS.z;
    m_applyCB_data.nearZ      = nearZ;
    m_applyCB_data.farZ       = farZ;
    m_applyCB_data.screenW    = screenW;
    m_applyCB_data.screenH    = screenH;
    m_applyCB_data.decalCount = m_decalCount;
}

// ---------------------------------------------------------------------------
RHI::CommandList DecalPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_cullPSO.IsValid() || !m_applyPSO.IsValid() || m_clusterAABBSRV == 0)
        return cl;

    // Skip the full pass when there's nothing to do AND debug heatmap is off —
    // avoids dispatching when the scene has no DecalComponent (hot path for
    // existing scenes without decals). In debug-heatmap mode we still want to
    // run apply so empty clusters get drawn green.
    if (m_decalCount == 0 && !m_debugHeatmap)
        return cl;

    auto& gfx  = static_cast<GraphicsDX12&>(*m_gfx);

    // Refresh CB data written to the persistently-mapped upload buffers.
    m_cullCB_data.decalCount  = m_decalCount;
    m_applyCB_data.decalCount = m_decalCount;
    m_applyCB_data.debugMode  = m_debugHeatmap ? 1u : 0u;
    if (m_cullCBMapped)  std::memcpy(m_cullCBMapped,  &m_cullCB_data,  sizeof(m_cullCB_data));
    if (m_applyCBMapped) std::memcpy(m_applyCBMapped, &m_applyCB_data, sizeof(m_applyCB_data));

    // ---- Pass 1: Cluster cull ------------------------------------------------
    // Skipped when:
    //   - No decals to cull (debug heatmap path still wants apply to run).
    //   - DispatchCull() already ran on an external CL this frame (see
    //     m_externalCullDone). Renderer uses this to run cull on the
    //     ClusterPass CL so it overlaps GBufferPass on the GPU.
    const uint32_t kClusterCount = ClusterPass::kClusterCount;
    if (m_decalCount > 0 && !m_externalCullDone)
    {
        gfx.BindComputePipelineState(m_cullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cullCB, 0, cl);
        gfx.SetComputeDescriptorTable(kSRV0_ClusterAABB, m_clusterAABBSRV, cl);
        gfx.SetComputeDescriptorTable(kSRV1_Decals,      m_decalsSRV,      cl);
        gfx.SetComputeDescriptorTable(kUAV0_IndexList,   m_decalIndexUAV,  cl);
        gfx.SetComputeDescriptorTable(kUAV1_Grid,        m_decalGridUAV,   cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_decalIndexBuf), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_decalGridBuf),  cl);
    }
    // Clear the "done elsewhere" flag — next frame's Renderer can opt in
    // again by calling DispatchCull() before Execute.
    m_externalCullDone = false;

    // ---- Pass 2: Apply to GBuffer -------------------------------------------
    gfx.BindComputePipelineState(m_applyPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_applyCB, 0, cl);

    gfx.SetComputeDescriptorTable(kSRV1_Decals,     m_decalsSRV,     cl);

    // Depth SRV — resolve current physical texture's SRV handle.
    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex = ctx.GetTexture({ m_depthH.id });
    if (!depthTex) return cl;
    uint64_t depthSRV = gfx.GetTextureSRVGpuHandle(*depthTex);
    gfx.SetComputeDescriptorTable(kSRV2_SceneDepth, depthSRV, cl);

    gfx.SetComputeDescriptorTable(kSRV3_Grid,       m_decalGridSRV,  cl);
    gfx.SetComputeDescriptorTable(kSRV4_IndexList,  m_decalIndexSRV, cl);

    // GBuffer UAVs.
    const RHI::Texture* albedoTex  = ctx.GetTexture({ m_albedoH.id  });
    const RHI::Texture* normalTex  = ctx.GetTexture({ m_normalH.id  });
    const RHI::Texture* surfaceTex = ctx.GetTexture({ m_surfaceH.id });
    if (!albedoTex || !normalTex || !surfaceTex) return cl;
    gfx.SetComputeDescriptorTable(kUAV2_Albedo,  gfx.GetTextureUAVGpuHandle(*albedoTex),  cl);
    gfx.SetComputeDescriptorTable(kUAV3_Normal,  gfx.GetTextureUAVGpuHandle(*normalTex),  cl);
    gfx.SetComputeDescriptorTable(kUAV4_Surface, gfx.GetTextureUAVGpuHandle(*surfaceTex), cl);

    // Bindless textures — same heap region graphics uses, exposed on compute
    // via the new root param 17.
    if (m_bindlessTexHandle != 0)
        gfx.SetComputeDescriptorTable(kBindless, m_bindlessTexHandle, cl);

    const uint32_t groupsX = (m_applyCB_data.screenW + 7) / 8;
    const uint32_t groupsY = (m_applyCB_data.screenH + 7) / 8;
    gfx.DispatchCompute(groupsX, groupsY, 1, cl);

    // UAV barriers on the three GBuffer textures so LightingPass sees the
    // writes before it transitions them to SHADER_RESOURCE. The graph will
    // emit the UAV→SRV transition itself; the MEMORY barrier here just
    // serialises pending UAV writes within this command list.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(albedoTex),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(normalTex),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(surfaceTex), cl);

    return cl;
}

// ---------------------------------------------------------------------------
// DispatchCull — record the cluster-cull compute on an external CL.
//
// Used by Renderer to run cull on the SAME graphics CL that hosts
// ClusterPass, before the render graph starts. Because ClusterPass runs
// before GBufferPass, and same-queue CLs naturally order on the GPU, the
// cull dispatch ends up overlapping GBufferPass rasterisation — i.e. a
// cheap approximation of "async compute" without a separate compute queue
// and without cross-queue fences.
//
// Data flow:
//   CPU writes m_decalCount + CB + decal buffer in Renderer::BuildScene_UploadLights.
//   → DispatchCull(cullCL) records cluster cull dispatch + UAV barriers.
//   → Execute(graphCL) sees m_externalCullDone, skips its own cull, runs apply.
// Graph CL already depends on cullCL via Renderer's CL chain so
// m_decalIndexBuf / m_decalGridBuf writes are visible to the apply CS.
// ---------------------------------------------------------------------------
void DecalPass::DispatchCull(RHI::CommandList cl)
{
    if (!m_gfx || !m_cullPSO.IsValid() || m_clusterAABBSRV == 0 || m_decalCount == 0)
        return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);

    // Refresh cull-side CB — camera was set in SetCamera(), decalCount is live.
    m_cullCB_data.decalCount = m_decalCount;
    if (m_cullCBMapped)
        std::memcpy(m_cullCBMapped, &m_cullCB_data, sizeof(m_cullCB_data));

    gfx.BindComputePipelineState(m_cullPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cullCB, 0, cl);
    gfx.SetComputeDescriptorTable(kSRV0_ClusterAABB, m_clusterAABBSRV, cl);
    gfx.SetComputeDescriptorTable(kSRV1_Decals,      m_decalsSRV,      cl);
    gfx.SetComputeDescriptorTable(kUAV0_IndexList,   m_decalIndexUAV,  cl);
    gfx.SetComputeDescriptorTable(kUAV1_Grid,        m_decalGridUAV,   cl);

    gfx.DispatchCompute((ClusterPass::kClusterCount + 63) / 64, 1, 1, cl);

    // UAV barriers so the end of this CL flushes writes to memory before
    // the next CL that reads the buffers as SRV. Same-queue CL ordering +
    // these barriers give RAW safety without needing a typed transition.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_decalIndexBuf), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_decalGridBuf),  cl);

    m_externalCullDone = true;
}
