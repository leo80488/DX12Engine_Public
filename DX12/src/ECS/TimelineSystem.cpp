#include "ECS/TimelineSystem.h"

#include "ECS/AnimationComponents.h"
#include "ECS/FrameContext.h"
#include "ECS/NotifyTypes.h"
#include "Resource/SkeletonAsset.h"   // ClipLibrary, ClipAsset

#include <algorithm>
#include <cmath>

namespace
{
    // True iff `t` lies in the half-open interval (prev, curr] on a looping
    // clip of length `duration`. Handles wrap when curr < prev. Half-open at
    // `prev` so a notify exactly at the wrap point fires once per loop, not
    // twice; closed at `curr` so the instantaneous endpoint catches notifies
    // authored at clip boundaries.
    bool CrossedTime(float prev, float curr, float duration, float t)
    {
        if (duration <= 0.f) return false;

        auto wrap = [duration](float x) {
            float r = std::fmod(x, duration);
            return (r < 0.f) ? r + duration : r;
        };

        prev = wrap(prev);
        curr = wrap(curr);

        if (prev <= curr) return t > prev && t <= curr;
        // wrapped: hit if in (prev, duration) ∪ [0, curr]
        return (t > prev) || (t <= curr);
    }

    // ---- Per-category dispatch into the Pending* mailbox components ---------
    // Shared by both the clip-authoritative path and the legacy
    // TimelineComponent path. A `world.AddComponent` here is safe: the
    // mailbox component type is distinct from the pool being iterated.

