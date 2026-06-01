#include "ECS/CharacterStateSystem.h"
#include "ECS/CharacterStateComponent.h"
#include "ECS/AnimationComponents.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/SkeletonAsset.h"
#include "System/Log.h"

#include <algorithm>

void CharacterStateSystem::Update(World& world, float dt, ClipLibrary& clipLib)
{
    auto* pCS = world.GetPool<CharacterStateComponent>();
    if (!pCS) return;

    auto& csEnts = pCS->Entities();
    auto& csData = pCS->Data();
    const size_t n = csData.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = csEnts[i];
        if (!world.IsAlive(e)) continue;
        CharacterStateComponent& cs = csData[i];

        AnimationComponent* anim = world.GetComponent<AnimationComponent>(e);
        if (!anim) continue;
        const SkeletonComponent* sc = world.GetComponent<SkeletonComponent>(e);
        if (!sc || sc->assetIndex == kInvalidAnimHandle) continue;
        const SkeletonAsset& skel = m_skeletons.Get(sc->assetIndex);

        // ---- 1. Lazy bind every state's clip --------------------------------
        // AcquireClip is cheap on the second call (path dedup); BindToSkeleton
        // returns the cached ClipLibrary index after the first bind. We only
        // poll states whose clipLibIdx is still invalid — fully-bound states
        // skip this branch entirely.
        if (m_clipSys)
        {
            for (auto& st : cs.states)
            {
                if (st.clipLibIdx != kInvalidClipIndex) continue;
                if (st.clipPath.empty())                continue;

                if (!st.clipResHandle.IsValid())
                    st.clipResHandle = m_clipSys->AcquireClip(st.clipPath);

                if (m_clipSys->IsReady(st.clipResHandle))
                {
                    const uint32_t idx =
                        m_clipSys->BindToSkeleton(st.clipResHandle, skel, clipLib);
                    if (idx != kInvalidClipIndex)
                        st.clipLibIdx = idx;
                }
            }
        }

        // ---- 2. Advance cross-fade ------------------------------------------
        if (cs.pendingIdx >= 0 &&
            static_cast<size_t>(cs.pendingIdx) < cs.states.size())
        {
            const auto& target = cs.states[cs.pendingIdx];
            if (target.clipLibIdx == kInvalidClipIndex)
                continue; // target not bound yet — wait

            // Initialise the secondary clip on the first frame of the blend.
            // SetState() only stamps pendingIdx + resets blendElapsed; the
            // actual AnimationComponent wire-up happens here so the system
            // owns the playback fields.
            if (anim->secondaryClip != target.clipLibIdx)
            {
                anim->secondaryClip = target.clipLibIdx;
                anim->secondaryTime = 0.f;
            }
            anim->looping = target.looping;
            anim->speed   = target.playbackSpeed;

            // Two instant-snap cases — both promote secondary → primary on
            // this frame and skip the cross-fade:
            //   (a) explicit zero-blend SetState (OnSpawn boot, hard cuts)
            //   (b) primary is still kInvalidAnimHandle. This happens on
            //       the *first* state to fully bind on a fresh entity —
            //       earlier SetState calls queued a pending state whose
            //       clip wasn't loaded yet, so primary was never written.
            //       AnimationSystem bails when primary is invalid
            //       (AnimationSystem.cpp:111), so without this branch the
            //       agent shows the rest pose for the entire blendDuration
            //       while the NavAgent already moves them in a straight
            //       line — looks like "no anim, walks in a fixed direction
            //       until the clip suddenly snaps in".
            if (cs.blendDuration <= 1e-4f ||
                anim->primaryClip == kInvalidAnimHandle)
            {
                anim->primaryClip   = anim->secondaryClip;
                anim->primaryTime   = anim->secondaryTime;
                anim->secondaryClip = kInvalidAnimHandle;
                anim->blendWeight   = 0.f;
                cs.currentIdx       = cs.pendingIdx;
                cs.pendingIdx       = -1;
                cs.blendElapsed     = 0.f;
            }
            else
            {
                cs.blendElapsed += dt;
                const float t = std::min(1.0f, cs.blendElapsed / cs.blendDuration);
                anim->blendWeight = t;
                if (t >= 1.f)
                {
                    anim->primaryClip   = anim->secondaryClip;
                    anim->primaryTime   = anim->secondaryTime;
                    anim->secondaryClip = kInvalidAnimHandle;
                    anim->blendWeight   = 0.f;
                    cs.currentIdx       = cs.pendingIdx;
                    cs.pendingIdx       = -1;
                    cs.blendElapsed     = 0.f;
                }
            }
        }
        else if (cs.currentIdx >= 0 &&
                 static_cast<size_t>(cs.currentIdx) < cs.states.size())
        {
            // No pending blend — make sure primary clip stays current. This
            // covers the case where the current state's clip only just
            // finished binding (the SetState that selected it was queued
            // before the bind completed).
            const auto& cur = cs.states[cs.currentIdx];
            if (cur.clipLibIdx != kInvalidClipIndex &&
                anim->primaryClip != cur.clipLibIdx)
            {
                anim->primaryClip = cur.clipLibIdx;
                anim->primaryTime = 0.f;
                anim->looping     = cur.looping;
                anim->speed       = cur.playbackSpeed;
            }
        }
    }
}
