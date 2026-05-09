#pragma once

// TAAJitterState — per-frame Halton sub-pixel jitter + previous-frame VP cache.
// Owns only the math + state; the caller (Renderer::UploadFrameData) drives
// Advance() once per frame, applies GetJitterPixels() to its projection matrix,
// and calls CommitFrame() with the final (unjittered VP, inverse jittered VP)
// pair so TAA can reproject next frame.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

class TAAJitterState
{
public:
    // Halton low-discrepancy sequence — base 2 for X, base 3 for Y.
    // index must be >= 1 for good distribution. Returns value in [0, 1).
    static float Halton(int index, int base)
    {
        float f = 1.0f, r = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(index % base);
            index /= base;
        }
        return r;
    }

    // Advance the jitter index when @p enabled is true; otherwise hold the
    // last jitter at (0,0). Call once per frame before computing the
    // projection matrix. Returns the sub-pixel jitter in units of pixels.
    void Advance(bool enabled)
    {
        if (!enabled) { m_jitterX = 0.0f; m_jitterY = 0.0f; return; }
        const int jIdx = (m_jitterIndex % kCount) + 1;
        m_jitterX = Halton(jIdx, 2) - 0.5f;
        m_jitterY = Halton(jIdx, 3) - 0.5f;
        m_jitterIndex = (m_jitterIndex + 1) % kCount;
    }

    // Apply the current jitter to @p projBase (reversed-Z perspective proj)
    // by offsetting the NDC X/Y by jitter_pixels × 2 / viewport_extent.
    // Operates in row-vector convention (3rd row of the 4×4 matrix).
    DirectX::XMMATRIX ApplyJitterToProjection(DirectX::XMMATRIX projBase,
                                              uint32_t vpW,
                                              uint32_t vpH) const
    {
        using namespace DirectX;
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, projBase);
        f._31 += m_jitterX * 2.0f / static_cast<float>(vpW);
        f._32 -= m_jitterY * 2.0f / static_cast<float>(vpH);
        return XMLoadFloat4x4(&f);
    }

    // Commit the current frame's matrices. The unjittered VP becomes the
    // "previous unjittered" VP for TAA velocity reprojection; the jittered
    // VP becomes the "previous jittered" VP for SSR's reflection-reproject
    // path (so history is sampled at the actual prev jittered pixel grid
    // rather than ~1 px off — the previous bug). The inverse jittered VP
    // is cached for TAA's CB upload path.
    void CommitFrame(DirectX::XMMATRIX curViewProjNoJitter,
                     DirectX::XMMATRIX curViewProjJittered,
                     DirectX::XMMATRIX invJitteredViewProjTransposed)
    {
        DirectX::XMStoreFloat4x4(&m_currVPNoJitter,  curViewProjNoJitter);
        DirectX::XMStoreFloat4x4(&m_currVPJittered,  curViewProjJittered);
        DirectX::XMStoreFloat4x4(&m_invJitteredVP,   invJitteredViewProjTransposed);
    }

    // Call at the END of the frame (after the CBs have been filled) to roll
    // current → previous for next frame. Separating this from CommitFrame()
    // lets Render() sample the "previous" VP after Execute() sees the frame.
    void AdvanceToNextFrame()
    {
        m_prevVPNoJitter = m_currVPNoJitter;
        m_prevVPJittered = m_currVPJittered;
    }

    float GetJitterX() const { return m_jitterX; }
    float GetJitterY() const { return m_jitterY; }

    const DirectX::XMFLOAT4X4& GetPrevViewProjNoJitter() const { return m_prevVPNoJitter; }
    const DirectX::XMFLOAT4X4& GetCurrViewProjNoJitter() const { return m_currVPNoJitter; }
    const DirectX::XMFLOAT4X4& GetPrevViewProjJittered() const { return m_prevVPJittered; }
    const DirectX::XMFLOAT4X4& GetCurrViewProjJittered() const { return m_currVPJittered; }
    const DirectX::XMFLOAT4X4& GetInvViewProj()          const { return m_invJitteredVP;  }

private:
    static constexpr int kCount = 16;

    int                  m_jitterIndex = 0;
    float                m_jitterX     = 0.0f;
    float                m_jitterY     = 0.0f;
    DirectX::XMFLOAT4X4  m_prevVPNoJitter{};
    DirectX::XMFLOAT4X4  m_currVPNoJitter{};
    DirectX::XMFLOAT4X4  m_prevVPJittered{};
    DirectX::XMFLOAT4X4  m_currVPJittered{};
    DirectX::XMFLOAT4X4  m_invJitteredVP{};
};
