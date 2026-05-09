#pragma once

// UIDrawList — CPU-side draw list (Dear ImGui style, but retained-mode).
//
// Widgets emit primitives into a UIDrawList during paint. The UIPass uploads
// the vertex/index/command buffers once per frame and translates each command
// into a DX12 draw call.
//
// Layout (matches design doc §5.1):
//   Vertex: 20 bytes (pos.xy, uv.xy, RGBA8 color packed into uint32).
//   Indices: uint16 (≤ 65535 vertices per frame).
//   Commands: { indexOffset, indexCount, clipRect, textureSrv, materialID }.

#include <cstdint>
#include <vector>

namespace UI
{
    // Packed 0xAABBGGRR (matches Dear ImGui IM_COL32 byte order so debugging
    // tools that already understand IM_COL32 can be reused unchanged).
    struct Color32
    {
        uint32_t rgba = 0xFFFFFFFFu;

        constexpr Color32() = default;
        constexpr Color32(uint32_t v) : rgba(v) {}
        constexpr Color32(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
            : rgba(uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24)) {}

        static constexpr Color32 White() { return Color32(255, 255, 255, 255); }
        static constexpr Color32 Black() { return Color32(0,   0,   0,   255); }
        static constexpr Color32 Transparent() { return Color32(0, 0, 0, 0); }
    };

    struct Vec2
    {
        float x = 0.f, y = 0.f;
        constexpr Vec2() = default;
        constexpr Vec2(float xx, float yy) : x(xx), y(yy) {}
    };

    // Field names use `mn` / `mx` (not `min` / `max`) so the type works even
    // in TUs that have already pulled in <Windows.h>'s function-like
    // min/max macros — NOMINMAX isn't enforced project-wide.
    struct Rect
    {
        Vec2 mn{}, mx{};
        constexpr Rect() = default;
        constexpr Rect(const Vec2& a, const Vec2& b) : mn(a), mx(b) {}
        float Width()  const { return mx.x - mn.x; }
        float Height() const { return mx.y - mn.y; }
        bool  Contains(const Vec2& p) const
        { return p.x >= mn.x && p.y >= mn.y && p.x < mx.x && p.y < mx.y; }
        bool  Intersects(const Rect& o) const
        { return !(o.mx.x <= mn.x || o.mn.x >= mx.x ||
                   o.mx.y <= mn.y || o.mn.y >= mx.y); }
    };

    struct UIVertex
    {
        float    pos[2];   // screen-space pixels (UIPass shader applies ortho)
        float    uv[2];    // 0..1 if texture, otherwise unused
        uint32_t col;      // RGBA8 packed (see Color32::rgba layout)
    };
    static_assert(sizeof(UIVertex) == 20, "UIVertex must be 20 bytes");

    // Texture binding identity for a draw command. SrvGpuHandle == 0 means
    // "use the engine's default 1×1 white texture" so AddRectFilled doesn't
    // require an atlas.
    struct UITextureRef
    {
        uint64_t srvGpuHandle = 0;
    };

    struct UIDrawCmd
    {
        uint32_t     indexOffset = 0;
        uint32_t     indexCount  = 0;
        Rect         clipRect;       // axis-aligned scissor (pixels)
        UITextureRef texture;
        uint32_t     materialID = 0; // 0=normal, 1=SDF text (future), 2=mask
    };

    class UIDrawList
    {
    public:
        // ---- lifecycle ----
        void Clear();
        bool IsEmpty() const { return m_indices.empty(); }

        // ---- clipping ----
        // Push an axis-aligned scissor rect. New commands inherit the current
        // clip until the matching Pop. Stacked scissors intersect (top wins).
        void PushClipRect(const Rect& r);
        void PopClipRect();
        Rect CurrentClipRect() const;

        // ---- texture ----
        // Bind a texture for subsequent primitives. Pass {} to revert to the
        // default white texture (solid-colour fills).
        void PushTexture(const UITextureRef& tex);
        void PopTexture();

        // ---- transform (pivot-anchored uniform scale) ----
        // Pushes a (pivot, scale) onto a stack. Every subsequent primitive
        // vertex P is transformed as `(P - pivot) * scale + pivot` before
        // being written. Used by world-space UI to grow/shrink whole UI
        // roots about their projected screen-space pivot. Multiple stacked
        // transforms compose innermost-first (most-recent push applied
        // before older ones — matches OpenGL pushMatrix semantics).
        // Note: hit-testing is NOT transformed; widgets that need
        // click-accurate scaling should clamp the visual scale to 1 or
        // disable hit-test.
        void PushTransform(const Vec2& pivot, const Vec2& scale);
        void PopTransform();

        // ---- primitives (filled) ----
        void AddRectFilled(const Vec2& min, const Vec2& max, Color32 col);
        // Rounded rect: r=0 is a sharp rect, r>0 carves the four corners.
        // segmentsPerCorner caps tessellation; design defaults are fine.
        void AddRectFilledRounded(const Vec2& min, const Vec2& max,
                                  Color32 col, float radius,
                                  int segmentsPerCorner = 6);
        void AddTriangleFilled(const Vec2& a, const Vec2& b, const Vec2& c, Color32 col);
        void AddCircleFilled(const Vec2& center, float radius,
                             Color32 col, int segments = 24);

        // ---- primitives (outline) ----
        void AddRect(const Vec2& min, const Vec2& max, Color32 col, float thickness = 1.f);
        void AddLine(const Vec2& a, const Vec2& b, Color32 col, float thickness = 1.f);

        // ---- textured ----
        // Sample @p tex over the rect. uv0/uv1 are texture-space coordinates
        // [0,1]. Tint is multiplied with the sampled colour.
        void AddImage(const UITextureRef& tex,
                      const Vec2& min, const Vec2& max,
                      const Vec2& uv0 = Vec2(0, 0), const Vec2& uv1 = Vec2(1, 1),
                      Color32 tint = Color32::White());

        // ---- text (Phase 2 will plug in the bitmap/MSDF font) ----
        // Stub interface — no-op until the font system lands so widget code
        // can already call it. Returns the cursor advance the caller should use.
        Vec2 AddText(const Vec2& pos, Color32 col, const char* text);

        // ---- raw access (consumed by UIPass) ----
        const std::vector<UIVertex>&  Vertices()  const { return m_verts; }
        const std::vector<uint16_t>&  Indices()   const { return m_indices; }
        const std::vector<UIDrawCmd>& Commands()  const { return m_cmds; }

        // ---- font binding (set once by App; AddText reads it) ----
        struct IFontProvider
        {
            virtual ~IFontProvider() = default;
            // Render @p text into @p out at @p pos with @p col. Returns the
            // pen advance used (so callers can chain text segments).
            virtual Vec2 RenderText(UIDrawList& out, const Vec2& pos,
                                    Color32 col, const char* text) = 0;
        };
        static void SetGlobalFontProvider(IFontProvider* fp);

    private:
        std::vector<UIVertex>  m_verts;
        std::vector<uint16_t>  m_indices;
        std::vector<UIDrawCmd> m_cmds;

        std::vector<Rect>         m_clipStack;
        std::vector<UITextureRef> m_texStack;
        struct XForm { Vec2 pivot; Vec2 scale; };
        std::vector<XForm>        m_xformStack;
        Vec2 TransformPoint(const Vec2& p) const;

        // Scratch — last-cmd merging when texture/clip/material match.
        void EnsureCmd(const UITextureRef& tex, uint32_t materialID);

        uint32_t ReserveQuad(); // returns first vertex index (4 verts + 6 indices)
    };

} // namespace UI
