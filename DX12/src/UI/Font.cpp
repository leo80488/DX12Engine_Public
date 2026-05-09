#include "UI/Font.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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
        Reset();
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

                // Wrap to next row if the glyph would overflow the right edge.
                if (penX + gw + kPad > W)
                {
                    penX  = kPad;
                    penY += rowH + kPad;
                    rowH  = 0;
                }
                if (penY + gh + kPad > H)
                {
                    LOG_WARNING("Font: atlas overflow at codepoint U+%04X — skipping rest", cp);
                    cp = r.last; // bail this range; outer loop continues to next
                    break;
                }

                // Copy glyph alpha into atlas[penX..penX+gw, penY..penY+gh].
                // FT bitmap is 1 byte per pixel (FT_PIXEL_MODE_GRAY).
                if (g->bitmap.buffer)
                {
                    for (uint32_t y = 0; y < gh; ++y)
                    {
                        const uint8_t* src = g->bitmap.buffer + y * g->bitmap.pitch;
                        uint8_t*       dst = pixels.data()
                                            + ((penY + y) * W + penX) * 4u
                                            + 3; // alpha channel
                        for (uint32_t x = 0; x < gw; ++x, ++src, dst += 4)
                            *dst = *src;
                    }
                }

                FontGlyph fg{};
                fg.uv0     = { static_cast<float>(penX)        / static_cast<float>(W),
                               static_cast<float>(penY)        / static_cast<float>(H) };
                fg.uv1     = { static_cast<float>(penX + gw)   / static_cast<float>(W),
                               static_cast<float>(penY + gh)   / static_cast<float>(H) };
                fg.size    = { static_cast<float>(gw), static_cast<float>(gh) };
                fg.bearing = { static_cast<float>(g->bitmap_left),
                               static_cast<float>(g->bitmap_top) };
                fg.advance = static_cast<float>(g->advance.x) / 64.f;
                m_glyphs.emplace(cp, fg);

                penX += gw + kPad;
                if (gh > rowH) rowH = gh;
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

        if (!m_gfx->CreateTexture(td, m_atlas, &sub))
        {
            LOG_ERROR("Font: CreateTexture failed for atlas");
            return false;
        }
        m_atlasSrv = m_gfx->GetTextureSRVGpuHandle(m_atlas);
        m_atlasW   = W;
        m_atlasH   = H;
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

    // ---- RenderText ---------------------------------------------------------
    Vec2 Font::RenderText(UIDrawList& out, const Vec2& pos,
                          Color32 col, const char* text)
    {
        if (!text || !*text || !m_atlasSrv) return pos;
        const float scale = m_metrics.pixelScale;
        // Pen at baseline; atlas y grows downward, FT bitmap_top is positive
        // upward — so the per-glyph y is `baselineY - bearing.y * scale`.
        const float baselineY0 = pos.y + m_metrics.ascender * scale;
        float       penX       = pos.x;
        float       baselineY  = baselineY0;

        UITextureRef texRef{};
        texRef.srvGpuHandle = m_atlasSrv;
        out.PushTexture(texRef);

        const char* p = text;
        while (*p)
        {
            const uint32_t cp = DecodeUtf8(p);
            if (cp == '\n')
            {
                penX       = pos.x;
                baselineY += m_metrics.lineHeight * scale;
                continue;
            }
            auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) continue; // not baked → skip silently

            const FontGlyph& g = it->second;
            if (g.size.x > 0.f && g.size.y > 0.f)
            {
                const Vec2 mn{
                    penX      + g.bearing.x * scale,
                    baselineY - g.bearing.y * scale,
                };
                const Vec2 mx{
                    mn.x + g.size.x * scale,
                    mn.y + g.size.y * scale,
                };
                out.AddImage(texRef, mn, mx, g.uv0, g.uv1, col);
            }
            penX += g.advance * scale;
        }

        out.PopTexture();
        return Vec2{ penX, baselineY };
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
