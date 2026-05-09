#pragma once

// World Space UI — gameplay elements anchored in 3D world (HP bars, name
// plates, damage numbers, quest markers).  Rendered as billboarded 3D
// quads through a dedicated pass (WorldUIBillboardPass), so:
//   * perspective scaling is automatic (projection matrix does it)
//   * positioning rides on the entity's GlobalTransform — pair with
//     FollowEntityComponent / parent / direct LocalTransform
//   * depth integrates with scene (optional Test mode hides behind walls)
//   * never shares a render command with screen-space UI — no cmd-merge
//     interactions, no transform-stack hacks
//
// Per-entity layout (independent entity per visual; recommended over
// composing multiple content components on one entity — gives independent
// fade / lifetime / culling, mirrors the design doc's effect-entity rule):
//
//   Entity {
//     LocalTransform                  // position in world
//     GlobalTransform                 // populated by Transform / Follow systems
//     WorldSpaceUIComponent           // shared scale / fade / pivot / depth params
//     <Content>                       // WorldUIBar / WorldUIText / WorldUIImage / DamageNumber
//     [FollowEntityComponent]         // optional — track another entity
//   }
//
// "World Space UI" applies to anything the player needs to *read* (HP,
// names, numbers, status icons). Decorative billboard imagery (smoke,
// floating leaves, light icons) belongs in the effects / decals systems.

#include "ECS/ECS.h"
#include <string>
#include <DirectXMath.h>

namespace UI
{
    enum class ScalingMode : uint8_t
    {
        ConstantPixel = 0,    // billboard keeps fixed pixel size (HUD-on-3D feel)
        ConstantWorld = 1,    // billboard keeps fixed world size — close = bigger
    };

    enum class DepthMode : uint8_t
    {
        Always       = 0,     // always on top of scene (default — HP bars / names)
        Test         = 1,     // depth-tested — hidden when behind walls
        TestWithFade = 2,     // future: hidden portions dimmed (per-pixel alpha)
    };

    // Shared marker for any world-space UI entity. Holds the per-instance
    // scale / fade / pivot / depth settings AND per-frame computed values
    // written by WorldSpaceUISystem (read by the render pass).
    struct WorldSpaceUIComponent
    {
        // Base size of the billboard quad.
        // ConstantPixel mode: pixel size on screen.
        // ConstantWorld mode: physical size in world metres.
        DirectX::XMFLOAT2 baseSize { 1.0f, 0.2f };

        // Pivot inside the quad — which point of the billboard sits on the
        // entity's world position.  (0.5, 1.0) = bottom-centre (HP bar above
        // head).  (0.5, 0.5) = centred.  (0, 0) = top-left.
        DirectX::XMFLOAT2 pivot { 0.5f, 1.0f };

        // Camera-relative anchor offset added on top of the entity's world
        // position.  Lets the UI sit consistently to the left / right / up
        // of the entity in SCREEN SPACE regardless of how the camera
        // orbits around it.
        //   .x → along cameraRightWS  (positive = camera's right side)
        //   .y → along world up        (positive = world +Y)
        //   .z → along cameraForwardWS (positive = away from camera)
        // Use this for "name tag 0.3m to the right of the head" or "damage
        // number 2m above the hit point" rather than baking a horizontal
        // offset into LocalTransform / FollowEntity (which would flip side
        // when the camera moves to the entity's back).
        DirectX::XMFLOAT3 screenSpaceOffset { 0.0f, 0.0f, 0.0f };

        ScalingMode scalingMode = ScalingMode::ConstantWorld;

        // Fade ranges (camera-space metres).  Outside [fadeNear, fadeFar]
        // the billboard's alpha goes to 0 (and isCulled = true).
        // Set 0 to disable that endpoint.
        float fadeNear = 0.0f;
        float fadeFar  = 50.0f;

        // Hide entirely when the world position is behind the camera.
        bool  hideWhenBehindCamera = true;

        // Reserved — depth integration will be wired when the scene depth
        // SRV is plumbed into the world-UI pass.  For now Always renders
        // on top of the tonemapped scene.
        DepthMode depthMode = DepthMode::Always;

        // ---- Per-frame outputs (written by WorldSpaceUISystem) -------------
        // The render pass reads these every frame.  Editing in Inspector is
        // a no-op (re-overwritten next tick).
        float computedAlpha = 1.0f;
        float computedScale = 1.0f;
        bool  isCulled      = false;
    };

    // ---- Content components -------------------------------------------------
    // Each entity carries ONE content component telling the pass what to
    // draw inside the billboard quad. Coexist with WorldSpaceUIComponent.

    // Solid bar with bg + value-fill + border.
    struct WorldUIBarComponent
    {
        float value = 1.0f;          // 0..1
        DirectX::XMFLOAT4 fillColor       { 0.24f, 0.78f, 0.39f, 1.0f }; // green
        DirectX::XMFLOAT4 backgroundColor { 0.08f, 0.08f, 0.12f, 1.0f };
        DirectX::XMFLOAT4 borderColor     { 0.47f, 0.47f, 0.55f, 1.0f };
        float borderThick = 0.02f;   // world-space border thickness when ConstantWorld
        bool  visible     = true;
    };

    // Text rendered through the engine's font atlas.
    struct WorldUITextComponent
    {
        std::string       text;
        DirectX::XMFLOAT4 color { 1.0f, 1.0f, 1.0f, 1.0f };
        // Multiplier applied to base text size — useful when you want
        // labels visually larger/smaller than the default.
        float             scale = 1.0f;
        bool              visible = true;
    };

    struct WorldUIImageComponent
    {
        // Bindless index into the engine's t0 space2 texture table.
        // ~0u (default) = no texture → component is hidden.
        // Use IGraphicsDevice / TextureSystem to obtain a texture's
        // handle_id; that handle_id IS the bindless index.
        uint32_t bindlessIndex = ~0u;
        DirectX::XMFLOAT4 tint { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT2 uv0  { 0.0f, 0.0f };
        DirectX::XMFLOAT2 uv1  { 1.0f, 1.0f };
        bool     visible = true;
    };

    // Transient floating damage popup. WorldSpaceUISystem advances
    // `lifetime`, integrates `velocity` into `currentOffset`, fades the
    // alpha as `lifetime / totalLifetime` → 0, and destroys the entity
    // when lifetime hits 0.
    struct DamageNumberComponent
    {
        std::string       text;        // "100" / "Crit!" / "+50"
        DirectX::XMFLOAT4 color         { 1.0f, 0.4f, 0.3f, 1.0f };
        float             lifetime      = 1.5f;
        float             totalLifetime = 1.5f;
        DirectX::XMFLOAT3 velocity      { 0.0f, 1.0f, 0.0f };  // world m/s
        DirectX::XMFLOAT3 currentOffset { 0.0f, 0.0f, 0.0f };
    };

} // namespace UI
