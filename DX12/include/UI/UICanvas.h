#pragma once

// Entity-as-widget Canvas UI (Bevy / Unity-uGUI style).
//
// Every UI element is a plain ECS entity composed of small components:
//   UICanvas        — root marker: render mode, scaler ref-resolution, sort order.
//   UIRect          — layout INPUT: anchors / pivot / size / offset (RectTransform).
//   UIParent        — hierarchy edge: which entity is this node's parent.
//   UIImage/UIText  — visual data (texture quad / SDF text + effects).
//   UIInteractable  — raycast target + interaction state (hover/press/click).
//   UIComputedRect  — layout OUTPUT: resolved screen-space rect + sort key.
//   UILayoutDirty / UIBatchDirty — dirty tags (re-layout / re-batch markers).
//
// UICanvasSystem (UI/UICanvasSystem.h) consumes these once per frame:
//   resolve scale -> build hierarchy -> layout -> interaction -> emit to UIDrawList.
//
// This coexists with the legacy widget-tree (UIRootComponent) and the flat-ECS
// (UIScreenSpaceComponent + UIImageComponent/UITextComponent) paths — all three
// emit into the same UIDrawList / UIPass. An entity is canvas-driven when it has
// a UIRect (its geometry comes from UIComputedRect, not UIScreenSpaceComponent).

#include "UI/UIDrawList.h"          // Vec2, Rect, Color32, TextEffectPreset
#include "ECS/ECS.h"               // Entity, NullEntity
#include "ECS/Guid.h"              // ECS::Guid (stable serialized parent ref)
#include "ECS/GuidRegistry.h"      // ECS::EnsureGuidOn (parent-attach helper)
#include "Resource/SystemHandles.h" // Resource::TextureHandle

#include <string>
#include <DirectXMath.h>

namespace UI
{
    // ---- Canvas root --------------------------------------------------------
    enum class CanvasRenderMode : uint32_t
    {
        ScreenSpaceOverlay = 0, // draws on top of everything in screen pixels
        // ScreenSpaceCamera / WorldSpace are future modes.
    };

    enum class CanvasScaleMode : uint32_t
    {
        ConstantPixelSize   = 0, // 1 UI unit == 1 screen pixel
        ScaleWithScreenSize = 1, // scale UI by screen vs reference resolution
    };

    struct UICanvas
    {
        CanvasRenderMode renderMode = CanvasRenderMode::ScreenSpaceOverlay;
        CanvasScaleMode  scaleMode  = CanvasScaleMode::ScaleWithScreenSize;
        Vec2  referenceResolution { 1920.f, 1080.f };
        float matchWidthOrHeight = 0.5f; // 0 = match width, 1 = match height
        int   sortOrder = 0;             // higher canvas draws on top

        // Runtime (written by UICanvasSystem each frame).
        float computedScale = 1.f;
    };

    // ---- Layout input (Unity RectTransform model) ---------------------------
    // anchorMin/Max in [0,1] of the parent rect. When equal -> point anchor
    // (size = `size`, positioned by `offset`); when different -> the element
    // stretches across that anchor span and `size` becomes padding (sizeDelta).
    struct UIRect
    {
        Vec2 anchorMin { 0.5f, 0.5f };
        Vec2 anchorMax { 0.5f, 0.5f };
        Vec2 pivot     { 0.5f, 0.5f };
        Vec2 size      { 160.f, 40.f }; // sizeDelta (px, scaled by canvas)
        Vec2 offset    { 0.f, 0.f };    // anchoredPosition (px, scaled by canvas)
    };

    // ---- Hierarchy edge -----------------------------------------------------
    struct UIParent
    {
        Entity    parent = NullEntity; // runtime resolved parent entity
        ECS::Guid parentGuid{};        // stable serialized reference (round-trips)
    };

    // ---- Visual: textured quad (fills the computed rect) --------------------
    struct UIImage
    {
        std::string             texturePath;            // stable identity (serialized)
        Resource::TextureHandle texture{};              // runtime handle cache
        uint64_t                srvGpuHandle = 0;        // runtime, re-resolved each frame
        DirectX::XMFLOAT4       color = { 1.f, 1.f, 1.f, 1.f };
        Vec2                    uv0 { 0.f, 0.f };
        Vec2                    uv1 { 1.f, 1.f };
        // UV addressing: Clamp (default, atlas sub-rects), Wrap/Mirror (tiling
        // when uv1 > 1). pointFilter = nearest sampling (pixel-art / sprites).
        UIWrapMode              wrapMode = UIWrapMode::Clamp;
        bool                    pointFilter = false;
        bool                    visible = true;
    };

