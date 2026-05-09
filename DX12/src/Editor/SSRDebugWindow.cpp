// SSRDebugWindow.cpp — floating inspector for the Hi-Z SSR pipeline.
//
// Owned by EditorLayer (toggle: View → SSR Debug). Split into its own TU so
// EditorLayer.cpp stays navigable.
//
// Window contents
// ---------------
//   [Composite mode]     full-screen debug-mode combo (drives SSRCompositePass)
//   [Runtime tunables]   sliders for every CB knob exposed by SSRPass and
//                        SSRResolvePass — no shader recompile between edits.
//   [Per-stage previews] ImGui::Image tiles showing the raw texture output of
//                        each pass (trace, resolve, temporal, upsample,
//                        rayDir, rayLength, variance, depthHier). Works by
//                        handing ImGui the SRV GPU handle directly; no
//                        intermediate copy / readback.
//
// Why the previews matter: the composite shader only sees the FINAL upsample
// texture, so if the final is black you can't tell whether trace, resolve,
// temporal, or upsample is the broken step. Each preview tile here is the
// raw SRV for that stage — if "Raw trace" tile is dark but nothing else can
// reach farther stages, the bug is in SSRPass. If "Raw trace" is fine but
// "Resolve" is dark, the bug is in SSRResolvePass, etc.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include "Graphics/Renderer.h"
#include "RenderGraph/RenderPass/SSRPass.h"
#include "RenderGraph/RenderPass/SSRDepthHierarchyPass.h"
#include "RenderGraph/RenderPass/SceneColorPyramidPass.h"
#include "imgui/imgui.h"

namespace
{
    // Helper — draw a labelled ImGui::Image tile at the given size. Clamped to
    // a sane default if the SRV is 0 (e.g. the pass hasn't run yet). Label is
    // always shown above the image so the reader can tell which stage is
    // which without hovering.
    void DrawPreviewTile(const char* label, uint64_t gpuSrv, ImVec2 size)
    {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);
        if (gpuSrv != 0)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(gpuSrv)), size);
        }
        else
        {
            // Draw an outlined placeholder so empty tiles are visually
            // distinct from black-because-no-hits tiles.
            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
            ImGui::Dummy(size);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(topLeft,
                ImVec2(topLeft.x + size.x, topLeft.y + size.y),
                IM_COL32(20, 20, 24, 255));
            dl->AddRect(topLeft,
                ImVec2(topLeft.x + size.x, topLeft.y + size.y),
                IM_COL32(80, 80, 90, 255));
        }
        ImGui::EndGroup();
    }
}

