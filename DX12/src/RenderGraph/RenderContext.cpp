#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

namespace RG
{
    const RHI::Texture* RenderContext::GetTexture(RGTextureHandle h) const
    {
        auto it = m_textureMap.find(h.id);
        if (it == m_textureMap.end())
        {
            LOG_ERROR("RenderContext: Texture not found for handle %u", h.id);
            return nullptr;
        }
        return &it->second;
    }

    const RHI::GPUBuffer* RenderContext::GetCB(const char* name) const
    {
        auto it = m_cbMap.find(name);
        if (it == m_cbMap.end())
        {
            LOG_ERROR("RenderContext: CB '%s' not found", name);
            return nullptr;
        }
        return it->second;
    }

    DrawList RenderContext::GetDrawList(DrawFilter filter) const
    {
        if (!m_drawPackets || m_drawPackets->empty()) return {};

        const DrawPacket* begin = m_drawPackets->data();
        const DrawPacket* end   = begin + m_drawPackets->size();

        // Packets are sorted by filter (primary key in SortAndBatch).
        // Find the contiguous range matching this filter.
        const DrawPacket* first = begin;
        while (first != end && first->filter != filter) ++first;
        const DrawPacket* last = first;
        while (last != end && last->filter == filter) ++last;

        return { first, static_cast<size_t>(last - first) };
    }

    void RenderContext::SetTexture(RGTextureHandle h, const RHI::Texture& tex)
    {
        m_textureMap[h.id] = tex;
    }

    const RHI::GPUBuffer* RenderContext::GetBuffer(const char* name) const
    {
        auto it = m_bufferMap.find(name);
        if (it == m_bufferMap.end())
        {
            LOG_ERROR("RenderContext: Buffer '%s' not found", name);
            return nullptr;
        }
        return it->second;
    }

    void RenderContext::SetCB(const char* name, const RHI::GPUBuffer& buffer)
    {
        m_cbMap[name] = &buffer;
    }

    void RenderContext::SetBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        m_bufferMap[name] = &buffer;
    }

    void RenderContext::Clear()
    {
        m_width               = 0;
        m_height              = 0;
        m_drawPackets         = nullptr;
        m_bindlessTableHandle = 0;
        m_textureMap.clear();
        m_cbMap.clear();
        m_bufferMap.clear();
    }

} // namespace RG
