#pragma once

// FrameCB<T> — kFrameCount-deep ring of persistently-mapped UPLOAD constant
// buffers. Solves the CPU/GPU race that breaks triple-buffered pipelining:
// a single mapped CB written every frame would be overwritten by frame N+1's
// CPU while frame N's GPU is still reading it. Each slot is a separate
// physical buffer, indexed by gfx.GetFrameIndex().
//
// Replaces the historical "single CB + persistent map" pattern (one
// RHI::GPUBuffer + one void* mapped pointer) used by ~22 passes.
//
// Usage:
//   FrameCB<MyCB> m_cb;
//   Init:    m_cb.Create(gfx, "MyPass.CB");
//   Execute: *m_cb.Current(gfx) = liveData;
//            gfx.SetComputeRootCBV(slot, m_cb.CurrentBuffer(gfx), cl);
//   Destroy: m_cb.Destroy(gfx);

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"

#include <cassert>

template <typename T>
struct FrameCB
{
    // Must match GraphicsDX12::FrameCount. Hard-coded to avoid pulling in
    // the DX12 header — FrameCount is a stable engine-wide constant.
    static constexpr uint32_t kFrameCount = 3;

    RHI::GPUBuffer  buffers[kFrameCount];
    T*              mapped [kFrameCount] = {};

    bool Create(IGraphicsDevice& gfx, const char* /*debugName*/ = nullptr)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            RHI::GPUBufferDesc d{};
            // CBs are 256-aligned per D3D12 spec.
            const uint64_t paddedSize = (sizeof(T) + 255ull) & ~255ull;
            d.size       = paddedSize;
            d.usage      = RHI::Usage::UPLOAD;
            d.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
            if (!gfx.CreateBuffer(d, buffers[i])) return false;
            mapped[i] = static_cast<T*>(gfx.MapBuffer(buffers[i]));
            if (!mapped[i]) return false;
        }
        return true;
    }

    void Destroy(IGraphicsDevice& gfx)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (mapped[i])           { gfx.UnmapBuffer(buffers[i]); mapped[i] = nullptr; }
            if (buffers[i].IsValid()) gfx.DestroyBuffer(buffers[i]);
            buffers[i] = {};
        }
    }

    bool IsValid() const { return buffers[0].IsValid(); }

    // Current-frame slot accessors. Caller MUST ensure BeginFrame ran (so
    // gfx.GetFrameIndex() reflects the current backbuffer slot).
    T*                    Current      (IGraphicsDevice& gfx)
    {
        const uint32_t s = gfx.GetFrameIndex();
        assert(s < kFrameCount && "GetFrameIndex out of range");
        return mapped[s];
    }
    const RHI::GPUBuffer& CurrentBuffer(IGraphicsDevice& gfx) const
    {
        const uint32_t s = gfx.GetFrameIndex();
        assert(s < kFrameCount && "GetFrameIndex out of range");
        return buffers[s];
    }
};