void EditorLayer::RenderSSRDebugWindow()
{
    if (!m_showSSRDebug) return;
    if (!m_renderer)     return;

    ImGui::SetNextWindowSize(ImVec2(720, 820), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SSR Debug", &m_showSSRDebug))
    {
        ImGui::End();
        return;
    }

    auto* trace    = m_renderer->GetSSRPass();
    auto* resolve  = m_renderer->GetSSRResolvePass();
    auto* temporal = m_renderer->GetSSRTemporalPass();
    auto* upsample = m_renderer->GetSSRUpsamplePass();
    auto* comp     = m_renderer->GetSSRCompositePass();
    auto* depthHier= m_renderer->GetSSRDepthHierPass();

    if (!trace || !resolve || !temporal || !upsample || !comp)
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
            "SSR passes not initialised yet.");
        ImGui::End();
        return;
    }

    // ------------------------------------------------------------------------
    // Master enable. When off, Phase 4.6 (trace/resolve/temporal/upsample)
    // and Phase 4.7 (composite) are entirely skipped, and LightingPass falls
    // back to the probe-array handle for the SSR slot — `ssrConf` reads as
    // ~0 so the (1 - ssrConf) IBL dampening becomes a no-op and you see
    // pure IBL specular reflections (sky cubemap, no SSR).
    // ------------------------------------------------------------------------
    {
        bool ssrOn = m_renderer->IsSSREnabled();
        if (ImGui::Checkbox("Enable SSR", &ssrOn))
            m_renderer->SetSSREnabled(ssrOn);
        if (!ssrOn)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.4f, 1),
                "(disabled — IBL specular fallback only)");
        }
        ImGui::Separator();
    }

    // ------------------------------------------------------------------------
    // Full-screen debug mode (matches shader switch in SSRComposite.cs.hlsl).
    // ------------------------------------------------------------------------
    {
        const char* kModes[] = {
            "0 — Off (normal composite)",
            "1 — Final SSR only",
            "2 — Final confidence (heatmap)",
            "3 — Raw trace hit colour",
            "4 — Raw trace confidence (heatmap)",
            "5 — Ray direction (world L)",
            "6 — Roughness cutoff mask",
            "7 — Surface normal",
            "8 — SANITY: pure white (composite path test)",
            "9 — Upsample presence (green=nonzero, red=zero)",
            "10 — Raw trace presence (upstream of resolve)",
            "11 — Ray emission presence (L written at all)",
        };
        int mode = static_cast<int>(comp->GetDebugMode());
        if (ImGui::Combo("Composite mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
            comp->SetDebugMode(static_cast<uint32_t>(mode));
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "Replaces the viewport HDR with the chosen debug view.\n"
                "Mode 3/4/5 need SSRPass.Execute to have run this frame —\n"
                "if nothing changes the trace pass was skipped entirely.");
            ImGui::EndTooltip();
        }

        // Always read the live value — avoids the "slider jumps back to 1.0"
        // bug from the earlier version that re-initialised the local each frame.
        // Range goes up to 50 for diagnostic use: when SSR output is very dim
        // (happens when the scene's reflected radiance is low in HDR), a gain
        // way above physical 1.0 is the easiest way to confirm that the chain
        // is producing content at the right screen positions. Keep at 1.0 for
        // physically correct composition.
        float intensity = comp->GetIntensity();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 50.0f, "%.2f",
                               ImGuiSliderFlags_Logarithmic))
            comp->SetIntensity(intensity);
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Multiplies the mode-0 additive composite only. Debug modes\n"
                "1/3/5 now display raw texture values so the slider can't\n"
                "accidentally dim them to black. Crank above 1.0 when the\n"
                "SSR output is very dim and you want to visually confirm\n"
                "its content without a tonemap collapsing it to zero.");
    }

    ImGui::Separator();

    // ------------------------------------------------------------------------
    // Trace tunables — the CB knobs we exposed through SSRPass::SetTraceParams
    // and the two extra setters (MaxRayLength, RoughnessCutoff).
    // ------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Trace", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float    thickness    = trace->GetTraceThickness();
        uint32_t hizMost      = trace->GetHiZMostDetailed();
        float    coneMipMax   = trace->GetConeMipMax();
        float    depthBias    = trace->GetDepthBiasFactor();
        float    maxRayLength = trace->GetMaxRayLength();
        float    roughCutoff  = trace->GetRoughnessCutoff();
        bool     dirty = false;

        ImGui::TextDisabled("All values take effect next frame — no rebuild needed.");

        if (ImGui::SliderFloat("depthBiasFactor", &depthBias, 0.0f, 0.2f, "%.4f"))
            dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Fraction of linear depth used to lift origin toward camera\n"
                "before raymarch. Fixes Hi-Z self-intersection. 0 = disabled.\n"
                "Tune up if floor reflections look like \"plane color leaking\";\n"
                "tune down if reflections visibly float above caster.");

        if (ImGui::SliderFloat("traceThickness (w.u.)", &thickness, 0.005f, 5.0f, "%.3f",
                               ImGuiSliderFlags_Logarithmic))
            dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Linear-Z tolerance for hit validation. UE ships ~0.02-0.05.\n"
                "Large values over-accept thin-surface tunnel hits.");

        int hizMostInt = static_cast<int>(hizMost);
        if (ImGui::SliderInt("hizMostDetailedLvl", &hizMostInt, 0, 4))
        { hizMost = static_cast<uint32_t>(hizMostInt); dirty = true; }
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Finest Hi-Z mip the walker visits (0 = pixel-level).\n"
                "Higher = coarser cells = less self-hit risk but less precision\n"
                "near silhouettes.");

        if (ImGui::SliderFloat("coneMipMax", &coneMipMax, 0.0f, 8.0f, "%.1f"))
            dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Upper bound on scene-color pyramid mip for rough reflections.\n"
                "Higher = blurrier radiance fetch.");

        if (ImGui::SliderFloat("maxRayLength (w.u.)", &maxRayLength, 1.0f, 500.0f, "%.1f",
                               ImGuiSliderFlags_Logarithmic))
            trace->SetMaxRayLength(maxRayLength);

        if (ImGui::SliderFloat("roughnessCutoff", &roughCutoff, 0.05f, 1.0f, "%.3f"))
            trace->SetRoughnessCutoff(roughCutoff);
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Pixels with roughness above this are skipped (probe only).\n"
                "Debug mode 6 shows which pixels SSR actually runs on.");

        if (dirty)
            trace->SetTraceParams(thickness, hizMost, coneMipMax, depthBias);
    }

    if (ImGui::CollapsingHeader("Resolve"))
    {
        float cap = resolve->GetFireflyCap();
        if (ImGui::SliderFloat("fireflyCap (luminance)", &cap, 0.0f, 64.0f, "%.1f"))
            resolve->SetFireflyCap(cap);
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Post-resolve luminance clamp. 0 disables. Lower = fewer\n"
                "fireflies; too low crushes bright speculars.");
    }

    ImGui::Separator();

    // ------------------------------------------------------------------------
    // Per-stage raw-texture previews. These are the SRVs each pass writes,
    // displayed at their HDR float values (ImGui will clip to [0,1] for
    // display — fine for sanity inspection).
    // ------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Stage previews", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "HDR values are clipped to [0,1] for preview. Use the composite\n"
            "debug modes for full-viewport inspection.");

        // Tile size — responsive to window width so resizing grows the tiles.
        const float avail = ImGui::GetContentRegionAvail().x;
        const float tileW = (avail - 24.0f) * 0.5f;        // 2 columns
        const float tileH = tileW * 9.0f / 16.0f;          // 16:9 screen aspect
        const ImVec2 tileSize(tileW, tileH);

        // Row 1 — trace output (what raymarch actually found) and its rayDir.
        DrawPreviewTile("Trace: hit colour (.rgb) + conf (.a)",
                        trace->GetResultSrv(),    tileSize);
        ImGui::SameLine();
        DrawPreviewTile("Trace: ray direction L (.rgb) + PDF (.a)",
                        trace->GetRayDirPDFSrv(), tileSize);

        // Row 2 — ray length and resolve output.
        DrawPreviewTile("Trace: ray length (world units, R16F)",
                        trace->GetRayLengthSrv(), tileSize);
        ImGui::SameLine();
        DrawPreviewTile("Resolve: reweighted colour (.rgb) + conf (.a)",
                        resolve->GetColorSrv(),   tileSize);

        // Row 3 — resolve variance + temporal output.
        DrawPreviewTile("Resolve: variance (R16F, luminance²)",
                        resolve->GetVarianceSrv(), tileSize);
        ImGui::SameLine();
        DrawPreviewTile("Temporal: history-accumulated colour",
                        temporal->GetColorSrv(),   tileSize);

        // Row 4 — temporal variance + final upsample.
        DrawPreviewTile("Temporal: variance (R16F)",
                        temporal->GetVarianceSrv(), tileSize);
        ImGui::SameLine();
        DrawPreviewTile("Upsample: final SSR colour (fed to composite + Lighting)",
                        upsample->GetColorSrv(),    tileSize);

        // Row 5 — HDR snapshot. This is the copy of the HDR scene that the
        // SceneColorPyramid is built from, and what SSRPass samples for the
        // hit colour. Critical diagnostic: if this tile is BLACK but the
        // main viewport (mode 0) shows a lit scene, the snapshot copy is
        // capturing HDR at the wrong point in the frame and the trace is
        // effectively sampling an unlit scene → mode 3 (raw trace hit) will
        // also be black even when mode 4 (confidence) shows hits. If this
        // tile is LIT with the scene, the problem is downstream.
        DrawPreviewTile("HDR snapshot (feeds SceneColorPyramid; what trace samples)",
                        resolve->GetSnapshotSrv(), tileSize);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("(DepthHier preview omitted — in UAV state at ImGui time)");
        ImGui::Dummy(tileSize);
        ImGui::EndGroup();
        (void)depthHier;
    }

    ImGui::End();
}
