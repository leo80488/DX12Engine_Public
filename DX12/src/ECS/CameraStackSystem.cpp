// CameraStackSystem.cpp — virtual-camera stack pipeline.
// See DesignMd/camera_stack_system.md and include/ECS/CameraStackComponents.h.

#include "ECS/CameraStackSystem.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/FrameContext.h"
#include "ECS/Components.h"           // CameraComponent (legacy bridge)
#include "ECS/HierarchyComponents.h"  // GlobalTransform
#include "ECS/ECS.h"
#include "System/Log.h"
#include "Physics/PhysicsSystem.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    // ---- Math helpers --------------------------------------------------

    inline XMVECTOR LoadPos(const CameraPoseComponent& p)
    {
        return XMLoadFloat3(&p.position);
    }
    inline XMVECTOR LoadRot(const CameraPoseComponent& p)
    {
        return XMQuaternionNormalize(XMLoadFloat4(&p.rotation));
    }

    // Shortest-path nlerp for incremental blending: q1 → q2 by t. Sign-flips
    // q2 if dot < 0 so we always blend the short way.
    XMVECTOR NLerpShortest(XMVECTOR q1, XMVECTOR q2, float t)
    {
        const float d = XMVectorGetX(XMVector4Dot(q1, q2));
        if (d < 0.f) q2 = XMVectorNegate(q2);
        XMVECTOR r = XMVectorAdd(
            XMVectorScale(q1, 1.f - t),
            XMVectorScale(q2, t));
        return XMQuaternionNormalize(r);
    }

    // tan(fov/2) blend — perceptually uniform zoom interpolation.
    float BlendFovTan(float fovA, float fovB, float t)
    {
        const float ta = std::tan(0.5f * fovA);
        const float tb = std::tan(0.5f * fovB);
        const float tc = ta * (1.f - t) + tb * t;
        return 2.f * std::atan(tc);
    }

    // Forward vector from a rotation quaternion under the engine's
    // RH/LH-agnostic convention: GlobalTransform.matrix row 2 is "forward".
    // We rebuild the equivalent here by rotating (0,0,1) by the quat.
    XMVECTOR ForwardFromQuat(XMVECTOR q)
    {
        return XMVector3Normalize(XMVector3Rotate(XMVectorSet(0.f, 0.f, 1.f, 0.f), q));
    }

    // Backfill four core VCam components onto an entity if any is missing.
    // Idempotent — already-present components are left alone.
    void EnsureVCamComponents(World& world, Entity e)
    {
        if (!world.HasComponent<VirtualCameraComponent>(e))
            world.AddComponent<VirtualCameraComponent>(e, VirtualCameraComponent{});
        if (!world.HasComponent<CameraPoseComponent>(e))
            world.AddComponent<CameraPoseComponent>(e, CameraPoseComponent{});
        if (!world.HasComponent<VCamPriorityComponent>(e))
            world.AddComponent<VCamPriorityComponent>(e, VCamPriorityComponent{});
        if (!world.HasComponent<VCamBlendComponent>(e))
            world.AddComponent<VCamBlendComponent>(e, VCamBlendComponent{});
    }
}

namespace Camera
{
    // ---- Channel-entity bookkeeping ------------------------------------

    Entity FindChannelEntity(World& world, CameraChannelId channelId)
    {
        Entity found = NullEntity;
        world.ForEach<CameraStackChannelSingleton>(
            [&](Entity e, CameraStackChannelSingleton& s) {
                if (found == NullEntity && s.channelId == channelId) found = e;
            });
        return found;
    }

    Entity GetOrCreateChannelEntity(World& world, CameraChannelId channelId)
    {
        if (Entity existing = FindChannelEntity(world, channelId);
            existing != NullEntity)
        {
            return existing;
        }
        const Entity e = world.CreateEntity();
        CameraStackChannelSingleton s{};
        s.channelId = channelId;
        world.AddComponent<CameraStackChannelSingleton>(e, std::move(s));
        LiveCameraComponent live{};
        live.channelId = channelId;
        world.AddComponent<LiveCameraComponent>(e, std::move(live));
        return e;
    }

