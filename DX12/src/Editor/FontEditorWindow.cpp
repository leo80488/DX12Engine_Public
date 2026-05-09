// FontEditorWindow.cpp — live editor for the global UI font.
//
// Drives `UI::DefaultFont()`. The user picks a TTF, pixel size, and one or
// more codepoint ranges; pressing "Bake" releases the current GPU atlas
// (deferred destroy is safe — the engine guards FrameCount frames) and
// re-rasterises through FreeType. The atlas SRV is shown as an ImGui::Image
// so packing failures are visible at a glance, and a "Test" preview renders
// a string with the current font right inside the panel via ImDrawList.

#include "Editor/EditorLayer.h"

#include "imgui/imgui.h"
#include "UI/Font.h"
#include "UI/UIDrawList.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    // ---- Range presets (used by the combo + multi-select) -------------------
    struct RangePreset
    {
        const char*               label;
        UI::CodepointRange        range;
    };

    // 0x4E00–0x9FFF is the full CJK Unified Ideographs block (~20K glyphs);
    // baking the whole thing into a 1024×1024 atlas at 24px overflows. The
    // editor shows the predicted glyph count BEFORE bake so the user can
    // pick a smaller subset.
    constexpr RangePreset kPresets[] = {
        // ASCII is always baked by Font::Init; it still appears here so the
        // user can clearly see what's included by default.
        { "ASCII (32-126)",          { 0x0020u, 0x007Eu } },
        { "Latin-1 Supplement",      { 0x00A0u, 0x00FFu } },
        { "Latin Extended-A",        { 0x0100u, 0x017Fu } },
        { "Hiragana",                { 0x3041u, 0x3096u } },
        { "Katakana",                { 0x30A1u, 0x30FBu } },
        { "CJK Punctuation",         { 0x3000u, 0x303Fu } },
        { "Halfwidth/Fullwidth",     { 0xFF00u, 0xFFEFu } },
        { "CJK 4E00-4FFF (subset)",  { 0x4E00u, 0x4FFFu } },
        { "CJK 4E00-5FFF",           { 0x4E00u, 0x5FFFu } },
    };
    constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

    // ---- Persistent UI state — owned by the editor singleton ---------------
    // Lives at TU scope because EditorLayer doesn't dedicate state to this
    // panel (it would bloat the header). One panel instance per process is
    // fine for an editor tool.
    struct FontEditorState
    {
        char        ttfPath[256] = "asset/font/FGMiraiRen.ttf";
        float       pixelSize    = 24.f;
        // Bit-mask over kPresets[] — 1 << i sets the i-th preset.
        uint32_t    presetMask   = 0u; // ASCII auto-included by Font::Init regardless
        // Custom range — added on top of presets when both ends > 0.
        int         customStart  = 0x4E00;
        int         customEnd    = 0x4E7F;
        // Test text preview.
        char        testText[512] = "Hello, world! 0123456789\nThe quick brown fox jumps over the lazy dog.";
        float       testColor[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
        float       testScale     = 1.0f;
        // Atlas zoom (1.0 = pixel-perfect, < 1 = shrink to fit).
        float       atlasZoom    = 0.5f;
        // Last bake outcome — shown beneath the Bake button.
        std::string lastStatus;
        bool        lastBakeOK  = false;
    };

    FontEditorState& State()
    {
        static FontEditorState s;
        return s;
    }
}

