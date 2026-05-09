#include "UI/UIDrawList.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace UI
{
    static UIDrawList::IFontProvider* g_fontProvider = nullptr;
    void UIDrawList::SetGlobalFontProvider(IFontProvider* fp) { g_fontProvider = fp; }

    static constexpr Rect kInfRect{ Vec2{ -1e6f, -1e6f }, Vec2{ 1e6f, 1e6f } };

    static Rect IntersectRect(const Rect& a, const Rect& b)
    {
        Rect r;
        r.mn.x = std::max(a.mn.x, b.mn.x);
        r.mn.y = std::max(a.mn.y, b.mn.y);
        r.mx.x = std::min(a.mx.x, b.mx.x);
        r.mx.y = std::min(a.mx.y, b.mx.y);
        if (r.mx.x < r.mn.x) r.mx.x = r.mn.x;
        if (r.mx.y < r.mn.y) r.mx.y = r.mn.y;
        return r;
    }

    void UIDrawList::Clear()
    {
        m_verts.clear();
        m_indices.clear();
        m_cmds.clear();
        m_clipStack.clear();
        m_texStack.clear();
        m_xformStack.clear();
    }

    void UIDrawList::PushClipRect(const Rect& r)
    {
        if (m_clipStack.empty()) m_clipStack.push_back(r);
        else                     m_clipStack.push_back(IntersectRect(m_clipStack.back(), r));
    }
    void UIDrawList::PopClipRect()
    {
        if (!m_clipStack.empty()) m_clipStack.pop_back();
    }
    Rect UIDrawList::CurrentClipRect() const
    {
        return m_clipStack.empty() ? kInfRect : m_clipStack.back();
    }

    void UIDrawList::PushTexture(const UITextureRef& tex) { m_texStack.push_back(tex); }
    void UIDrawList::PopTexture()
    {
        if (!m_texStack.empty()) m_texStack.pop_back();
    }

    void UIDrawList::PushTransform(const Vec2& pivot, const Vec2& scale)
    {
        m_xformStack.push_back({ pivot, scale });
    }
    void UIDrawList::PopTransform()
    {
        if (!m_xformStack.empty()) m_xformStack.pop_back();
    }

    // Walk the stack innermost-first (last pushed first), mirroring OpenGL
    // pushMatrix semantics: P_world = T_outer * T_inner * P_local.
    Vec2 UIDrawList::TransformPoint(const Vec2& p) const
    {
        if (m_xformStack.empty()) return p;
        Vec2 q = p;
        for (auto it = m_xformStack.rbegin(); it != m_xformStack.rend(); ++it)
        {
            q.x = (q.x - it->pivot.x) * it->scale.x + it->pivot.x;
            q.y = (q.y - it->pivot.y) * it->scale.y + it->pivot.y;
        }
        return q;
    }

    void UIDrawList::EnsureCmd(const UITextureRef& tex, uint32_t materialID)
    {
        const Rect clip = CurrentClipRect();
        // Merge into the previous cmd ONLY when the active transform stack
        // is empty AND clip / texture / material match. With a transform
        // active each primitive's vertex coords are already baked through
        // TransformPoint, so merging is technically still correct — but a
        // coexistence regression made earlier transforms stop showing when
        // a screen-space (no-transform) cmd preceded them. Splitting on the
        // transform-stack-depth boundary sidesteps the issue with negligible
        // perf cost (1-2 extra cmds per UI Root).
        if (!m_cmds.empty() && m_xformStack.empty())
        {
            UIDrawCmd& last = m_cmds.back();
            if (last.texture.srvGpuHandle == tex.srvGpuHandle
                && last.materialID == materialID
                && std::memcmp(&last.clipRect, &clip, sizeof(Rect)) == 0)
            {
                return; // mergeable
            }
        }
        UIDrawCmd cmd{};
        cmd.indexOffset = static_cast<uint32_t>(m_indices.size());
        cmd.indexCount  = 0;
        cmd.clipRect    = clip;
        cmd.texture     = tex;
        cmd.materialID  = materialID;
        m_cmds.push_back(cmd);
    }

    uint32_t UIDrawList::ReserveQuad()
    {
        const uint32_t base = static_cast<uint32_t>(m_verts.size());
        m_verts.resize(base + 4);
        const size_t i0 = m_indices.size();
        m_indices.resize(i0 + 6);
        const uint16_t b = static_cast<uint16_t>(base);
        m_indices[i0 + 0] = b;
        m_indices[i0 + 1] = static_cast<uint16_t>(b + 1);
        m_indices[i0 + 2] = static_cast<uint16_t>(b + 2);
        m_indices[i0 + 3] = b;
        m_indices[i0 + 4] = static_cast<uint16_t>(b + 2);
        m_indices[i0 + 5] = static_cast<uint16_t>(b + 3);
        m_cmds.back().indexCount += 6;
        return base;
    }

    static UITextureRef CurrentTex(const std::vector<UITextureRef>& s)
    {
        return s.empty() ? UITextureRef{} : s.back();
    }

    // ---- filled primitives ---------------------------------------------------

    void UIDrawList::AddRectFilled(const Vec2& min, const Vec2& max, Color32 col)
    {
        if (min.x >= max.x || min.y >= max.y) return;
        EnsureCmd(CurrentTex(m_texStack), 0);
        const uint32_t b = ReserveQuad();
        const Vec2 p0 = TransformPoint({ min.x, min.y });
        const Vec2 p1 = TransformPoint({ max.x, min.y });
        const Vec2 p2 = TransformPoint({ max.x, max.y });
        const Vec2 p3 = TransformPoint({ min.x, max.y });
        UIVertex* v = m_verts.data() + b;
        v[0] = { { p0.x, p0.y }, { 0, 0 }, col.rgba };
        v[1] = { { p1.x, p1.y }, { 1, 0 }, col.rgba };
        v[2] = { { p2.x, p2.y }, { 1, 1 }, col.rgba };
        v[3] = { { p3.x, p3.y }, { 0, 1 }, col.rgba };
    }

    void UIDrawList::AddTriangleFilled(const Vec2& a, const Vec2& b, const Vec2& c, Color32 col)
    {
        EnsureCmd(CurrentTex(m_texStack), 0);
        const uint32_t base = static_cast<uint32_t>(m_verts.size());
        const Vec2 ta = TransformPoint(a);
        const Vec2 tb = TransformPoint(b);
        const Vec2 tc = TransformPoint(c);
        m_verts.push_back({ { ta.x, ta.y }, { 0, 0 }, col.rgba });
        m_verts.push_back({ { tb.x, tb.y }, { 0, 0 }, col.rgba });
        m_verts.push_back({ { tc.x, tc.y }, { 0, 0 }, col.rgba });
        const uint16_t bv = static_cast<uint16_t>(base);
        m_indices.push_back(bv);
        m_indices.push_back(static_cast<uint16_t>(bv + 1));
        m_indices.push_back(static_cast<uint16_t>(bv + 2));
        m_cmds.back().indexCount += 3;
    }

    void UIDrawList::AddCircleFilled(const Vec2& center, float radius,
                                     Color32 col, int segments)
    {
        if (radius <= 0.f || segments < 3) return;
        EnsureCmd(CurrentTex(m_texStack), 0);
        const uint32_t base = static_cast<uint32_t>(m_verts.size());
        const Vec2 tc = TransformPoint(center);
        m_verts.push_back({ { tc.x, tc.y }, { 0, 0 }, col.rgba });
        const float step = 6.28318530718f / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i)
        {
            const float a = step * static_cast<float>(i);
            const Vec2 p = TransformPoint({ center.x + std::cos(a) * radius,
                                             center.y + std::sin(a) * radius });
            m_verts.push_back({ { p.x, p.y }, { 0, 0 }, col.rgba });
        }
        for (int i = 0; i < segments; ++i)
        {
            const uint16_t i0 = static_cast<uint16_t>(base);
            const uint16_t i1 = static_cast<uint16_t>(base + 1 + i);
            const uint16_t i2 = static_cast<uint16_t>(base + 1 + ((i + 1) % segments));
            m_indices.push_back(i0);
            m_indices.push_back(i1);
            m_indices.push_back(i2);
        }
        m_cmds.back().indexCount += static_cast<uint32_t>(segments * 3);
    }

    void UIDrawList::AddRectFilledRounded(const Vec2& min, const Vec2& max,
                                          Color32 col, float radius,
                                          int segmentsPerCorner)
    {
        if (radius <= 0.f) { AddRectFilled(min, max, col); return; }
        const float maxR = std::min((max.x - min.x) * 0.5f,
                                    (max.y - min.y) * 0.5f);
        const float r = std::min(radius, maxR);
        if (segmentsPerCorner < 1) segmentsPerCorner = 1;

        EnsureCmd(CurrentTex(m_texStack), 0);
        const uint32_t base = static_cast<uint32_t>(m_verts.size());

        // Centre + rim (4 corners × N segments). Corner order: TL, TR, BR, BL.
        const Vec2 centers[4] = {
            { min.x + r, min.y + r },
            { max.x - r, min.y + r },
            { max.x - r, max.y - r },
            { min.x + r, max.y - r },
        };
        const float startAng[4] = { 3.14159265f, 4.71238898f, 0.f, 1.57079633f };

        const Vec2 rectCentre{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };
        const Vec2 trc = TransformPoint(rectCentre);
        m_verts.push_back({ { trc.x, trc.y }, { 0, 0 }, col.rgba });

        const float step = 1.57079633f / static_cast<float>(segmentsPerCorner);
        for (int c = 0; c < 4; ++c)
        {
            for (int s = 0; s <= segmentsPerCorner; ++s)
            {
                const float a = startAng[c] + step * static_cast<float>(s);
                const Vec2 p = TransformPoint({ centers[c].x + std::cos(a) * r,
                                                 centers[c].y + std::sin(a) * r });
                m_verts.push_back({ { p.x, p.y }, { 0, 0 }, col.rgba });
            }
        }

        const int rimCount = static_cast<int>(m_verts.size() - base - 1);
        for (int i = 0; i < rimCount; ++i)
        {
            const uint16_t i0 = static_cast<uint16_t>(base);
            const uint16_t i1 = static_cast<uint16_t>(base + 1 + i);
            const uint16_t i2 = static_cast<uint16_t>(base + 1 + ((i + 1) % rimCount));
            m_indices.push_back(i0);
            m_indices.push_back(i1);
            m_indices.push_back(i2);
        }
        m_cmds.back().indexCount += static_cast<uint32_t>(rimCount * 3);
    }

    // ---- outline primitives --------------------------------------------------

    void UIDrawList::AddLine(const Vec2& a, const Vec2& b, Color32 col, float thickness)
    {
        if (thickness <= 0.f) return;
        // Build a thin quad along the line.
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-5f) return;
        const float nx = -dy / len * (thickness * 0.5f);
        const float ny =  dx / len * (thickness * 0.5f);

        EnsureCmd(CurrentTex(m_texStack), 0);
        const uint32_t base = ReserveQuad();
        const Vec2 p0 = TransformPoint({ a.x + nx, a.y + ny });
        const Vec2 p1 = TransformPoint({ b.x + nx, b.y + ny });
        const Vec2 p2 = TransformPoint({ b.x - nx, b.y - ny });
        const Vec2 p3 = TransformPoint({ a.x - nx, a.y - ny });
        UIVertex* v = m_verts.data() + base;
        v[0] = { { p0.x, p0.y }, { 0, 0 }, col.rgba };
        v[1] = { { p1.x, p1.y }, { 0, 0 }, col.rgba };
        v[2] = { { p2.x, p2.y }, { 0, 0 }, col.rgba };
        v[3] = { { p3.x, p3.y }, { 0, 0 }, col.rgba };
    }

    void UIDrawList::AddRect(const Vec2& min, const Vec2& max, Color32 col, float thickness)
    {
        AddLine({ min.x, min.y }, { max.x, min.y }, col, thickness);
        AddLine({ max.x, min.y }, { max.x, max.y }, col, thickness);
        AddLine({ max.x, max.y }, { min.x, max.y }, col, thickness);
        AddLine({ min.x, max.y }, { min.x, min.y }, col, thickness);
    }

    // ---- textured ------------------------------------------------------------

    void UIDrawList::AddImage(const UITextureRef& tex,
                              const Vec2& min, const Vec2& max,
                              const Vec2& uv0, const Vec2& uv1, Color32 tint)
    {
        if (min.x >= max.x || min.y >= max.y) return;
        EnsureCmd(tex, 0);
        const uint32_t b = ReserveQuad();
        const Vec2 p0 = TransformPoint({ min.x, min.y });
        const Vec2 p1 = TransformPoint({ max.x, min.y });
        const Vec2 p2 = TransformPoint({ max.x, max.y });
        const Vec2 p3 = TransformPoint({ min.x, max.y });
        UIVertex* v = m_verts.data() + b;
        v[0] = { { p0.x, p0.y }, { uv0.x, uv0.y }, tint.rgba };
        v[1] = { { p1.x, p1.y }, { uv1.x, uv0.y }, tint.rgba };
        v[2] = { { p2.x, p2.y }, { uv1.x, uv1.y }, tint.rgba };
        v[3] = { { p3.x, p3.y }, { uv0.x, uv1.y }, tint.rgba };
    }

    // ---- text ----------------------------------------------------------------

    Vec2 UIDrawList::AddText(const Vec2& pos, Color32 col, const char* text)
    {
        if (!text || !*text) return pos;
        if (g_fontProvider) return g_fontProvider->RenderText(*this, pos, col, text);
        return pos; // no font installed → silently no-op
    }

} // namespace UI