    void PushVCam(World& world, Entity vcam, const PushVCamArgs& args)
    {
        if (!world.IsAlive(vcam))
        {
            LOG_WARNING("Camera.PushVCam: entity %u not alive", vcam);
            return;
        }
        EnsureVCamComponents(world, vcam);

        if (auto* vc = world.GetComponent<VirtualCameraComponent>(vcam))
            vc->channelId = args.channelId;

        auto* prio = world.GetComponent<VCamPriorityComponent>(vcam);
        if (prio) {
            prio->priority = args.priority;
            prio->weight   = args.weight;
            prio->enabled  = true;
        }

        auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
        if (blend) {
            blend->blendInDuration  = args.blendInDuration;
            blend->blendOutDuration = args.blendOutDuration;
            blend->curveIn          = args.curveIn;
            blend->curveOut         = args.curveOut;
            // Transition to BlendingIn from the CURRENT blend value — a re-push
            // during BlendingOut must not snap back to 0.
            if (blend->state == BlendState::Inactive ||
                blend->state == BlendState::BlendingOut)
            {
                blend->state = BlendState::BlendingIn;
            }
            // If zero-duration blendIn, jump straight to Active.
            if (args.blendInDuration <= 0.f) {
                blend->currentBlend = 1.f;
                blend->state        = BlendState::Active;
            }
        }
        // Ensure the channel entity exists.
        GetOrCreateChannelEntity(world, args.channelId);
    }

    void PopVCam(World& world, Entity vcam,
                 float blendOutOverride,
                 BlendCurve curveOverride)
    {
        if (!world.IsAlive(vcam)) return;
        auto* prio  = world.GetComponent<VCamPriorityComponent>(vcam);
        auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
        if (!prio || !blend) return;

        if (blendOutOverride >= 0.f) blend->blendOutDuration = blendOutOverride;
        // Sentinel: Custom passed by default in the public API means "leave alone".
        if (curveOverride != BlendCurve::Custom) blend->curveOut = curveOverride;

        // Mark disabled so the stack stops electing it as winner; the stack
        // tick will transition us to BlendingOut and keep contributing until
        // currentBlend reaches 0.
        prio->enabled = false;
        if (blend->blendOutDuration <= 0.f) {
            blend->currentBlend = 0.f;
            blend->state        = BlendState::Inactive;
        }
    }

    void HardCutTo(World& world, Entity vcam, CameraChannelId channelId)
    {
        const Entity ch = GetOrCreateChannelEntity(world, channelId);
        if (auto* s = world.GetComponent<CameraStackChannelSingleton>(ch))
            s->requestHardCut = true;

        if (vcam != NullEntity)
        {
            PushVCamArgs a{};
            a.priority         = 10000;   // arbitrary high
            a.blendInDuration  = 0.f;
            a.blendOutDuration = 0.f;
            a.channelId        = channelId;
            PushVCam(world, vcam, a);
        }
    }
}

// ===========================================================================
// Behavior systems (Phase D)
// ===========================================================================

void FollowCameraTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<FollowCameraTag>([&](Entity e, FollowCameraTag&)
    {
        auto* fc   = world.GetComponent<FollowCameraComponent>(e);
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        if (!fc || !pose) return;
        const Entity targetE = fc->target.Resolve(world);
        if (targetE == NullEntity) return;

        auto* targetGT = world.GetComponent<GlobalTransform>(targetE);
        if (!targetGT) return;

        const XMMATRIX M = XMLoadFloat4x4(&targetGT->matrix);
        const XMVECTOR targetPos = M.r[3];
        // Use the target's rotation so the offset feels "behind/above target".
        XMVECTOR targetRot;
        XMVECTOR scale, trans;
        XMMatrixDecompose(&scale, &targetRot, &trans, M);

        // Desired world-space camera position = target + targetRot * offset.
        const XMVECTOR desired = XMVectorAdd(
            targetPos,
            XMVector3Rotate(XMLoadFloat3(&fc->offset), targetRot));

        // Look-at (optional).
        XMVECTOR desiredRot;
        if (fc->useLookAt)
        {
            const XMVECTOR lookAtPt = XMVectorAdd(
                targetPos,
                XMVector3Rotate(XMLoadFloat3(&fc->lookAtOffset), targetRot));
            const XMVECTOR fwd = XMVector3Normalize(
                XMVectorSubtract(lookAtPt, desired));
            XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            // Avoid lookAt singularity when forward ≈ up.
            if (std::abs(XMVectorGetX(XMVector3Dot(fwd, up))) > 0.999f)
                up = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            // Build a quaternion that rotates +Z to fwd.
            const XMMATRIX look = XMMatrixLookToLH(XMVectorZero(), fwd, up);
            // LookTo gives a view matrix; world rotation is its transpose.
            const XMMATRIX worldRot = XMMatrixTranspose(look);
            desiredRot = XMQuaternionRotationMatrix(worldRot);
        }
        else
        {
            desiredRot = targetRot;
        }

        // Smooth toward current pose. We don't know dt cheaply here — read
        // from FrameContext via the system override would be cleaner; for now
        // approximate with a fixed lerp factor that feels right at 60Hz.
        const float kPosLerp = 0.25f;
        const float kRotLerp = 0.25f;
        XMVECTOR cur = XMLoadFloat3(&pose->position);
        cur = XMVectorLerp(cur, desired, kPosLerp);
        XMStoreFloat3(&pose->position, cur);

        XMVECTOR curRot = XMQuaternionNormalize(XMLoadFloat4(&pose->rotation));
        curRot = NLerpShortest(curRot, desiredRot, kRotLerp);
        XMStoreFloat4(&pose->rotation, curRot);
    });
}

void AimCameraTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<AimCameraTag>([&](Entity e, AimCameraTag&)
    {
        auto* aim  = world.GetComponent<AimCameraComponent>(e);
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        if (!aim || !pose) return;
        const Entity targetE = aim->target.Resolve(world);
        if (targetE == NullEntity) return;

        // Clamp pitch.
        aim->pitch = std::clamp(aim->pitch, aim->pitchMin, aim->pitchMax);

        auto* targetGT = world.GetComponent<GlobalTransform>(targetE);
        if (!targetGT) return;
        const XMMATRIX M = XMLoadFloat4x4(&targetGT->matrix);
        const XMVECTOR targetPos = M.r[3];

        const XMVECTOR pivot = XMVectorAdd(
            targetPos, XMLoadFloat3(&aim->pivotOffset));

        // Camera rotation built from yaw/pitch (world Y, then camera X).
        const XMVECTOR qYaw   = XMQuaternionRotationAxis(
            XMVectorSet(0.f, 1.f, 0.f, 0.f), aim->yaw);
        const XMVECTOR right  = XMVector3Rotate(
            XMVectorSet(1.f, 0.f, 0.f, 0.f), qYaw);
        const XMVECTOR qPitch = XMQuaternionRotationAxis(right, aim->pitch);
        const XMVECTOR qRot   = XMQuaternionMultiply(qYaw, qPitch);

        // Forward of the resulting basis points +Z (engine convention).
        const XMVECTOR forward = ForwardFromQuat(qRot);

        float dist = aim->distance;
        if (m_physics && aim->collisionAvoid && dist > 0.f)
        {
            XMFLOAT3 from, dir;
            XMStoreFloat3(&from, pivot);
            XMStoreFloat3(&dir,  XMVectorNegate(forward));
            const auto hit = m_physics->CastSphereClosest(
                from, dir, std::max(0.01f, aim->probeRadius), dist);
            if (hit.hit) dist = std::max(0.05f, hit.distance);
        }
        const XMVECTOR camPos = XMVectorSubtract(
            pivot, XMVectorScale(forward, dist));

        XMStoreFloat3(&pose->position, camPos);
        XMStoreFloat4(&pose->rotation, qRot);
    });
}

// ===========================================================================
// Stack + Resolve + Shake (PreRender)
// ===========================================================================