    void DispatchPoint(World& world, Entity e, const Notify& n)
    {
        switch (n.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type     = static_cast<HitboxCmdType>(n.params.GetInt("cmdType", 0));
            cmd.slot     = static_cast<uint32_t>(n.params.GetInt("slot", 0));
            cmd.params.radius      = n.params.GetFloat("radius",   0.5f);
            cmd.params.damage      = n.params.GetFloat("damage",  10.f);
            cmd.params.hitGroup    = static_cast<uint32_t>(n.params.GetInt("hitGroup", 0));
            cmd.params.localOffset = n.params.GetVec3("offset", {0,0,0});
            mb->commands.push_back(cmd);
        } break;

        case NotifyCategory::VFX: {
            auto* mb = world.GetComponent<PendingVFXSpawns>(e);
            if (!mb) { world.AddComponent<PendingVFXSpawns>(e, {}); mb = world.GetComponent<PendingVFXSpawns>(e); }
            PendingVFXSpawns::Spawn s;
            s.assetPath = n.params.GetString("asset");
            s.mode      = static_cast<VFXAttachMode>(n.params.GetInt("attach", 0));
            s.boneIndex = n.params.GetInt("bone", -1);
            s.localPos  = n.params.GetVec3("offset", {0,0,0});
            s.duration  = n.params.GetFloat("duration", 1.f);
            mb->spawns.push_back(std::move(s));
        } break;

        case NotifyCategory::Camera: {
            auto* mb = world.GetComponent<PendingCameraEffects>(e);
            if (!mb) { world.AddComponent<PendingCameraEffects>(e, {}); mb = world.GetComponent<PendingCameraEffects>(e); }
            PendingCameraEffects::Effect ef;
            ef.type = static_cast<CameraEffectType>(n.params.GetInt("kind", 0));
            ef.params.shakeAmplitude = n.params.GetFloat("amp",      0.2f);
            ef.params.shakeFrequency = n.params.GetFloat("freq",    20.f);
            ef.params.fovDelta       = n.params.GetFloat("fovDelta", 0.f);
            ef.params.duration       = n.params.GetFloat("duration",0.05f);
            ef.params.timeScale      = n.params.GetFloat("scale",   0.05f);
            mb->effects.push_back(ef);
        } break;

        case NotifyCategory::Audio: {
            auto* mb = world.GetComponent<PendingAudioPlays>(e);
            if (!mb) { world.AddComponent<PendingAudioPlays>(e, {}); mb = world.GetComponent<PendingAudioPlays>(e); }
            PendingAudioPlays::Play p;
            p.clipPath   = n.params.GetString("clip");
            p.category   = static_cast<AudioCategoryTag>(n.params.GetInt("cat", 0));
            p.volume     = n.params.GetFloat("volume", 1.f);
            p.pitch      = n.params.GetFloat("pitch",  1.f);
            p.attachBone = n.params.GetInt("bone", -1);
            mb->plays.push_back(std::move(p));
        } break;

        case NotifyCategory::StateToggle: {
            auto* mb = world.GetComponent<PendingStateToggles>(e);
            if (!mb) { world.AddComponent<PendingStateToggles>(e, {}); mb = world.GetComponent<PendingStateToggles>(e); }
            PendingStateToggles::Toggle t;
            t.tag      = static_cast<StateTag>(n.params.GetInt("tag", 1));
            t.enable   = n.params.GetInt("enable", 1) != 0;
            t.duration = n.params.GetFloat("duration", 0.f);
            mb->toggles.push_back(t);
        } break;

        case NotifyCategory::Custom: {
            auto* mb = world.GetComponent<PendingGenericNotifies>(e);
            if (!mb) { world.AddComponent<PendingGenericNotifies>(e, {}); mb = world.GetComponent<PendingGenericNotifies>(e); }
            PendingGenericNotifies::Notify gn;
            gn.typeId = StringID(n.params.GetString("type", n.displayName));
            gn.params = n.params;
            mb->notifies.push_back(std::move(gn));
        } break;

        default: break;
        }
    }

    // NotifyState Begin — interval start crossed going forward. Mapped to the
    // same mailbox cmd layout as a point notify so consumers need no new
    // branch. Begin only carries meaning for Hitbox / StateToggle / Custom.
    void DispatchStateBegin(World& world, Entity e, const NotifyState& s)
    {
        switch (s.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type     = HitboxCmdType::Begin;
            cmd.slot     = static_cast<uint32_t>(s.params.GetInt("slot", 0));
            cmd.params.radius      = s.params.GetFloat("radius",   0.5f);
            cmd.params.damage      = s.params.GetFloat("damage",  10.f);
            cmd.params.hitGroup    = static_cast<uint32_t>(s.params.GetInt("hitGroup", 0));
            cmd.params.localOffset = s.params.GetVec3("offset", {0,0,0});
            mb->commands.push_back(cmd);
        } break;
        case NotifyCategory::StateToggle: {
            auto* mb = world.GetComponent<PendingStateToggles>(e);
            if (!mb) { world.AddComponent<PendingStateToggles>(e, {}); mb = world.GetComponent<PendingStateToggles>(e); }
            PendingStateToggles::Toggle t;
            t.tag      = static_cast<StateTag>(s.params.GetInt("tag", 1));
            t.enable   = true;   // Begin → enable; interval bounds the window
            t.duration = 0.f;
            mb->toggles.push_back(t);
        } break;
        // Camera / VFX / Audio / Custom: a NotifyState's start edge fires the
        // same one-shot effect a point Notify would, so an interval authored on
        // these categories actually triggers (e.g. a Camera-Shake NotifyState
        // kicks the shake on Begin). Without this, those intervals did nothing.
        default: {
            Notify n;
            n.id          = s.id;
            n.category    = s.category;
            n.time        = s.startTime;
            n.params      = s.params;
            n.displayName = s.displayName;
            n.color       = s.color;
            DispatchPoint(world, e, n);
        } break;
        }
    }

    // NotifyState End — interval end crossed. Hitbox → End cmd, StateToggle →
    // disable, Custom → "<type>:end" event. Camera/VFX/Audio are one-shots
    // fired on Begin, so their End edge does nothing.
    void DispatchStateEnd(World& world, Entity e, const NotifyState& s)
    {
        switch (s.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type = HitboxCmdType::End;
            cmd.slot = static_cast<uint32_t>(s.params.GetInt("slot", 0));
            cmd.params.hitGroup = static_cast<uint32_t>(s.params.GetInt("hitGroup", 0));
            mb->commands.push_back(cmd);
        } break;
        case NotifyCategory::StateToggle: {
            auto* mb = world.GetComponent<PendingStateToggles>(e);
            if (!mb) { world.AddComponent<PendingStateToggles>(e, {}); mb = world.GetComponent<PendingStateToggles>(e); }
            PendingStateToggles::Toggle t;
            t.tag    = static_cast<StateTag>(s.params.GetInt("tag", 1));
            t.enable = false;
            mb->toggles.push_back(t);
        } break;
        case NotifyCategory::Custom: {
            auto* mb = world.GetComponent<PendingGenericNotifies>(e);
            if (!mb) { world.AddComponent<PendingGenericNotifies>(e, {}); mb = world.GetComponent<PendingGenericNotifies>(e); }
            PendingGenericNotifies::Notify gn;
            gn.typeId = StringID(s.params.GetString("type", s.displayName) + ":end");
            gn.params = s.params;
            mb->notifies.push_back(std::move(gn));
        } break;
        default: break;
        }
    }

    // Evaluate one track list over the [prev, curr] window (clip length
    // `duration`) and dispatch every crossed point notify / state edge.
    void FireTracks(World& world, Entity e,
                    const std::vector<NotifyTrack>& tracks,
                    float prev, float curr, float duration)
    {
        for (const auto& track : tracks)
        {
            if (track.muted) continue;

            for (const auto& s : track.states)
            {
                if (CrossedTime(prev, curr, duration, s.startTime)) DispatchStateBegin(world, e, s);
                if (CrossedTime(prev, curr, duration, s.endTime))   DispatchStateEnd(world, e, s);
            }
            for (const auto& n : track.notifies)
            {
                if (CrossedTime(prev, curr, duration, n.time)) DispatchPoint(world, e, n);
            }
        }
    }
}

