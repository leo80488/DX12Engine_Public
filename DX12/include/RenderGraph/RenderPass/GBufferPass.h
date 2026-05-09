#pragma once

// GBufferPass — PBR geometry pass writing albedo, normals, surface data, and depth.
//               World position is reconstructed from depth in LightingPass.
//
// Responsibility:
//   - Owns its ShaderLibrary (GBuffer_VS / GBuffer_PS) and PSOCache.
//   - Selects PSO per-draw via DrawPacket::permutation → PSOCache::GetOrCreate.
//   - Batches draws with the same PSO (draw list pre-sorted by permutation).
//   - Does NOT know about scene geometry; all mesh data comes from DrawPackets.
//   - Binds the per-frame MaterialBuffer (StructuredBuffer<MaterialGPUData>)
//     at t2 space0 so the PS can read bindless material data.
//
// Parallel recording:
//   When draw count >= kMinDrawsPerChunk * 2, the draw list is split into up to
//   kMaxChunks contiguous ranges.  Each range is recorded on its own command list
//   on a TaskSystem::High worker thread, concurrently with the other chunks.
//   The primary CL (passed into Execute) handles only clears; all draw CLs are
//   submitted after it on the same GRAPHICS queue, so GPU ordering is preserved.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/RenderTypes.h"

namespace RG { class RenderContext; }

class GBufferPass : public RG::RenderPass
{
public:
    GBufferPass(RG::RGTextureHandle albedo,
                RG::RGTextureHandle normal,
                RG::RGTextureHandle surface,
                RG::RGTextureHandle depth,
                RG::RGTextureHandle velocity,
                RG::RGTextureHandle emissive);

    const char* GetName() const override { return "GBufferPass"; }
    void Setup  (RG::RenderGraphBuilder& b)           override;
    void Init   (IGraphicsDevice& gfx)                override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx)          override;

    // ExecuteIndirect path: set by Renderer each frame when GPU culling is active.
    void SetIndirectDraw(const RHI::GPUBuffer* argBuf,
                         const std::vector<IndirectGroup>& groups)
    {
        m_indirectArgBuffer = argBuf;
        m_indirectGroups    = groups;
    }
    void ClearIndirectDraw() { m_indirectArgBuffer = nullptr; m_indirectGroups.clear(); }

    // Expose the owned ShaderLibrary so upstream code (Renderer::BuildRenderScene)
    // can RegisterDynamic material-provided custom PSes into the same registry
    // that PSOCache consults when it resolves psID → bytecode.
    ShaderLibrary& GetShaderLibrary() { return m_shaderLib; }

    // Minimum number of draws per chunk. Below this threshold the pass records
    // everything on a single CL to avoid TaskSystem overhead.
    static constexpr int kMinDrawsPerChunk = 16;
    // Maximum number of parallel recording chunks (= parallel command lists).
    static constexpr int kMaxChunks        = 4;

    // Exposes the 1×1 white SRV other passes can borrow as a fallback texture
    // (e.g. probe capture's t3 base-colour slot when a draw has no bindless map).
    uint64_t GetDefaultWhiteSrvHandle()      const { return m_defaultWhiteGpuHandle; }
    // 1×1 flat-normal SRV (RGB = 0.5, 0.5, 1.0) — neutral tangent-space
    // normal, fallback for the t5 normal-map slot when a draw lacks one.
    uint64_t GetDefaultFlatNormalSrvHandle() const { return m_defaultFlatNormalGpuHandle; }

private:
    // customPSID==0 keeps the default GBuffer_PS. Non-zero values come from
    // ShaderLibrary::RegisterDynamic (material's custom shader path) and
    // slot directly into PSODesc::psID — PSOCache hashes them the same way
    // as static ShaderIDs.
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID = 0) const;

    // Bind all per-frame global resources onto @p cl (heaps, CBs, SRVs, samplers).
    // Called once per PSO switch inside RecordChunk.
    void BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const;

    // Record a contiguous slice [begin, end) of @p draws onto @p cl.
    // Safe to call from multiple TaskSystem worker threads simultaneously as long
    // as each invocation uses a distinct command list.
    void RecordChunk(RHI::CommandList cl, DrawList draws, size_t begin, size_t end);

    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    // Default 1×1 white texture (fallback for base-color and surface map slots).
    RHI::Texture m_defaultWhite;
    uint64_t     m_defaultWhiteGpuHandle = 0;

    // Default 1×1 flat-normal texture (RGB=0.5,0.5,1.0 → tangent-space (0,0,1)).
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;

    // Linear-wrap sampler bound at s0 space0 for texture sampling in the PS.
    int m_linearSamplerIdx = -1;

    // ExecuteIndirect path (set per-frame by Renderer).
    const RHI::GPUBuffer*           m_indirectArgBuffer = nullptr;
    std::vector<::IndirectGroup>    m_indirectGroups;
};
