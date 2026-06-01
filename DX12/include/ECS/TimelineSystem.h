#pragma once

// TimelineSystem — cross-time dispatcher for AnimNotify-style events.
//
// Two coexisting sources of notify tracks, both fired into the same Pending*
// mailboxes (additive — a consumer can't tell which source produced a cmd):
//
//   Path A (primary, Unreal AnimSequence-style):
//     Notify tracks authored ON the animation clip. For every entity with an
//     AnimationComponent whose primaryClip is valid, the clip's notifyTracks
//     (looked up in the ClipLibrary) are evaluated against the window
//     [prevNotifyTime, primaryTime] using the per-entity cursor stored on the
//     AnimationComponent. One authored set drives every entity that plays the
//     clip — no per-entity authoring required.
//
//   Path B (legacy / per-entity override):
//     Notify tracks stored on a TimelineComponent attached to the entity.
//     Uses the component's own lastObservedTime cursor. Kept for backward
//     compatibility and instance-specific events; new authoring goes on the
//     clip via Path A.
//
// Scheduler placement: TickPhase::Animation, registered AFTER the system that
// mutates AnimationComponent::primaryTime so the time window is the latest
// one. Consumer systems (Hitbox / VFX / Camera / Audio / StateToggle /
// GenericNotify) live in TickPhase::BoneAttachment so they drain the mailbox
// in the same render frame.
//
// Notes:
//   - Looping clips wrap on duration. The cross-time test handles
//     (prev, end] ∪ [0, curr] when curr < prev.
//   - "Muted" tracks are skipped entirely.
//   - First sight / clip switch snapshots the time without firing, so opening
//     a clip doesn't replay every notify at once.
//   - Paused animations don't fire (matches editor preview pause semantics).

#include "ECS/ISystem.h"

class ClipLibrary;

class TimelineSystem final : public SystemInPhase<TickPhase::Animation>
{
public:
    // `clips` is the engine's ClipLibrary (owned by the Renderer / skinning
    // subsystem). Stored by reference — the library object outlives the
    // system; only its contents change across world reloads, which is safe
    // because lookups are guarded by index < Count().
    explicit TimelineSystem(const ClipLibrary& clips) : m_clips(clips) {}

    const char* GetName() const override { return "TimelineSystem"; }

    void Update(World& world, const FrameContext& ctx) override;

private:
    const ClipLibrary& m_clips;
};