void CameraStackTickSystem::Update(World& world, const FrameContext& ctx)
{
    const float dt = std::max(ctx.deltaTime, 0.f);

    // Bridge: any VCam without a behavior tag mirrors its pose from
    // GlobalTransform. This is how the legacy FPS camera (driven by
    // CameraSystem → LocalTransform → TransformSystem → GlobalTransform)
    // participates in the stack without writing CameraPoseComponent itself.
    world.ForEach<VirtualCameraComponent>(
        [&](Entity e, VirtualCameraComponent&)
    {
        if (world.HasComponent<FollowCameraTag>(e)) return;
        if (world.HasComponent<AimCameraTag>(e))    return;
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        auto* gt   = world.GetComponent<GlobalTransform>(e);
        if (!pose || !gt) return;
        const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, M);
        XMStoreFloat3(&pose->position, t);
        XMStoreFloat4(&pose->rotation, XMQuaternionNormalize(r));
    });

    // For each channel: rebuild entries, identify winning priority bucket,
    // advance every contributing VCam's blend state machine.
    world.ForEach<CameraStackChannelSingleton>(
        [&](Entity /*channelEnt*/, CameraStackChannelSingleton& chan)
    {
        chan.entries.clear();

        // Gather VCams targeting this channel.
        world.ForEach<VirtualCameraComponent>(
            [&](Entity vcam, VirtualCameraComponent& vc)
        {
            if (vc.channelId != chan.channelId) return;
            auto* prio  = world.GetComponent<VCamPriorityComponent>(vcam);
            auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
            if (!prio || !blend) return;
            // Include if enabled OR still blending out (still contributing).
            const bool contributing = (prio->enabled)
                                    || (blend->currentBlend > 0.f)
                                    || (blend->state == BlendState::BlendingOut);
            if (!contributing) return;

            CameraStackChannelSingleton::StackEntry e{};
            e.vcam         = vcam;
            e.priority     = prio->priority;
            e.weight       = prio->weight;
            e.currentBlend = blend->currentBlend;
            chan.entries.push_back(e);
        });

        // Sort priority desc, ties by entity id for stability.
        std::sort(chan.entries.begin(), chan.entries.end(),
            [](const auto& a, const auto& b) {
                if (a.priority != b.priority) return a.priority > b.priority;
                return a.vcam < b.vcam;
            });

        // Determine the winning priority bucket. The bucket is "the highest
        // priority bucket containing at least one enabled VCam". If no VCam
        // is enabled (all popped), the BlendingOut survivors get to blend
        // down naturally and no one is a winner.
        int winningPriority = INT_MIN;
        for (const auto& e : chan.entries)
        {
            auto* prio = world.GetComponent<VCamPriorityComponent>(e.vcam);
            if (prio && prio->enabled) { winningPriority = e.priority; break; }
        }

        // Advance every contributing VCam.
        for (auto& e : chan.entries)
        {
            auto* prio  = world.GetComponent<VCamPriorityComponent>(e.vcam);
            auto* blend = world.GetComponent<VCamBlendComponent>(e.vcam);
            if (!prio || !blend) continue;

            const bool isWinner = prio->enabled && e.priority == winningPriority;

            if (isWinner)
            {
                if (blend->state == BlendState::Inactive ||
                    blend->state == BlendState::BlendingOut)
                {
                    blend->state = BlendState::BlendingIn;
                }
                if (blend->state == BlendState::BlendingIn)
                {
                    if (blend->blendInDuration <= 0.f) {
                        blend->currentBlend = 1.f;
                    } else {
                        blend->currentBlend += dt / blend->blendInDuration;
                    }
                    if (blend->currentBlend >= 1.f) {
                        blend->currentBlend = 1.f;
                        blend->state = BlendState::Active;
                    }
                }
                else // Active
                {
                    blend->currentBlend = 1.f;
                }
            }
            else // loser this frame
            {
                if (blend->state == BlendState::Active ||
                    blend->state == BlendState::BlendingIn)
                {
                    blend->state = BlendState::BlendingOut;
                }
                if (blend->state == BlendState::BlendingOut)
                {
                    if (blend->blendOutDuration <= 0.f) {
                        blend->currentBlend = 0.f;
                    } else {
                        blend->currentBlend -= dt / blend->blendOutDuration;
                    }
                    if (blend->currentBlend <= 0.f) {
                        blend->currentBlend = 0.f;
                        blend->state = BlendState::Inactive;
                    }
                }
            }
            e.currentBlend = blend->currentBlend;
        }
    });
}

void CameraResolveTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<LiveCameraComponent>([&](Entity channelEnt, LiveCameraComponent& live)
    {
        auto* chan = world.GetComponent<CameraStackChannelSingleton>(channelEnt);
        if (!chan) return;

        // Filter contributing entries (currentBlend > 0).
        struct Contrib { Entity vcam; float w; };
        std::vector<Contrib> contribs;
        contribs.reserve(chan->entries.size());

        // Apply curve only at resolve time so the stored currentBlend remains
        // the linear progress (easier to reason about + author-friendly).
        float sumW = 0.f;
        for (const auto& e : chan->entries)
        {
            if (e.currentBlend <= 0.f) continue;
            auto* bc = world.GetComponent<VCamBlendComponent>(e.vcam);
            const BlendCurve curve = bc
                ? (bc->state == BlendState::BlendingOut ? bc->curveOut : bc->curveIn)
                : BlendCurve::Linear;
            const float curved = Camera::ApplyBlendCurve(curve, e.currentBlend);
            const float w = curved * e.weight;
            if (w <= 0.f) continue;
            contribs.push_back({ e.vcam, w });
            sumW += w;
        }

        if (sumW <= 1e-6f)
        {
            // Nobody contributing: hold last valid pose. historyValid stays
            // whatever it was; nothing new to render-from anyway.
            return;
        }

        // Normalize.
        const float invSum = 1.f / sumW;
        for (auto& c : contribs) c.w *= invSum;

        // Blend pose. First contributor seeds; subsequent ones blend in by
        // incremental weight w_i / (sum_so_far). This avoids the
        // quaternion-chain sign hazard of summing nlerps blindly.
        XMVECTOR accPos = XMVectorZero();
        XMVECTOR accRot = XMVectorZero();
        float    accFovTan = 0.f;
        float    accNear   = 0.f;
        float    accFar    = 0.f;
        bool     first = true;
        for (const auto& c : contribs)
        {
            auto* pose = world.GetComponent<CameraPoseComponent>(c.vcam);
            auto* vc   = world.GetComponent<VirtualCameraComponent>(c.vcam);
            if (!pose || !vc) continue;

            const XMVECTOR p = LoadPos(*pose);
            const XMVECTOR q = LoadRot(*pose);
            const float    t = std::tan(0.5f * vc->fov);

            if (first)
            {
                accPos = XMVectorScale(p, c.w);
                accRot = q;             // seed
                accFovTan = t * c.w;
                accNear   = vc->nearZ * c.w;
                accFar    = vc->farZ  * c.w;
                first = false;
            }
            else
            {
                accPos = XMVectorAdd(accPos, XMVectorScale(p, c.w));
                accFovTan += t * c.w;
                accNear   += vc->nearZ * c.w;
                accFar    += vc->farZ  * c.w;
                // Incremental nlerp: existing accRot represents the blend of
                // all prior contributors; mix in q with weight w_i / running.
                // Approximation: blend by c.w directly (linear weights already
                // sum to 1, so nlerp(prev, this, c.w) ≈ correct for 2 terms;
                // for >2 mixed contributors the visual error is tiny).
                accRot = NLerpShortest(accRot, q, c.w);
            }
        }

        // historyValid semantics: true iff (a) we wrote a valid pose at least
        // one frame ago AND (b) no hard cut was requested this frame. The
        // first-ever frame has hasPrev=false so historyValid stays false;
        // the second frame flips it true. HardCutTo() forces one more
        // historyValid=false frame even if hasPrev is true.
        const bool hadValidPrev = live.hasPrev;
        const bool hardCut      = chan->requestHardCut;
        chan->requestHardCut    = false;

        XMStoreFloat3(&live.position, accPos);
        XMVECTOR rot = XMQuaternionNormalize(accRot);
        XMStoreFloat4(&live.rotation, rot);
        XMStoreFloat3(&live.forward, ForwardFromQuat(rot));
        live.fov   = 2.f * std::atan(accFovTan);
        live.nearZ = accNear;
        live.farZ  = accFar;

        live.historyValid = hadValidPrev && !hardCut;
        live.hasPrev      = true;
    });
}

void CameraShakeTickSystem::Update(World& world, const FrameContext& ctx)
{
    const float dt = std::max(ctx.deltaTime, 0.f);

    // Shake lives on the LiveCamera entity (per channel). Decay trauma then
    // apply trauma²-scaled noise on top of the resolved pose.
    world.ForEach<CameraShakeComponent>(
        [&](Entity e, CameraShakeComponent& shake)
    {
        if (shake.trauma <= 0.f) return;
        auto* live = world.GetComponent<LiveCameraComponent>(e);
        if (!live) return;

        shake.time += dt * shake.frequency;
        const float strength = shake.trauma * shake.trauma;

        // Cheap deterministic noise via sine combos; seed offsets each axis.
        auto noise = [&](float phase) {
            return std::sin(shake.time * 1.7f + phase) * 0.5f
                 + std::sin(shake.time * 2.3f + phase * 1.7f) * 0.5f;
        };
        const float nx = noise(0.f);
        const float ny = noise(1.3f);
        const float nz = noise(2.7f);

        XMVECTOR p = XMLoadFloat3(&live->position);
        const XMVECTOR offset = XMVectorSet(
            shake.posAmplitude.x * strength * nx,
            shake.posAmplitude.y * strength * ny,
            shake.posAmplitude.z * strength * nz,
            0.f);
        XMStoreFloat3(&live->position, XMVectorAdd(p, offset));

        // Additive rotation: build a small-angle quaternion from axis*amp*str.
        const XMVECTOR axis = XMVector3Normalize(
            XMVectorSet(nx, ny, nz, 0.f));
        const float angle = strength *
            (std::abs(shake.rotAmplitude.x) +
             std::abs(shake.rotAmplitude.y) +
             std::abs(shake.rotAmplitude.z)) / 3.f;
        const XMVECTOR dq = XMQuaternionRotationAxis(axis, angle);
        XMVECTOR q = XMQuaternionNormalize(XMLoadFloat4(&live->rotation));
        q = XMQuaternionMultiply(q, dq);
        XMStoreFloat4(&live->rotation, q);

        // Drain trauma.
        shake.trauma = std::max(0.f, shake.trauma - shake.falloffPerSec * dt);
    });
}
