#pragma once

// RenderContext — per-pass execution context built by RenderGraph each frame.
//
// Provides:
//   - Resolved RHI texture handles for all virtual textures declared at Setup.
//   - Named constant buffer handles supplied by the Renderer before Execute.
//   - The draw list (sorted DrawPackets) for passes that issue draw calls.
//
// The command list is NO LONGER stored here.  Each pass receives its pre-opened
// RHI::CommandList directly as a parameter to Execute() and wraps it in an
// RHICommandList for type-safe recording.
//
// No DX12 types appear here.  Passes MUST NOT cache any handle across frames.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/RenderTypes.h"     // DrawPacket, DrawList, DrawFilter
#include "RenderGraph/RGTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RG
{
    class RenderContext
    {
    public:
        // ---- Resolved texture handles ---------------------------------------
        const RHI::Texture*   GetTexture(RGTextureHandle h) const;

        // ---- Named constant buffer handles ----------------------------------
        const RHI::GPUBuffer* GetCB(const char* name) const;

        // ---- Named GPU buffer handles (for root SRV binding) ----------------
        const RHI::GPUBuffer* GetBuffer(const char* name) const;

        // ---- Bindless descriptor table handle (g_Buffers[]) -----------------
        uint64_t GetBindlessTableHandle() const { return m_bindlessTableHandle; }

        // ---- Draw list — sorted packets produced by Renderer::BuildRenderScene
        // Returns all packets that belong to @p filter (contiguous in sorted order).
        DrawList GetDrawList(DrawFilter filter) const;

        // ---- Render dimensions ----------------------------------------------
        uint32_t GetWidth()  const { return m_width; }
        uint32_t GetHeight() const { return m_height; }

        // =====================================================================
        // Builder interface — used exclusively by RenderGraph. Do not call from passes.
        // =====================================================================
        void SetTexture(RGTextureHandle h, const RHI::Texture& tex);
        void SetCB(const char* name, const RHI::GPUBuffer& buffer);
        void SetBuffer(const char* name, const RHI::GPUBuffer& buffer);
        void SetBindlessTableHandle(uint64_t handle) { m_bindlessTableHandle = handle; }
        void SetDrawList(const std::vector<DrawPacket>* packets) { m_drawPackets = packets; }
        void SetDimensions(uint32_t w, uint32_t h)  { m_width  = w; m_height = h; }
        void Clear();

    private:
        uint32_t         m_width  = 0;
        uint32_t         m_height = 0;

        const std::vector<DrawPacket>*                         m_drawPackets = nullptr;
        uint64_t                                               m_bindlessTableHandle = 0;
        std::unordered_map<uint32_t, RHI::Texture>             m_textureMap;
        std::unordered_map<std::string, const RHI::GPUBuffer*> m_cbMap;
        std::unordered_map<std::string, const RHI::GPUBuffer*> m_bufferMap;
    };

} // namespace RG
