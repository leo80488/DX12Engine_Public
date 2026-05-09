#pragma once

// Font — FreeType-backed text rendering for the UI module.
//
// Init() rasterises a configurable codepoint range (default: ASCII 32..126)
// from a TTF file into a single R8G8B8A8 atlas (rgb=255, a=glyph coverage),
// uploads the atlas via IGraphicsDevice, and stores per-glyph metrics.
//
// At RenderText time, each glyph emits one UIDrawList::AddImage call against
// the atlas SRV; identical (atlas, clip) draws merge into a single batch by
// UIDrawList's command-aggregation logic.
//
// Codepoints outside the baked range emit no quad (skipped silently).
// CJK / extended ranges can be added via Init's @p extraRanges parameter
// or by extending kDefaultRanges below.

#include "UI/UIDrawList.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;

namespace UI
{
    struct FontGlyph
    {
        Vec2     uv0{};   // top-left in [0,1] of atlas
        Vec2     uv1{};   // bottom-right in [0,1]
        Vec2     size{};  // pixel size of the glyph quad
        Vec2     bearing{}; // pixel offset from pen-on-baseline to glyph top-left
        float    advance = 0.f; // pixel advance to next pen position
    };

    struct FontMetrics
    {
        float pixelSize  = 24.f;  // baked size in pixels
        float lineHeight = 28.f;  // line advance in pixels
        float ascender   = 18.f;  // baseline → top of glyph (positive)
        float descender  = 6.f;   // baseline → bottom of glyph (positive)
        float pixelScale = 1.f;   // user-side scale on top of baked size
    };

    // Codepoint range [first..last] inclusive. 32..126 is printable ASCII.
    struct CodepointRange { uint32_t first; uint32_t last; };

    class Font : public UIDrawList::IFontProvider
    {
    public:
        // Default ranges baked when none are supplied: printable ASCII only.
        static const CodepointRange kDefaultRanges[];
        static const int            kDefaultRangeCount;

        // Bake @p ttfPath at @p pixelSize into a fresh GPU atlas.
        // Returns false if the TTF cannot be opened or atlas allocation fails;
        // any prior atlas is released. Safe to call multiple times.
        bool Init(IGraphicsDevice& gfx,
                  const std::string& ttfPath,
                  float              pixelSize    = 24.f,
                  const CodepointRange* extraRanges = nullptr,
                  int                   extraRangeCount = 0);

        // Free the GPU atlas (pre-Shutdown of the device); the next Init call
        // re-bakes from scratch.
        void Reset();

        // ---- IFontProvider --------------------------------------------------
        Vec2 RenderText(UIDrawList& out, const Vec2& pos,
                        Color32 col, const char* utf8Text) override;

        // Convenience: pixel-space size of @p utf8Text at the current scale.
        Vec2 MeasureText(const char* utf8Text) const;

        // Install as the global UIDrawList font provider so AddText / TextWidget
        // pick it up automatically.
        void InstallAsGlobal();

        // ---- Accessors ------------------------------------------------------
        const FontMetrics& Metrics() const { return m_metrics; }
        FontMetrics&       Metrics()       { return m_metrics; }
        uint64_t           AtlasSrvHandle() const { return m_atlasSrv; }
        // Bindless index into the engine's t0 space2 texture table.
        // Returns kInvalidBindlessIndex (~0u) when the atlas is not ready.
        uint32_t           AtlasBindlessIndex() const {
            return m_atlas.IsValid() ? m_atlas.handle_id : ~0u;
        }
        bool               IsReady() const { return m_atlasSrv != 0; }

        // Editor-introspection — current bake parameters + glyph table.
        const std::string& TTFPath()  const { return m_ttfPath; }
        uint32_t           AtlasWidth()  const { return m_atlasW; }
        uint32_t           AtlasHeight() const { return m_atlasH; }
        const std::unordered_map<uint32_t, FontGlyph>& Glyphs() const { return m_glyphs; }

    private:
        IGraphicsDevice* m_gfx = nullptr;

        std::string                                  m_ttfPath;
        FontMetrics                                  m_metrics;
        std::unordered_map<uint32_t, FontGlyph>      m_glyphs;
        RHI::Texture                                 m_atlas;
        uint64_t                                     m_atlasSrv = 0;
        uint32_t                                     m_atlasW = 0;
        uint32_t                                     m_atlasH = 0;

        bool BakeAtlas(const std::string& ttfPath,
                       const std::vector<CodepointRange>& ranges);
    };

    // Singleton accessor — owned by the UI module, lazy-initialised.
    Font& DefaultFont();

} // namespace UI
