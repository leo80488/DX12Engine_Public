#pragma once

// FootIKComponent — per-entity configuration for ground-aware foot IK on
// PMX-rigged characters.
//
// The CCD-IK solver in IKSystem already pulls each foot's effector (足首)
// to the position written into its IK-control bone's LocalPose (左足ＩＫ /
// 右足ＩＫ). FootIKTargetSystem runs BETWEEN AnimationSystem::Update and
// IKSystem::Update; for entities carrying this component it raycasts the
// physics world straight down from each foot's animated position and
// overwrites the IK-control bone's local translation with the resulting
// ground point. The solver then naturally bends the leg so the ankle sits
// on the actual terrain rather than the animation's flat-ground baseline.
//
// Authoring expectations
//   • Skeleton has at least one foot IK chain whose ikBone matches
//     "左足ＩＫ" / "右足ＩＫ" / "*FootIK*" / "*foot_ik*" (case-insensitive,
//     left/right tagged via 左/右 / "Left"/"Right" / "L_"/"R_" prefix).
//   • Auto-detection runs the first time leftChainIdx / rightChainIdx are
//     still -1 — designers can override either by index in the Inspector
//     when the heuristic mislabels a rig.

#include "ECS/ECS.h"

#include <cstdint>

struct FootIKComponent : ComponentBase
{
    // Resolved IK chain indices into SkeletonAsset::ikChains. -1 means
    // "auto-detect on first tick" (default). Cleared back to -1 by the
    // Inspector "Re-detect" button.
    int32_t leftChainIdx  = -1;
    int32_t rightChainIdx = -1;

    // Master enable + smooth weight (0 = passthrough, 1 = full snap).
    // Use enableWeight for gameplay events that need to fade IK out (e.g.
    // entering a jump animation where physical foot placement is wrong).
    bool  enabled       = true;
    float enableWeight  = 1.0f;

    // Vertical search range AROUND each ankle's pre-IK world pose.
    // The ray starts at `ankleY + rayUp` and travels `rayUp + rayDown` down.
    // 0.4 / 1.0 fits human characters on terrain with ~80 cm step deltas;
    // raise rayUp for jumping rigs whose anim foot is above the IK target.
    float rayUp         = 0.4f;
    float rayDown       = 1.0f;

    // Lift the resolved ankle this far above the hit point. Pure zero would
    // place the ankle exactly on the surface — usually you want a few cm to
    // avoid the foot mesh clipping into ground tiles.
    float footOffset    = 0.02f;

    // Future flags reserved (kept as packed bools so the component stays
    // small even when the feature set grows):
    //   alignToNormal — rotate foot to match ground normal (Phase 2)
    //   pelvisDrop    — lower the pelvis when both feet over-extend (Phase 3)
    bool alignToNormal  = false;
    bool pelvisDrop     = false;
};
