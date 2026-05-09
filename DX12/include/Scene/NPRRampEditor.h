#pragma once

// NPRRampEditor — ShaderLab-only tool for authoring the 512×25 NPR ramp
// texture in-engine, per design doc §5.3. Lives in the ShaderLab.exe
// project (NOT EngineCore) because it depends on ImGui.
//
// v0 deliverable:
//   - In-memory CPU buffer (512×25 RGBA8)
//   - GPU mirror via RHI::Texture; rebuilt on Apply (small enough that
//     destroy+create is fine for tooling cadence)
//   - ImGui panel: atlas display, click-y to select row, magnified row,
//     two color stops with linear interpolation, Apply button, PNG export
//
// v0 NOT yet wired:
//   - Live material binding (user manually drags exported PNG onto a
//     material's Ramp Texture slot for now)
//   - Per-pixel painting / brush
//   - Multi-stop gradient
//   - Curve editor

#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <string>
#include <vector>

class IGraphicsDevice;

namespace ShaderLab
{

class NPRRampEditor
{
public:
    static constexpr uint32_t kWidth  = 512;
    static constexpr uint32_t kHeight = 25;

    NPRRampEditor() = default;
    ~NPRRampEditor() = default;

    // One-time setup: fills the buffer with a default vertical gradient and
    // uploads it to the GPU mirror. Returns false if texture creation fails.
    bool Init(IGraphicsDevice& gfx);

    // Releases the GPU mirror. Buffer freed by destructor.
    void Shutdown(IGraphicsDevice& gfx);

    // Renders the floating panel when @p visible is true. Caller toggles
    // visibility from the main ShaderLab panel.
    void OnUIRender(IGraphicsDevice& gfx, bool& visible);

    // GPU handle for the current ramp texture (used by ImGui::Image inside
    // OnUIRender; also exposed so future callers can bind it manually as a
    // material's RampMap before file-import flow lands).
    uint64_t GetTextureSrvGpuHandle() const { return m_srvHandle; }

    // Bindless g_AllTextures[] index for the editor's GPU mirror, or -1 if
    // the texture isn't built yet. ShaderLabScene writes this into a
    // material's textures[RAMPMAP].bindlessIndex per-frame to live-bind the
    // editor output to the test sphere. The value can change after each
    // RebuildTexture (destroy+create cycle), so callers MUST refresh per
    // frame, not cache.
    int32_t GetBindlessIndex() const;

private:
    // Fill the entire buffer with a default 25-row vertical gradient (HSV
    // hue spread top→bottom) so the panel shows something interpretable
    // before the user touches anything.
    void GenerateDefault();

    // Linear interp from m_stopStartColor to m_stopEndColor across @p row.
    void ApplyToRow(int row);

    // Destroy+recreate the GPU texture from m_buffer. Updates m_srvHandle.
    bool RebuildTexture(IGraphicsDevice& gfx);

    // Save m_buffer as a 512×25 RGBA PNG via DirectXTex. Returns false on I/O failure.
    bool SaveToPNG(const std::string& path) const;

    // 512×25×4 bytes (~50 KB).
    std::vector<uint8_t> m_buffer;
    RHI::Texture         m_texture;
    uint64_t             m_srvHandle = 0;

    int   m_selectedRow      = 0;
    float m_stopStartColor[4] = { 0.30f, 0.20f, 0.18f, 1.0f }; // shadow tone
    float m_stopEndColor[4]   = { 1.00f, 0.95f, 0.88f, 1.0f }; // lit tone
};

} // namespace ShaderLab
