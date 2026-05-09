#include "Scene/NPRRampEditor.h"

#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <imgui.h>

#include <DirectXTex.h>

#include <cmath>
#include <cstring>
#include <filesystem>

namespace ShaderLab
{

namespace
{
    inline uint8_t F2B(float v)
    {
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // Tiny HSV→RGB helper (h: 0..1, s/v: 0..1) used by the default gradient.
    inline void HSV(float h, float s, float v, uint8_t out[3])
    {
        const float i = std::floor(h * 6.0f);
        const float f = h * 6.0f - i;
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - f * s);
        const float t = v * (1.0f - (1.0f - f) * s);
        float r = 0, g = 0, b = 0;
        switch (static_cast<int>(i) % 6)
        {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            case 5: r = v; g = p; b = q; break;
        }
        out[0] = F2B(r); out[1] = F2B(g); out[2] = F2B(b);
    }
}

int32_t NPRRampEditor::GetBindlessIndex() const
{
    return m_texture.IsValid()
        ? static_cast<int32_t>(m_texture.handle_id)
        : -1;
}

bool NPRRampEditor::Init(IGraphicsDevice& gfx)
{
    m_buffer.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    GenerateDefault();
    if (!RebuildTexture(gfx))
    {
        LOG_ERROR("NPRRampEditor::Init: RebuildTexture failed");
        return false;
    }
    LOG_INFO("NPRRampEditor: ready (%ux%u, %zu bytes CPU mirror)",
             kWidth, kHeight, m_buffer.size());
    return true;
}

void NPRRampEditor::Shutdown(IGraphicsDevice& gfx)
{
    if (m_texture.IsValid()) gfx.DestroyTexture(m_texture);
    m_texture = {};
    m_srvHandle = 0;
    m_buffer.clear();
}

void NPRRampEditor::GenerateDefault()
{
    // Each row gets its own hue (top-to-bottom rainbow) with a left-to-right
    // shadow→lit value ramp. Lets the user see distinct rows + see the
    // x-axis ramp shape before they edit anything.
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        const float hue = static_cast<float>(y) / static_cast<float>(kHeight - 1);
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const float t = static_cast<float>(x) / static_cast<float>(kWidth - 1);
            uint8_t rgb[3];
            HSV(hue, 0.55f, 0.20f + 0.80f * t, rgb);
            const size_t off = (y * kWidth + x) * 4;
            m_buffer[off + 0] = rgb[0];
            m_buffer[off + 1] = rgb[1];
            m_buffer[off + 2] = rgb[2];
            m_buffer[off + 3] = 255;
        }
    }
}

void NPRRampEditor::ApplyToRow(int row)
{
    if (row < 0 || row >= static_cast<int>(kHeight)) return;
    for (uint32_t x = 0; x < kWidth; ++x)
    {
        const float t = static_cast<float>(x) / static_cast<float>(kWidth - 1);
        const float r = m_stopStartColor[0] * (1.0f - t) + m_stopEndColor[0] * t;
        const float g = m_stopStartColor[1] * (1.0f - t) + m_stopEndColor[1] * t;
        const float b = m_stopStartColor[2] * (1.0f - t) + m_stopEndColor[2] * t;
        const float a = m_stopStartColor[3] * (1.0f - t) + m_stopEndColor[3] * t;
        const size_t off = (row * kWidth + x) * 4;
        m_buffer[off + 0] = F2B(r);
        m_buffer[off + 1] = F2B(g);
        m_buffer[off + 2] = F2B(b);
        m_buffer[off + 3] = F2B(a);
    }
}

bool NPRRampEditor::RebuildTexture(IGraphicsDevice& gfx)
{
    if (m_texture.IsValid()) gfx.DestroyTexture(m_texture);

    RHI::TextureDesc desc;
    desc.format     = RHI::Format::R8G8B8A8_UNORM;
    desc.width      = kWidth;
    desc.height     = kHeight;
    desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
    desc.usage      = RHI::Usage::DEFAULT;

    RHI::SubresourceData init{};
    init.data_ptr    = m_buffer.data();
    init.row_pitch   = kWidth * 4;
    init.slice_pitch = init.row_pitch * kHeight;

    if (!gfx.CreateTexture(desc, m_texture, &init))
    {
        LOG_ERROR("NPRRampEditor: CreateTexture failed");
        return false;
    }
    m_srvHandle = gfx.GetTextureSRVGpuHandle(m_texture);
    return m_srvHandle != 0;
}