void TimelineSystem::Update(World& world, const FrameContext&)
{
    // ---- Path A: clip-authored notify tracks (Unreal AnimSequence-style) ----
    // Drive notifies straight off the active clip via the per-entity cursor
    // stored on the AnimationComponent. One authored set covers every entity
    // that plays the clip.
    const uint32_t clipCount = m_clips.Count();
    world.ForEach<AnimationComponent>([&](Entity e, AnimationComponent& anim)
    {
        if (anim.primaryClip == kInvalidAnimHandle || anim.primaryClip >= clipCount)
            return;

        const ClipAsset& clip = m_clips.Get(anim.primaryClip);
        if (clip.notifyTracks.empty() || clip.duration <= 0.f)
        {
            // Keep the cursor tracking so a later switch to a notify-bearing
            // clip doesn't replay a stale window.
            anim.prevNotifyTime = anim.primaryTime;
            anim.lastNotifyClip = anim.primaryClip;
            return;
        }

        const float curr = anim.primaryTime;

        // First sight or clip switch → snapshot, don't fire.
        if (anim.prevNotifyTime < 0.f || anim.lastNotifyClip != anim.primaryClip)
        {
            anim.prevNotifyTime = curr;
            anim.lastNotifyClip = anim.primaryClip;
            return;
        }

        const float prev = anim.prevNotifyTime;
        anim.prevNotifyTime = curr;
        anim.lastNotifyClip = anim.primaryClip;

        if (anim.paused || prev == curr) return;

        FireTracks(world, e, clip.notifyTracks, prev, curr, clip.duration);
    });

    // ---- Path B: legacy per-entity TimelineComponent override tracks --------
    // Uses the component's own lastObservedTime cursor. Additive to Path A.
    world.ForEach<TimelineComponent>([&](Entity e, TimelineComponent& tl)
    {
        const auto* anim = world.GetComponent<AnimationComponent>(e);
        if (!anim || anim->paused) return;
        if (tl.clipDuration <= 0.f) return;
        if (tl.tracks.empty()) return;

        const float curr = anim->primaryTime;

        if (tl.lastObservedTime < 0.f) { tl.lastObservedTime = curr; return; }

        const float prev = tl.lastObservedTime;
        tl.lastObservedTime = curr;
        if (prev == curr) return;

        FireTracks(world, e, tl.tracks, prev, curr, tl.clipDuration);
    });
}