    // Sprite-sheet (atlas) animation. Drives a sibling UIImage's uv0/uv1 by
    // stepping through a `columns` x `rows` grid of frames. The image's texture
    // is the full sheet; this component picks the current frame's sub-rect.
    struct UISpriteAnimComponent
    {
        int   columns    = 4;     // frames per row
        int   rows       = 4;     // rows of frames
        int   frameCount = 0;     // 0 = columns*rows (skip trailing empty cells otherwise)
        float fps        = 12.f;
        bool  loop       = true;
        bool  playing    = true;
        bool  pingpong   = false; // bounce back and forth instead of wrapping

        // Runtime.
        float elapsed = 0.f;
        int   frame   = 0;
    };

    // ---- Visual: SDF text (aligned within the computed rect) ----------------
    struct UIText
    {
        std::string       text = "Text";
        DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 1.f };
        float             fontScale = 1.f;   // multiplier on the baked font size
        int               alignH = 1;        // 0 left, 1 centre, 2 right
        int               alignV = 1;        // 0 top,  1 middle, 2 bottom
        bool              visible = true;

        // Effects (same SDF effect set as UITextComponent).
        TextEffectPreset  effectPreset = TextEffectPreset::None;
        DirectX::XMFLOAT4 outlineColor = { 0.f, 0.f, 0.f, 1.f };
        float             outlineWidth = 2.f;
        DirectX::XMFLOAT4 glowColor    = { 1.f, 0.85f, 0.4f, 1.f };
        float             glowWidth    = 4.f;
        DirectX::XMFLOAT4 shadowColor  = { 0.f, 0.f, 0.f, 0.6f };
        float             shadowOffsetX = 2.f;
        float             shadowOffsetY = 2.f;
        float             jitterAmplitude = 2.f;
        float             jitterFrequency = 12.f;
    };

    // ---- Interaction: raycast target + state --------------------------------
    struct UIInteractable
    {
        bool raycastTarget = true;
        bool disabled      = false;

        // Runtime state (written by UICanvasSystem; poll these from gameplay/Lua).
        bool hovered = false;
        bool pressed = false;
        bool clicked = false; // true for ONE frame on release-inside

        // Optional Unity-Selectable-style state tinting of the sibling UIImage.
        bool              tintTransition = false;
        DirectX::XMFLOAT4 normalColor   = { 1.f, 1.f, 1.f, 1.f };
        DirectX::XMFLOAT4 hoverColor    = { 0.92f, 0.92f, 0.92f, 1.f };
        DirectX::XMFLOAT4 pressedColor  = { 0.78f, 0.78f, 0.78f, 1.f };
        DirectX::XMFLOAT4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f };
    };

    // ---- Layout output ------------------------------------------------------
    struct UIComputedRect
    {
        Rect rect;            // resolved screen-space rect (pixels)
        int  sortKey = 0;     // canvas sortOrder major + tree order minor
        int  depth   = 0;     // tree depth (0 = canvas root)
        bool culled  = false; // fully outside the screen
        bool valid   = false; // computed this frame
    };

    // ---- Dirty tags (empty markers) -----------------------------------------
    // Present so gameplay/editor can signal changes; UICanvasSystem clears them
    // after processing. (The layout currently recomputes every frame for
    // correctness; the tags are the hook for future incremental layout/batching.)
    struct UILayoutDirty {};
    struct UIBatchDirty  {};

    // Attach @p child under @p parent in the UI hierarchy, stamping a stable
    // GUID on the parent so the edge survives save/load. Use this (editor /
    // gameplay / Lua) instead of writing UIParent directly.
    inline void AttachUIParent(World& world, Entity child, Entity parent)
    {
        UIParent up;
        up.parent = parent;
        if (parent != NullEntity && world.IsAlive(parent))
            up.parentGuid = ECS::EnsureGuidOn(world, parent);
        world.AddComponent<UIParent>(child, up);
    }

    // Advance every UISpriteAnimComponent by @p dt and write the current
    // frame's grid sub-rect into the same entity's UIImage (canvas) or
    // UIImageComponent (flat) uv0/uv1. Call once per frame BEFORE the UI
    // systems render. Defined in UICanvasSystem.cpp.
    void AdvanceSpriteAnimations(World& world, float dt);

} // namespace UI
