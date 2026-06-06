#include "UI/Font.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// CPU signed-distance-field generation (8SSEDT). File-local — helper structs
// live in an anonymous namespace per the project's ODR rule (same-name structs
// across TUs are silent ICF merges).
// ---------------------------------------------------------------------------
namespace
{
    struct EdtPoint { int dx, dy; };
    inline int EdtDist2(const EdtPoint& p) { return p.dx * p.dx + p.dy * p.dy; }

    // 8-points Signed Sequential Euclidean Distance Transform (Danielsson).
    // Propagates, per cell, the offset vector to the nearest seeded cell.
    struct EdtGrid
    {
        int w = 0, h = 0;
        std::vector<EdtPoint> g;

        EdtPoint Get(int x, int y) const
        {
            if (x >= 0 && y >= 0 && x < w && y < h) return g[static_cast<size_t>(y) * w + x];
            return { 9999, 9999 };
        }
        void Put(int x, int y, const EdtPoint& p) { g[static_cast<size_t>(y) * w + x] = p; }
        void Compare(EdtPoint& p, int x, int y, int ox, int oy) const
        {
            EdtPoint o = Get(x + ox, y + oy);
            o.dx += ox; o.dy += oy;
            if (EdtDist2(o) < EdtDist2(p)) p = o;
        }
        void Generate()
        {
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,-1,0); Compare(p,x,y,0,-1); Compare(p,x,y,-1,-1); Compare(p,x,y,1,-1); Put(x,y,p); }
                for (int x = w - 1; x >= 0; --x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,1,0); Put(x,y,p); }
            }
            for (int y = h - 1; y >= 0; --y)
            {
                for (int x = w - 1; x >= 0; --x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,1,0); Compare(p,x,y,0,1); Compare(p,x,y,-1,1); Compare(p,x,y,1,1); Put(x,y,p); }
                for (int x = 0; x < w; ++x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,-1,0); Put(x,y,p); }
            }
        }
    };

    // Build a padded (cellW x cellH) single-channel SDF (alpha bytes, edge=0.5)
    // from an FT_PIXEL_MODE_GRAY coverage bitmap. The glyph sits `spread` px in
    // from every cell edge. `pitch` is FreeType's signed row stride (add pitch
    // to step down one logical row — valid for both up- and down-flow bitmaps).
    void ComputeGlyphSDF(const uint8_t* cover, int gw, int gh, int pitch,
                         int spread, int cellW, int cellH,
                         std::vector<uint8_t>& outAlpha)
    {
        const int N = cellW * cellH;
        EdtGrid gIn, gOut;                  // gIn seeds INSIDE cells, gOut OUTSIDE
        gIn.w = gOut.w = cellW; gIn.h = gOut.h = cellH;
        const EdtPoint kZero{ 0, 0 }, kInf{ 9999, 9999 };
        gIn.g.assign(N, kInf);
        gOut.g.assign(N, kInf);

        for (int y = 0; y < cellH; ++y)
            for (int x = 0; x < cellW; ++x)
            {
                const int gx = x - spread, gy = y - spread;
                bool inside = false;
                if (gx >= 0 && gy >= 0 && gx < gw && gy < gh)
                    inside = cover[static_cast<ptrdiff_t>(gy) * pitch + gx] >= 128;
                const size_t i = static_cast<size_t>(y) * cellW + x;
                if (inside) gIn.g[i]  = kZero;  // distance-to-inside seed
                else        gOut.g[i] = kZero;  // distance-to-outside seed
            }

        gIn.Generate();
        gOut.Generate();

        outAlpha.assign(static_cast<size_t>(N), 0);
        const float range = 2.0f * static_cast<float>(spread);
        for (int i = 0; i < N; ++i)
        {
            const float dToInside  = std::sqrt(static_cast<float>(EdtDist2(gIn.g[i])));
            const float dToOutside = std::sqrt(static_cast<float>(EdtDist2(gOut.g[i])));
            // Signed distance, positive INSIDE the glyph; edge maps to alpha 0.5.
            const float signedDist = (EdtDist2(gIn.g[i]) == 0) ? +dToOutside : -dToInside;
            float a = 0.5f + signedDist / range;
            a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
            outAlpha[i] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
} // anonymous namespace

namespace UI
{
    // Default range — printable ASCII. Callers wanting CJK pass a range list.
    const CodepointRange Font::kDefaultRanges[] = {
        { 0x0020u, 0x007Eu },
    };
    const int Font::kDefaultRangeCount = 1;

    // ---- Singleton -----------------------------------------------------------
    Font& DefaultFont()
    {
        static Font s_font;
        return s_font;
    }

    void Font::InstallAsGlobal() { UIDrawList::SetGlobalFontProvider(this); }

    void Font::Reset()
    {
        m_glyphs.clear();
        if (m_atlas.IsValid() && m_gfx) m_gfx->DestroyTexture(m_atlas);
        m_atlasSrv = 0;
        m_atlasW = m_atlasH = 0;
    }

    // ---- Init ---------------------------------------------------------------
    bool Font::Init(IGraphicsDevice& gfx,
                    const std::string& ttfPath,
                    float pixelSize,
                    const CodepointRange* extraRanges,
                    int extraRangeCount)
    {
        m_gfx = &gfx;
        // Clear glyphs only — KEEP the atlas texture + descriptor so BakeAtlas
        // can re-upload into it in place. That keeps the atlas SRV slot stable
        // across re-bakes, so it is never freed/recycled and can never be
        // aliased by a UI image texture (the "image samples the font atlas"
        // bug). The public Reset() (shutdown) still frees it.
        m_glyphs.clear();
        m_metrics.pixelSize = pixelSize;
        m_ttfPath = ttfPath;

        std::vector<CodepointRange> ranges;
        ranges.reserve(static_cast<size_t>(kDefaultRangeCount + extraRangeCount));
        for (int i = 0; i < kDefaultRangeCount; ++i) ranges.push_back(kDefaultRanges[i]);
        for (int i = 0; i < extraRangeCount;   ++i) ranges.push_back(extraRanges[i]);

        if (!BakeAtlas(ttfPath, ranges))
        {
            LOG_ERROR("Font::Init: bake failed for %s", ttfPath.c_str());
            return false;
        }
        LOG_SUCCESS("Font: baked %s @ %.1fpx (atlas %ux%u, %zu glyphs, SRV=%llu)",
                    ttfPath.c_str(), pixelSize, m_atlasW, m_atlasH,
                    m_glyphs.size(),
                    static_cast<unsigned long long>(m_atlasSrv));
        return true;
    }

    // ---- BakeAtlas ----------------------------------------------------------
    // Approach: simple shelf packer. Walk the codepoint list, for each glyph
    // load+render via FreeType, place left-to-right in the current row; when
    // the row fills, advance Y by the row's max height. Atlas is fixed size
    // (1024x1024) — plenty for ASCII at 24-32px; CJK bake users size up.
    bool Font::BakeAtlas(const std::string& ttfPath,
                         const std::vector<CodepointRange>& ranges)
    {
        // Slurp TTF via AssetFS so packed builds find it inside game.ipak.
        std::vector<uint8_t> ttfBytes;
        if (!Resource::AssetFS::Get().ReadFile(ttfPath, ttfBytes) || ttfBytes.empty())
        {
            // AssetFS::ReadFile already falls back to loose disk; if it failed,
            // the file genuinely isn't there.
            LOG_ERROR("Font: TTF not found at %s", ttfPath.c_str());
            return false;
        }

        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0)
        {
            LOG_ERROR("Font: FT_Init_FreeType failed");
            return false;
        }

        FT_Face face = nullptr;
        if (FT_New_Memory_Face(library,
                               ttfBytes.data(),
                               static_cast<FT_Long>(ttfBytes.size()),
                               0, &face) != 0)
        {
            LOG_ERROR("Font: FT_New_Memory_Face failed for %s", ttfPath.c_str());
            FT_Done_FreeType(library);
            return false;
        }

        if (FT_Set_Pixel_Sizes(face, 0,
                               static_cast<FT_UInt>(m_metrics.pixelSize)) != 0)
        {
            LOG_ERROR("Font: FT_Set_Pixel_Sizes failed");
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        // Capture global metrics (FT 26.6 fixed → pixels via /64).
        m_metrics.lineHeight = static_cast<float>(face->size->metrics.height) / 64.f;
        m_metrics.ascender   = static_cast<float>(face->size->metrics.ascender) / 64.f;
        m_metrics.descender  = -static_cast<float>(face->size->metrics.descender) / 64.f;
        // Distance field range = full [0,1] alpha span across ±spread glyph px.
        m_metrics.sdfPixelRange = 2.0f * static_cast<float>(kSdfSpread);

        // Atlas: 1024×1024 R8G8B8A8 (rgb=255, a=glyph coverage). Memory: 4 MB.
        // For ASCII at 24px this fills <10% of the atlas — plenty of room
        // for users to add small CJK ranges via extraRanges without resizing.
        constexpr uint32_t W = 1024;
        constexpr uint32_t H = 1024;
        constexpr uint32_t kPad = 1; // padding between glyphs to avoid bleed

        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4u, 0u);
        // Pre-fill RGB with white; alpha stays 0 until per-glyph coverage writes it.
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
        {
            pixels[i * 4 + 0] = 255;
            pixels[i * 4 + 1] = 255;
            pixels[i * 4 + 2] = 255;
        }

        // Each glyph cell is padded by `spread` on every side so the SDF has
        // room for outline/glow. The padding makes the cell-border texels read
        // ~0 (far-outside), so kPad=1 between cells is enough — no bleed.
        const int spread = kSdfSpread;
        std::vector<uint8_t> sdf; // scratch alpha buffer reused per glyph

        uint32_t penX = kPad;
        uint32_t penY = kPad;
        uint32_t rowH = 0;

        for (const auto& r : ranges)
        {
            for (uint32_t cp = r.first; cp <= r.last; ++cp)
            {
                if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;
                FT_GlyphSlot g = face->glyph;
                const uint32_t gw = g->bitmap.width;
                const uint32_t gh = g->bitmap.rows;

                // Whitespace / empty glyphs (e.g. space): no quad, advance only.
                if (gw == 0 || gh == 0 || !g->bitmap.buffer)
                {
                    FontGlyph fge{};
                    fge.advance = static_cast<float>(g->advance.x) / 64.f;
                    m_glyphs.emplace(cp, fge);
                    continue;
                }

                // Padded SDF cell dimensions.
                const uint32_t cellW = gw + 2u * static_cast<uint32_t>(spread);
                const uint32_t cellH = gh + 2u * static_cast<uint32_t>(spread);

                // Wrap to next row if the cell would overflow the right edge.
                if (penX + cellW + kPad > W)
                {
                    penX  = kPad;
                    penY += rowH + kPad;
                    rowH  = 0;
                }
                if (penY + cellH + kPad > H)
                {
                    LOG_WARNING("Font: atlas overflow at codepoint U+%04X — skipping rest", cp);
                    cp = r.last; // bail this range; outer loop continues to next
                    break;
                }

                // Generate the signed distance field for this glyph into `sdf`.
                ComputeGlyphSDF(g->bitmap.buffer,
                                static_cast<int>(gw), static_cast<int>(gh),
                                g->bitmap.pitch, spread,
                                static_cast<int>(cellW), static_cast<int>(cellH),
                                sdf);

                // Copy the SDF into the atlas ALPHA channel (rgb already 255).
                for (uint32_t y = 0; y < cellH; ++y)
                {
                    const uint8_t* src = sdf.data() + static_cast<size_t>(y) * cellW;
                    uint8_t*       dst = pixels.data()
                                        + ((static_cast<size_t>(penY) + y) * W + penX) * 4u
                                        + 3; // alpha channel
                    for (uint32_t x = 0; x < cellW; ++x, ++src, dst += 4)
                        *dst = *src;
                }

                // Store the ENLARGED quad: the cell includes `spread` padding on
                // every side, so bearing shifts (-spread, +spread) and size grows
                // by 2*spread. Advance is unchanged (the true glyph metric).
                FontGlyph fg{};
                fg.uv0     = { static_cast<float>(penX)         / static_cast<float>(W),
                               static_cast<float>(penY)         / static_cast<float>(H) };
                fg.uv1     = { static_cast<float>(penX + cellW) / static_cast<float>(W),
                               static_cast<float>(penY + cellH) / static_cast<float>(H) };
                fg.size    = { static_cast<float>(cellW), static_cast<float>(cellH) };
                fg.bearing = { static_cast<float>(g->bitmap_left) - static_cast<float>(spread),
                               static_cast<float>(g->bitmap_top)  + static_cast<float>(spread) };
                fg.advance = static_cast<float>(g->advance.x) / 64.f;
                m_glyphs.emplace(cp, fg);

                penX += cellW + kPad;
                if (cellH > rowH) rowH = cellH;
            }
        }

        FT_Done_Face(face);
        FT_Done_FreeType(library);

        // Upload atlas as a static R8G8B8A8 SRV-only texture.
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.width      = W;
        td.height     = H;
        td.depth      = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;

        RHI::SubresourceData sub{};
        sub.data_ptr    = pixels.data();
        sub.row_pitch   = W * 4u;
        sub.slice_pitch = W * H * 4u;

        // Re-upload into the EXISTING atlas (fixed 1024x1024) when present, so
        // the SRV descriptor slot stays stable across re-bakes — it is never
        // freed, hence never recycled, hence can never be aliased by a UI image
        // texture. Only the very first bake allocates a slot; fall back to a
        // fresh create if UpdateTexture isn't supported.
        bool reused = false;
        if (m_atlas.IsValid() && m_atlasW == W && m_atlasH == H)
            reused = m_gfx->UpdateTexture(m_atlas, &sub, 1);

        if (!reused)
        {
            if (m_atlas.IsValid()) m_gfx->DestroyTexture(m_atlas);
            if (!m_gfx->CreateTexture(td, m_atlas, &sub))
            {
                LOG_ERROR("Font: CreateTexture failed for atlas");
                return false;
            }
            m_atlasSrv = m_gfx->GetTextureSRVGpuHandle(m_atlas);
            m_atlasW   = W;
            m_atlasH   = H;
        }
        return m_atlasSrv != 0;
    }

    // ---- UTF-8 decode -------------------------------------------------------
    // Returns codepoint and advances p past the consumed bytes. On a malformed
    // sequence, returns U+FFFD and advances by 1.
    static uint32_t DecodeUtf8(const char*& p)
    {
        const auto b0 = static_cast<uint8_t>(*p);
        if (b0 < 0x80u) { ++p; return b0; }
        auto take = [&](int extra) -> uint32_t {
            uint32_t cp = b0 & static_cast<uint8_t>(0xFFu >> (extra + 1));
            for (int i = 0; i < extra; ++i)
            {
                ++p;
                if ((static_cast<uint8_t>(*p) & 0xC0u) != 0x80u) { return 0xFFFDu; }
                cp = (cp << 6) | (static_cast<uint8_t>(*p) & 0x3Fu);
            }
            ++p;
            return cp;
        };
        if ((b0 & 0xE0u) == 0xC0u) return take(1);
        if ((b0 & 0xF0u) == 0xE0u) return take(2);
        if ((b0 & 0xF8u) == 0xF0u) return take(3);
        ++p;
        return 0xFFFDu;
    }

    // ---- Color helpers ------------------------------------------------------
    static void Color32ToFloat4(Color32 c, float out[4])
    {
        out[0] = ((c.rgba >> 0)  & 0xFFu) / 255.f;
        out[1] = ((c.rgba >> 8)  & 0xFFu) / 255.f;
        out[2] = ((c.rgba >> 16) & 0xFFu) / 255.f;
        out[3] = ((c.rgba >> 24) & 0xFFu) / 255.f;
    }
    // Stable per-glyph [0,1) hash → jitter phase.
    static float GlyphHash01(int glyphIndex)
    {
        uint32_t h = static_cast<uint32_t>(glyphIndex) * 374761393u + 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
    }

    // ---- RenderText (plain SDF, no effects) ---------------------------------
    Vec2 Font::RenderText(UIDrawList& out, const Vec2& pos,
                          Color32 col, const char* text)
    {
        return RenderTextStyled(out, pos, col, text, TextEffect{}, 0.f);
    }

    // ---- RenderTextStyled ---------------------------------------------------
    Vec2 Font::RenderTextStyled(UIDrawList& out, const Vec2& pos, Color32 col,
                                const char* text, const TextEffect& fx, float timeSec)
    {
        if (!text || !*text || !m_atlasSrv) return pos;
        const float scale = m_metrics.pixelScale;

        UITextureRef texRef{};
        texRef.srvGpuHandle = m_atlasSrv; // read fresh — atlas SRV can be recycled

        // Resolve outline/glow into a per-frame effect-table entry (shader-side).
        // Shadow + jitter are CPU-side and need no table slot.
        uint32_t effIdx = 0;
        if (fx.outline || fx.glow)
        {
            const float range = m_metrics.sdfPixelRange > 0.f ? m_metrics.sdfPixelRange : 8.f;
            GpuTextEffect ge{};
            if (fx.outline)
            {
                Color32ToFloat4(fx.outlineColor, ge.outlineColor);
                ge.outlineWidthN = std::min(0.49f, std::max(0.f, fx.outlineWidthPx / range));
            }
            else { ge.outlineColor[3] = 0.f; }
            if (fx.glow)
            {
                Color32ToFloat4(fx.glowColor, ge.glowColor);
                ge.glowWidthN = std::min(0.5f, std::max(0.f, fx.glowWidthPx / range));
            }
            else { ge.glowColor[3] = 0.f; }
            effIdx = out.AddTextEffect(ge);
        }

        // One styled pass: walk the string emitting SDF glyph quads at a pixel
        // offset (ox,oy), tinted `c`, with effect index `eff`. Jitter (if on)
        // wobbles each glyph quad about its pen position; the pen itself never
        // jitters so spacing stays stable and shadow tracks the main glyph.
        Vec2 endPen{ pos.x, pos.y };
        auto emitPass = [&](float ox, float oy, Color32 c, uint32_t eff)
        {
            float penX      = pos.x + ox;
            float baselineY  = pos.y + m_metrics.ascender * scale + oy;
            int   glyphIndex = 0;
            const char* p = text;
            while (*p)
            {
                const uint32_t cp = DecodeUtf8(p);
                if (cp == '\n')
                {
                    penX      = pos.x + ox;
                    baselineY += m_metrics.lineHeight * scale;
                    continue;
                }
                auto it = m_glyphs.find(cp);
                if (it != m_glyphs.end())
                {
                    const FontGlyph& g = it->second;
                    if (g.size.x > 0.f && g.size.y > 0.f)
                    {
                        float jx = 0.f, jy = 0.f;
                        if (fx.jitter)
                        {
                            const float ph = GlyphHash01(glyphIndex) * 6.2831853f;
                            const float amp = fx.jitterAmpPx * scale;
                            jx = amp * std::sin(timeSec * fx.jitterFreq + ph);
                            jy = amp * std::cos(timeSec * fx.jitterFreq * 1.13f + ph * 1.7f);
                        }
                        const Vec2 mn{ penX + g.bearing.x * scale + jx,
                                       baselineY - g.bearing.y * scale + jy };
                        const Vec2 mx{ mn.x + g.size.x * scale,
                                       mn.y + g.size.y * scale };
                        out.AddImageMat(texRef, mn, mx, g.uv0, g.uv1, c, 1u, eff);
                    }
                    penX += g.advance * scale;
                }
                ++glyphIndex;
            }
            endPen = Vec2{ penX, baselineY };
        };

        // Drop shadow first (behind), then the main text on top.
        if (fx.shadow)
            emitPass(fx.shadowOffsetPx.x * scale, fx.shadowOffsetPx.y * scale,
                     fx.shadowColor, 0u);
        emitPass(0.f, 0.f, col, effIdx);

        return endPen;
    }

    // ---- MeasureText --------------------------------------------------------
    Vec2 Font::MeasureText(const char* text) const
    {
        if (!text || !*text) return { 0, 0 };
        const float scale = m_metrics.pixelScale;
        float lineW   = 0.f;
        float maxW    = 0.f;
        int   lines   = 1;
        const char* p = text;
        while (*p)
        {
            const uint32_t cp = DecodeUtf8(p);
            if (cp == '\n')
            {
                if (lineW > maxW) maxW = lineW;
                lineW = 0.f;
                ++lines;
                continue;
            }
            auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) continue;
            lineW += it->second.advance * scale;
        }
        if (lineW > maxW) maxW = lineW;
        return Vec2{ maxW,
                     static_cast<float>(lines) * m_metrics.lineHeight * scale };
    }

} // namespace UI