bool NPRRampEditor::SaveToPNG(const std::string& path) const
{
    // Make sure parent dir exists.
    {
        std::error_code ec;
        std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    DirectX::Image img{};
    img.width      = kWidth;
    img.height     = kHeight;
    img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    img.rowPitch   = kWidth * 4;
    img.slicePitch = img.rowPitch * kHeight;
    img.pixels     = const_cast<uint8_t*>(m_buffer.data());

    // UTF-8 → wide
    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen > 0)
        {
            wpath.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
        }
    }

    const HRESULT hr = DirectX::SaveToWICFile(
        img, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
        wpath.c_str());
    if (FAILED(hr))
    {
        LOG_ERROR("NPRRampEditor: SaveToWICFile failed (hr=0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }
    LOG_SUCCESS("NPRRampEditor: saved '%s' (%ux%u)", path.c_str(), kWidth, kHeight);
    return true;
}

void NPRRampEditor::OnUIRender(IGraphicsDevice& gfx, bool& visible)
{
    if (!visible) return;

    if (!ImGui::Begin("NPR Ramp Editor", &visible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("512 × 25 RGBA — design doc §5.3");
    ImGui::Spacing();

    // ---- Atlas display: scale up so each row is ~6 px tall and width is
    //      half-screen ----
    const float atlasW = 512.0f;
    const float atlasH = static_cast<float>(kHeight) * 6.0f; // 25 × 6 = 150 px
    if (m_srvHandle != 0)
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(m_srvHandle),
                     ImVec2(atlasW, atlasH));

        // Click-to-select-row: map y within image to row index.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float relY = (m.y - cursor.y) / atlasH;
            int row = static_cast<int>(relY * kHeight);
            if (row >= 0 && row < static_cast<int>(kHeight))
                m_selectedRow = row;
        }

        // Highlight the currently-selected row with a thin overlay rectangle.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rowY  = cursor.y + (m_selectedRow / static_cast<float>(kHeight)) * atlasH;
        const float rowH  = atlasH / static_cast<float>(kHeight);
        dl->AddRect(ImVec2(cursor.x - 1, rowY),
                    ImVec2(cursor.x + atlasW + 1, rowY + rowH),
                    IM_COL32(255, 255, 80, 220), 0.0f, 0, 2.0f);
    }
    else
    {
        ImGui::TextDisabled("(no GPU mirror — Init failed?)");
    }

    ImGui::Spacing();
    ImGui::Text("Selected row: %d / %u", m_selectedRow, kHeight);

    // ---- Single-row magnified preview ---------------------------------------
    if (m_srvHandle != 0)
    {
        // Use UV0/UV1 to crop the atlas to just the selected row.
        const float v0 = static_cast<float>(m_selectedRow)     / kHeight;
        const float v1 = static_cast<float>(m_selectedRow + 1) / kHeight;
        ImGui::Image(static_cast<ImTextureID>(m_srvHandle),
                     ImVec2(atlasW, 40.0f),
                     ImVec2(0.0f, v0), ImVec2(1.0f, v1));
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Color stops --------------------------------------------------------
    ImGui::Text("Linear gradient (left → right):");
    ImGui::ColorEdit4("Start (shadow)", m_stopStartColor,
                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    ImGui::ColorEdit4("End (lit)",      m_stopEndColor,
                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

    if (ImGui::Button("Apply to selected row"))
    {
        ApplyToRow(m_selectedRow);
        RebuildTexture(gfx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply to ALL rows"))
    {
        for (int r = 0; r < static_cast<int>(kHeight); ++r) ApplyToRow(r);
        RebuildTexture(gfx);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- File I/O -----------------------------------------------------------
    if (ImGui::Button("Reset to default"))
    {
        GenerateDefault();
        RebuildTexture(gfx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save PNG"))
    {
        SaveToPNG("captures/ramp_export.png");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("→ captures/ramp_export.png");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Workflow: edit colors → Apply → Save PNG → drag the PNG onto a "
        "material's Ramp Texture slot in the inspector. Live-update without "
        "the manual drag is queued for the next iteration.");

    ImGui::End();
}

} // namespace ShaderLab
