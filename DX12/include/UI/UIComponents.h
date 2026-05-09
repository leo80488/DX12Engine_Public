#pragma once

// UIComponents — ECS components that wire UI Roots into the World.
//
// Design doc §2.2: one Entity per UI screen (HUD, menu, etc.); the Widget
// tree itself is NOT expanded into entities. UIRootComponent owns the root
// Widget via unique_ptr — the ECS pool moves it with the entity, so cloning
// or copying is forbidden (matches the "ownership in C++" rule of §6.3).

#include "UI/Widget.h"
#include <memory>
#include <string>
#include <DirectXMath.h>

namespace UI
{
    struct UIRootComponent
    {
        std::unique_ptr<Widget> root;        // owning ptr — owns the whole tree
        std::string             name;        // debug label / Lua key
        int                     sortOrder = 0;   // lower = back, higher = front
        bool                    visible = true;
        bool                    inputEnabled = true;
        // Canvas size override. (0,0) means "use viewport size."
        Vec2                    canvasSizeOverride{ 0, 0 };

        // Move-only — the unique_ptr inside enforces this; spell it out so
        // accidental copies fail with a clear message.
        UIRootComponent() = default;
        UIRootComponent(const UIRootComponent&)            = delete;
        UIRootComponent& operator=(const UIRootComponent&) = delete;
        UIRootComponent(UIRootComponent&&)                 = default;
        UIRootComponent& operator=(UIRootComponent&&)      = default;
    };

    // Tracks which UI Root has keyboard focus and which leaf widget it
    // currently routes Char/Key events to. Optional; only present on the
    // entity that holds the focused root.
    struct UIFocusComponent
    {
        Widget* focusedWidget = nullptr;
    };

    // -----------------------------------------------------------------------
    // SCREEN-SPACE ONLY.  World-space UI (HP bars, name plates, damage
    // numbers) lives in `UI/WorldSpaceUI.h` — separate components +
    // separate render pass (WorldUIBillboardPass). The two systems never
    // share components or render commands.
    // -----------------------------------------------------------------------

    // Screen-space marker.
    //
    // For Widget-tree UIs (UIRootComponent on the same entity), the fields
    // are ignored — the root widget owns its own anchor/offset/pivot via
    // its LayoutSpec.
    //
    // For "flat" UIs (UIImageComponent / UITextComponent without a widget
    // tree), this component decides where the visual lands on the canvas:
    //   anchorX,Y in [0,1] — viewport-relative reference point
    //                        (0,0)=top-left  (0.5,0.5)=centre  (1,1)=bottom-right
    //   offsetX,Y          — pixel offset from anchor
    //   pivotX,Y in [0,1]  — pivot inside the visual itself
    //                        (0,0)=top-left of image/text
    //                        (0.5,0.5)=centre of image/text
    struct UIScreenSpaceComponent
    {
        float anchorX = 0.f;
        float anchorY = 0.f;
        float offsetX = 0.f;
        float offsetY = 0.f;
        float pivotX  = 0.f;
        float pivotY  = 0.f;
    };

    // ------------------------------------------------------------------
    // Flat-ECS UI primitives — simple alternatives to building a Widget
    // tree when you only need to display ONE image or ONE text.
    //
    // Pair with UIScreenSpaceComponent (canvas pixels) OR UIWorldSpace
    // (entity GlobalTransform). UIRootComponent is OPTIONAL; widget trees
    // and flat components on the same entity both render.
    // ------------------------------------------------------------------

    struct UIImageComponent
    {
        // GPU SRV handle of the texture (use TextureSystem::GetTexture and
        // GraphicsDX12::GetTextureSRVGpuHandle to obtain). 0 = invisible.
        uint64_t srvGpuHandle = 0;

        float    sizeX = 64.f;
        float    sizeY = 64.f;
        // Sub-region of the source texture (atlas slicing).
        float    uv0X = 0.f, uv0Y = 0.f;
        float    uv1X = 1.f, uv1Y = 1.f;
        // RGBA in 0..1 — multiplied with the sampled texel.
        // (XMFLOAT4 so the inspector picks a colour swatch + picker.)
        DirectX::XMFLOAT4 tint = { 1.f, 1.f, 1.f, 1.f };
        bool     visible = true;
    };

    struct UITextComponent
    {
        // The string the editor / Lua / gameplay can edit live.
        std::string       text;
        DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 1.f };
        float             scale = 1.f;
        bool              visible = true;
    };

    // Flat-ECS progress / HP bar — bg rect + filled portion + optional border.
    // Doesn't need UIRootComponent / Widget tree. Pair with UIScreenSpace
    // (canvas pixels) OR UIWorldSpace (entity GlobalTransform projects).
    // For world-space, scale comes from UIWorldSpaceComponent.scalingMode just
    // like UITextComponent / UIImageComponent.
    struct UIBarComponent
    {
        float             value       = 1.f;       // 0..1
        float             sizeX       = 200.f;     // pixel size at scale=1
        float             sizeY       = 24.f;
        float             borderThick = 1.f;
        DirectX::XMFLOAT4 fillColor       = { 0.24f, 0.78f, 0.39f, 1.f }; // green
        DirectX::XMFLOAT4 backgroundColor = { 0.08f, 0.08f, 0.12f, 1.f };
        DirectX::XMFLOAT4 borderColor     = { 0.47f, 0.47f, 0.55f, 1.f };
        bool              visible = true;
    };

} // namespace UI