void EditorLayer::RenderFontEditorWindow()
{
    if (!m_gfx) return;
    auto& st = State();

    ImGui::SetNextWindowSize(ImVec2(720.f, 640.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI Font Editor", &m_showFontEditor))
    {
        ImGui::End();
        return;
    }

    UI::Font& font = UI::DefaultFont();

    // ---- Bake parameters -----------------------------------------------------
    ImGui::SeparatorText("Bake Parameters");

    ImGui::PushItemWidth(-180.f);
    ImGui::InputText("TTF path##fonted", st.ttfPath, sizeof(st.ttfPath));
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##path"))
        ::strcpy_s(st.ttfPath, "asset/font/FGMiraiRen.ttf");

    ImGui::DragFloat("Pixel size##fonted", &st.pixelSize, 0.5f, 8.f, 96.f, "%.1f px");
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::TextDisabled("Codepoint ranges (ASCII always included)");
    if (ImGui::BeginTable("##fontranges", 2,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Glyphs", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < kPresetCount; ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool checked = (st.presetMask & (1u << i)) != 0u;
            if (ImGui::Checkbox(kPresets[i].label, &checked))
                st.presetMask = checked ? (st.presetMask | (1u << i))
                                        : (st.presetMask & ~(1u << i));
            ImGui::TableNextColumn();
            const uint32_t span = kPresets[i].range.last - kPresets[i].range.first + 1u;
            ImGui::TextDisabled("%u", span);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Custom range (hex codepoints, set both > 0 to enable)");
    ImGui::PushItemWidth(120.f);
    if (ImGui::InputInt("Start##custom", &st.customStart, 1, 16, ImGuiInputTextFlags_CharsHexadecimal))
        st.customStart = std::clamp(st.customStart, 0, 0x10FFFF);
    ImGui::SameLine();
    if (ImGui::InputInt("End##custom", &st.customEnd, 1, 16, ImGuiInputTextFlags_CharsHexadecimal))
        st.customEnd = std::clamp(st.customEnd, 0, 0x10FFFF);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (st.customStart > 0 && st.customEnd >= st.customStart)
        ImGui::TextDisabled("→ U+%04X..U+%04X (%d glyphs)",
                            st.customStart, st.customEnd,
                            st.customEnd - st.customStart + 1);
    else
        ImGui::TextDisabled("→ disabled");

    // ---- Bake button ---------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::Button("Bake / Re-bake", ImVec2(180.f, 32.f)))
    {
        std::vector<UI::CodepointRange> extras;
        extras.reserve(static_cast<size_t>(kPresetCount) + 1u);
        for (int i = 0; i < kPresetCount; ++i)
            if (st.presetMask & (1u << i)) extras.push_back(kPresets[i].range);
        if (st.customStart > 0 && st.customEnd >= st.customStart)
            extras.push_back({ static_cast<uint32_t>(st.customStart),
                                static_cast<uint32_t>(st.customEnd) });

        const bool ok = font.Init(*m_gfx, st.ttfPath, st.pixelSize,
                                   extras.empty() ? nullptr : extras.data(),
                                   static_cast<int>(extras.size()));
        if (ok)
        {
            font.InstallAsGlobal();
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "Baked %zu glyphs into %ux%u atlas",
                          font.Glyphs().size(),
                          font.AtlasWidth(), font.AtlasHeight());
            st.lastStatus = buf;
            st.lastBakeOK = true;
        }
        else
        {
            st.lastStatus = "Bake failed — check log (TTF missing? path typo?)";
            st.lastBakeOK = false;
        }
    }
    ImGui::SameLine();
    if (!st.lastStatus.empty())
    {
        if (st.lastBakeOK) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "%s", st.lastStatus.c_str());
        else               ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.f), "%s", st.lastStatus.c_str());
    }

    // ---- Current bake status -------------------------------------------------
    ImGui::SeparatorText("Current Bake");
    if (!font.IsReady())
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f),
                           "Font has not been baked yet — press Bake.");
    }
    else
    {
        ImGui::Text("TTF:        %s", font.TTFPath().c_str());
        ImGui::Text("Pixel size: %.1f px (line %.1f, asc %.1f, desc %.1f)",
                    font.Metrics().pixelSize,
                    font.Metrics().lineHeight,
                    font.Metrics().ascender,
                    font.Metrics().descender);
        ImGui::Text("Atlas:      %u x %u  (%zu glyphs)",
                    font.AtlasWidth(), font.AtlasHeight(),
                    font.Glyphs().size());
    }

    // ---- Atlas preview -------------------------------------------------------
    if (font.IsReady() && ImGui::CollapsingHeader("Atlas Preview", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Zoom##atlas", &st.atlasZoom, 0.1f, 2.0f, "%.2fx");
        const float w = static_cast<float>(font.AtlasWidth())  * st.atlasZoom;
        const float h = static_cast<float>(font.AtlasHeight()) * st.atlasZoom;

        ImGui::BeginChild("##atlasview", ImVec2(0.f, std::min(h + 20.f, 480.f)), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        // Atlas is RGBA8 (rgb=255, a=glyph coverage). Display with checker so
        // the empty (alpha=0) regions don't blend into the panel background.
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cur, ImVec2(cur.x + w, cur.y + h),
            IM_COL32(40, 40, 50, 255));
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(font.AtlasSrvHandle())),
                     ImVec2(w, h));
        ImGui::EndChild();
    }

    // ---- Test text preview ---------------------------------------------------
    if (font.IsReady() && ImGui::CollapsingHeader("Test Preview", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputTextMultiline("##testtext", st.testText, sizeof(st.testText),
                                   ImVec2(-FLT_MIN, 60.f));
        ImGui::ColorEdit4("Color##test", st.testColor);
        ImGui::SliderFloat("Scale##test", &st.testScale, 0.25f, 4.0f, "%.2fx");

        // Render the test text via ImDrawList::AddImage per glyph using the
        // baked atlas. Mirrors UI::Font::RenderText but writes directly into
        // ImGui's foreground draw list so the result lives inside this panel.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float boxH = std::max(96.f, font.Metrics().lineHeight * st.testScale * 4.f + 16.f);
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, boxH));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Background panel so glyphs are visible against editor chrome.
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + ImGui::GetContentRegionAvail().x, origin.y + boxH),
                          IM_COL32(20, 20, 25, 255));
        dl->AddRect(origin,
                    ImVec2(origin.x + ImGui::GetContentRegionAvail().x, origin.y + boxH),
                    IM_COL32(80, 80, 95, 255));

        const ImU32 col = IM_COL32(
            static_cast<int>(st.testColor[0] * 255.f),
            static_cast<int>(st.testColor[1] * 255.f),
            static_cast<int>(st.testColor[2] * 255.f),
            static_cast<int>(st.testColor[3] * 255.f));

        const ImTextureID atlasTex = static_cast<ImTextureID>(font.AtlasSrvHandle());
        const float pad = 8.f;
        float penX = origin.x + pad;
        float baseY = origin.y + pad + font.Metrics().ascender * st.testScale;

        // UTF-8 walk — same primitive Font.cpp uses internally; inlined here
        // to keep the panel self-contained.
        const char* p = st.testText;
        while (*p)
        {
            uint32_t cp;
            const uint8_t b0 = static_cast<uint8_t>(*p);
            if (b0 < 0x80u) { cp = b0; ++p; }
            else if ((b0 & 0xE0u) == 0xC0u && p[1])
            { cp = ((b0 & 0x1Fu) << 6) | (uint8_t(p[1]) & 0x3Fu); p += 2; }
            else if ((b0 & 0xF0u) == 0xE0u && p[1] && p[2])
            { cp = ((b0 & 0x0Fu) << 12) | ((uint8_t(p[1]) & 0x3Fu) << 6) | (uint8_t(p[2]) & 0x3Fu); p += 3; }
            else { cp = 0xFFFDu; ++p; }

            if (cp == '\n')
            {
                penX  = origin.x + pad;
                baseY += font.Metrics().lineHeight * st.testScale;
                continue;
            }
            auto it = font.Glyphs().find(cp);
            if (it == font.Glyphs().end()) continue;
            const UI::FontGlyph& g = it->second;
            if (g.size.x > 0.f && g.size.y > 0.f)
            {
                const ImVec2 mn(
                    penX + g.bearing.x * st.testScale,
                    baseY - g.bearing.y * st.testScale);
                const ImVec2 mx(
                    mn.x + g.size.x * st.testScale,
                    mn.y + g.size.y * st.testScale);
                dl->AddImage(atlasTex, mn, mx,
                              ImVec2(g.uv0.x, g.uv0.y),
                              ImVec2(g.uv1.x, g.uv1.y),
                              col);
            }
            penX += g.advance * st.testScale;
        }
    }

    // ---- Tips ---------------------------------------------------------------
    if (ImGui::CollapsingHeader("Tips"))
    {
        ImGui::TextDisabled(
            "* The atlas is fixed at 1024x1024 R8G8B8A8 (rgb=255, a=glyph coverage).\n"
            "* Re-baking with a larger pixel size or wider ranges may overflow;\n"
            "  the log warns when a codepoint is dropped.\n"
            "* Bake parameters live in the editor only — to ship a different\n"
            "  font, change the call in App::Run().\n"
            "* TextWidget / ButtonWidget pick up the new font on the next frame.\n"
            "* Codepoints not in the baked ranges render as nothing.");
    }

    ImGui::End();
}
