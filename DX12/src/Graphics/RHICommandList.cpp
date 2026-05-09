#include "Graphics/GraphicsStruct.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cassert>

// ============================================================================
// RHI::CommandList — recording method implementations
// ============================================================================

namespace RHI
{

IGraphicsDevice& CommandList::GetDevice() const
{
    assert(gfx && "CommandList::GetDevice called on bare handle (gfx == null)");
    return *gfx;
}

RG::RenderContext& CommandList::GetContext() const
{
    assert(ctx && "CommandList::GetContext called on bare handle (ctx == null)");
    return *ctx;
}

// ---- Viewport / scissor / topology -----------------------------------------

void CommandList::SetViewport(uint32_t width, uint32_t height)
{
    RHI::Viewport vp;
    vp.width  = static_cast<float>(width  ? width  : (ctx ? ctx->GetWidth()  : gfx->GetWidth()));
    vp.height = static_cast<float>(height ? height : (ctx ? ctx->GetHeight() : gfx->GetHeight()));
    gfx->SetViewport(vp, *this);
}

void CommandList::SetScissorRect(uint32_t width, uint32_t height)
{
    const uint32_t w = width  ? width  : (ctx ? ctx->GetWidth()  : gfx->GetWidth());
    const uint32_t h = height ? height : (ctx ? ctx->GetHeight() : gfx->GetHeight());
    gfx->SetScissorRect(0, 0, w, h, *this);
}

void CommandList::SetPrimitiveTopology(RHI::PrimitiveTopology topo)
{
    gfx->SetPrimitiveTopology(topo, *this);
}

// ---- Pipeline state --------------------------------------------------------

void CommandList::SetPipelineState(const RHI::PipelineState& pso)
{
    gfx->BindPipelineState(pso, *this);
}

// ---- Render targets — RG virtual-handle variants ---------------------------

void CommandList::SetRenderTarget(RG::RGTextureHandle rtv, RG::RGTextureHandle dsv)
{
    const RHI::Texture* rtvTex = ctx->GetTexture(rtv);
    const RHI::Texture* dsvTex = dsv.IsValid() ? ctx->GetTexture(dsv) : nullptr;
    if (rtvTex)
        gfx->SetRenderTargets(1, &rtvTex, dsvTex, *this);
}

void CommandList::SetRenderTargets(std::initializer_list<RG::RGTextureHandle> rtvs,
                                    RG::RGTextureHandle dsv)
{
    const RHI::Texture* rtvPtrs[8];
    uint32_t count = 0;
    for (auto h : rtvs)
    {
        if (count >= 8) break;
        rtvPtrs[count++] = ctx->GetTexture(h);
    }
    const RHI::Texture* dsvTex = dsv.IsValid() ? ctx->GetTexture(dsv) : nullptr;
    gfx->SetRenderTargets(count, rtvPtrs, dsvTex, *this);
}

void CommandList::SetRenderTargetsAndHdr(std::initializer_list<RG::RGTextureHandle> rtvs,
                                          RG::RGTextureHandle dsv)
{
    const RHI::Texture* rtvPtrs[8];
    uint32_t count = 0;
    for (auto h : rtvs)
    {
        if (count >= 7) break;   // reserve last slot for HDR append
        rtvPtrs[count++] = ctx->GetTexture(h);
    }
    const RHI::Texture* dsvTex = dsv.IsValid() ? ctx->GetTexture(dsv) : nullptr;
    gfx->SetRenderTargetsAndHdr(count, rtvPtrs, dsvTex, *this);
}

void CommandList::ClearRenderTarget(RG::RGTextureHandle h, const float color[4])
{
    const RHI::Texture* tex = ctx->GetTexture(h);
    if (tex && tex->IsValid())
        gfx->ClearRenderTarget(*tex, color, *this);
}

void CommandList::ClearHdrRenderTarget(const float color[4])
{
    gfx->ClearHdrRenderTarget(color, *this);
}

void CommandList::ClearDepthStencil(RG::RGTextureHandle h, float depth, uint8_t stencil)
{
    const RHI::Texture* tex = ctx->GetTexture(h);
    if (tex && tex->IsValid())
        gfx->ClearDepthStencil(*tex, depth, stencil, *this);
}

// ---- Render targets — raw RHI::Texture* variants ---------------------------

void CommandList::SetRenderTargets(uint32_t numRTs,
                                    const RHI::Texture* const* rtvs,
                                    const RHI::Texture* dsv)
{
    gfx->SetRenderTargets(numRTs, rtvs, dsv, *this);
}

void CommandList::ClearRenderTarget(const RHI::Texture& tex, const float color[4])
{
    gfx->ClearRenderTarget(tex, color, *this);
}

void CommandList::ClearDepthStencil(const RHI::Texture& tex, float depth, uint8_t stencil)
{
    gfx->ClearDepthStencil(tex, depth, stencil, *this);
}

// ---- Descriptor heap / root bindings ---------------------------------------

void CommandList::BindDescriptorHeaps()
{
    gfx->BindDescriptorHeaps(*this);
}

void CommandList::BindCBByName(uint32_t slot, const char* name)
{
    const RHI::GPUBuffer* cb = ctx->GetCB(name);
    if (!cb || !cb->IsValid())
    {
        LOG_ERROR("CommandList::BindCBByName: '%s' not registered", name);
        return;
    }
    gfx->BindConstantBuffer(*cb, slot, *this);
}

void CommandList::BindSRVByHandle(uint32_t slot, RG::RGTextureHandle h)
{
    const RHI::Texture* tex = ctx->GetTexture(h);
    if (tex && tex->IsValid())
        gfx->BindResource(*tex, slot, *this);
}

void CommandList::BindSampler(uint32_t slot, int samplerIndex)
{
    gfx->BindSampler(samplerIndex, slot, *this);
}

void CommandList::SetPVFRootConstants(uint32_t meshDescIdx,
                                       uint32_t instanceOffset,
                                       uint32_t materialIndex)
{
    gfx->SetRootConstants(meshDescIdx, instanceOffset, materialIndex, *this);
}

void CommandList::BindBufferSRVByName(uint32_t rootSlot, const char* name)
{
    const RHI::GPUBuffer* buf = ctx->GetBuffer(name);
    if (!buf || !buf->IsValid())
    {
        LOG_ERROR("CommandList::BindBufferSRVByName: '%s' not registered", name);
        return;
    }
    gfx->SetRootBufferSRV(*buf, rootSlot, *this);
}

void CommandList::BindBufferSRV(uint32_t slot, const GPUBuffer& buf)
{
    if (buf.IsValid())
        gfx->BindResource(buf, slot, *this);
}

void CommandList::BindDescriptorTableHandle(uint32_t rootSlot, uint64_t gpuHandle)
{
    gfx->BindDescriptorTableGpuHandle(rootSlot, gpuHandle, *this);
}

// ---- Draw calls ------------------------------------------------------------

void CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                                 uint32_t startVertex, uint32_t startInstance)
{
    gfx->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance, *this);
}

void CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                        uint32_t startIndex, int32_t baseVertex,
                                        uint32_t startInstance)
{
    gfx->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex,
                              startInstance, *this);
}

void CommandList::DrawFullscreenTriangle()
{
    gfx->SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, *this);
    gfx->DrawInstanced(3, 1, 0, 0, *this);
}

void CommandList::DispatchMesh(uint32_t x, uint32_t y, uint32_t z)
{
    gfx->DispatchMesh(x, y, z, *this);
}

// ---- Barriers / copies -----------------------------------------------------

void CommandList::PushBarrier(const RHI::GPUBarrier& barrier)
{
    gfx->PushBarrier(barrier, *this);
}

void CommandList::CopyTexturePixelToBuffer(const RHI::Texture& src,
                                            uint32_t srcX, uint32_t srcY,
                                            RHI::GPUBuffer& dst)
{
    gfx->CopyTexturePixelToBuffer(src, srcX, srcY, dst, *this);
}

// ---- Dimension / bindless queries ------------------------------------------

uint32_t CommandList::GetWidth()  const { return ctx ? ctx->GetWidth()  : gfx->GetWidth();  }
uint32_t CommandList::GetHeight() const { return ctx ? ctx->GetHeight() : gfx->GetHeight(); }

uint64_t CommandList::GetBindlessTableHandle() const
{
    return ctx->GetBindlessTableHandle();
}

} // namespace RHI
