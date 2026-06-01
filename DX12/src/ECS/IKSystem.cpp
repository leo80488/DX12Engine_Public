#include "ECS/IKSystem.h"
#include "ECS/AnimationComponents.h"
#include "System/TaskSystem.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a TRS matrix from a LocalPose (row-major, row-vector convention).
// Promoted to IKSystem public static so FootIKTargetSystem can reuse the
// exact same composition order — diverging would silently produce a
// different world pose than IKSystem itself reads on the same frame.
XMMATRIX IKSystem::LocalPoseToMatrix(const AnimationSystem::LocalPose& p)
{
    XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
    XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
    XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
    return S * R * T;
}
// Backwards-compat shim for the rest of this TU.
static XMMATRIX LocalPoseToMat(const AnimationSystem::LocalPose& p)
{
    return IKSystem::LocalPoseToMatrix(p);
}

static float Clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// ---------------------------------------------------------------------------
// Euler angle utilities (ported from saba)
// ---------------------------------------------------------------------------

static float NormalizeAngle(float angle)
{
    float ret = angle;
    while (ret >= XM_2PI) ret -= XM_2PI;
    while (ret < 0.f)    ret += XM_2PI;
    return ret;
}

static float DiffAngle(float a, float b)
{
    float diff = NormalizeAngle(a) - NormalizeAngle(b);
    if (diff > XM_PI)       return diff - XM_2PI;
    else if (diff < -XM_PI) return diff + XM_2PI;
    return diff;
}

// Decompose a rotation matrix into XYZ Euler angles, choosing the representation
// closest to 'before' to prevent flipping. (Ported from saba, col→row-major conversion.)
//
// saba uses column-major glm where m[col][row].
// Our row-major XMFLOAT4X4 rm._RC uses 1-indexed row/col.
// Mapping: glm m[col][row] = M_col(row,col) → M_row(col,row) = rm._((col+1)(row+1))
static XMFLOAT3 DecomposeEuler(const XMFLOAT4X4& rm, const XMFLOAT3& before)
{
    XMFLOAT3 r;

    // rm._13 corresponds to saba's m[0][2] = sin(ry)
    float sy = -rm._13;
    const float e = 1.0e-6f;

    if ((1.0f - std::fabsf(sy)) < e)
    {
        // Gimbal lock
        r.y = std::asinf(Clampf(sy, -1.f, 1.f));

        float sx = std::sinf(before.x);
        float sz = std::sinf(before.z);
        if (std::fabsf(sx) < std::fabsf(sz))
        {
            float cx = std::cosf(before.x);
            if (cx > 0.f)
            {
                r.x = 0.f;
                r.z = std::asinf(Clampf(-rm._21, -1.f, 1.f)); // saba: -m[1][0]
            }
            else
            {
                r.x = XM_PI;
                r.z = std::asinf(Clampf(rm._21, -1.f, 1.f));  // saba: m[1][0]
            }
        }
        else
        {
            float cz = std::cosf(before.z);
            if (cz > 0.f)
            {
                r.z = 0.f;
                r.x = std::asinf(Clampf(-rm._32, -1.f, 1.f)); // saba: -m[2][1]
            }
            else
            {
                r.z = XM_PI;
                r.x = std::asinf(Clampf(rm._32, -1.f, 1.f));  // saba: m[2][1]
            }
        }
    }
    else
    {
        r.x = std::atan2f(rm._23, rm._33); // saba: atan2(m[1][2], m[2][2])
        r.y = std::asinf(Clampf(-rm._13, -1.f, 1.f));  // saba: asin(-m[0][2])
        r.z = std::atan2f(rm._12, rm._11); // saba: atan2(m[0][1], m[0][0])
    }

    // Test 8 alternative Euler representations; pick closest to 'before'
    const float pi = XM_PI;
    XMFLOAT3 tests[] =
    {
        { r.x + pi,  pi - r.y, r.z + pi },
        { r.x + pi,  pi - r.y, r.z - pi },
        { r.x + pi, -pi - r.y, r.z + pi },
        { r.x + pi, -pi - r.y, r.z - pi },
        { r.x - pi,  pi - r.y, r.z + pi },
        { r.x - pi,  pi - r.y, r.z - pi },
        { r.x - pi, -pi - r.y, r.z + pi },
        { r.x - pi, -pi - r.y, r.z - pi },
    };

    float errX = std::fabsf(DiffAngle(r.x, before.x));
    float errY = std::fabsf(DiffAngle(r.y, before.y));
    float errZ = std::fabsf(DiffAngle(r.z, before.z));
    float minErr = errX + errY + errZ;

    for (const auto& t : tests)
    {
        float err = std::fabsf(DiffAngle(t.x, before.x))
                  + std::fabsf(DiffAngle(t.y, before.y))
                  + std::fabsf(DiffAngle(t.z, before.z));
        if (err < minErr)
        {
            minErr = err;
            r = t;
        }
    }

    return r;
}

// Reconstruct a quaternion from XYZ Euler angles (same order as saba: Rz * Ry * Rx).
// In DXMath Multiply convention: Multiply(Multiply(Rx, Ry), Rz) gives the same rotation.
static XMVECTOR QuatFromEulerXYZ(float rx, float ry, float rz)
{
    XMVECTOR qx = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), rx);
    XMVECTOR qy = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), ry);
    XMVECTOR qz = XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), rz);
    return XMQuaternionMultiply(XMQuaternionMultiply(qx, qy), qz);
}

// ---------------------------------------------------------------------------
// Quaternion convention helpers
//
// saba (glm):   Hamilton product: glm's q1 * q2 = H(q1, q2)
//               "apply q2 first, then q1"
// DirectXMath:  XMQuaternionMultiply(q1, q2) = H(q2, q1)
//               "apply q1 first, then q2"
//
// So:  glm's (A * B) ↔ DXMath Multiply(B, A)
//
// saba convention:
//   finalRot = ikRot * animRot     →  DX: Multiply(animRot, ikRot)
//   chainRot = ikRot * animRot * delta → DX: Multiply(Multiply(delta, animRot), ikRot)
//   ikRot    = chainRot * inv(animRot)  → DX: Multiply(conj(animRot), chainRot)
// ---------------------------------------------------------------------------

// Compose: final = ikRot ∘ animRot (saba order: animRot first, then ikRot)
static XMVECTOR ComposeIKAnim(XMVECTOR ikRot, XMVECTOR animRot)
{
    return XMQuaternionMultiply(animRot, ikRot);
}

// Extract IK rotation: ikRot = final ∘ inv(animRot) (saba convention)
static XMVECTOR ExtractIKRot(XMVECTOR chainRot, XMVECTOR animRot)
{
    return XMQuaternionMultiply(XMQuaternionConjugate(animRot), chainRot);
}

// chainRot = ikRot * animRot * delta (saba order)
static XMVECTOR CombineChainRot(XMVECTOR ikRot, XMVECTOR animRot, XMVECTOR delta)
{
    return XMQuaternionMultiply(XMQuaternionMultiply(delta, animRot), ikRot);
}


// ===========================================================================
// IKSystem::Update
// ===========================================================================
void IKSystem::Update(World& world, const std::unordered_set<Entity>* activeSet)
{
    // Editor toggle (see IKSystem::SetEnabled). When disabled, skeletons
    // keep the raw pose AnimationSystem::Update just wrote — no IK pass
    // runs, no per-skeleton iteration cost.
    if (!m_enabled) return;

    struct IKJob {
        AnimationSystem::LocalPose* poses;
        const SkeletonAsset*        asset;
        uint32_t                    boneCount;
    };
    std::vector<IKJob> jobs;

    // Iterate SkeletonComponent pool directly.
    auto* pSkel = world.GetPool<SkeletonComponent>();
    auto* pAnim = world.GetPool<AnimationComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = pSkel ? pSkel->Entities() : std::vector<Entity>{};
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        auto* skel = &pSkel->Data()[si];
        if (skel->assetIndex == kInvalidAnimHandle) continue;

        // Skip IK on un-animated skeletons: rest-pose LocalPose now exists
        // (so morphs can deform) but the IK target bones sit at their bind
        // positions, which the solver would otherwise yank chain links toward.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (!anim || anim->primaryClip == kInvalidAnimHandle) continue;

        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);
        if (asset.ikChains.empty()) continue;

        jobs.push_back({ poses, &asset, skel->boneCount });
    }

    // Phase 2 (parallel): solve IK chains per entity.
    // Each entity's LocalPose array is independent — no conflicts.
    if (jobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(jobs.size()),
            [&](uint32_t i)
            {
                auto& j = jobs[i];
                for (const auto& chain : j.asset->ikChains)
                    SolveChain(chain, j.poses, *j.asset, j.boneCount);
            });
    }
    else if (!jobs.empty())
    {
        auto& j = jobs[0];
        for (const auto& chain : j.asset->ikChains)
            SolveChain(chain, j.poses, *j.asset, j.boneCount);
    }
}

// ===========================================================================
// IKSystem::ComputeWorldTransform
// ===========================================================================
XMMATRIX IKSystem::ComputeBoneWorldTransform(
    uint32_t                          boneIndex,
    const AnimationSystem::LocalPose* poses,
    const SkeletonAsset&              skel)
{
    // Max practical bone depth is ~30 for PMX models.
    // 64 × 4 bytes = 256 bytes (fits in L1 cache, vs 4KB before).
    uint32_t chain[64];
    uint32_t depth = 0;
    int32_t  cur   = static_cast<int32_t>(boneIndex);
    while (cur >= 0 && depth < 64)
    {
        chain[depth++] = static_cast<uint32_t>(cur);
        cur = skel.parentIndex[cur];
    }

    XMMATRIX world = XMMatrixIdentity();
    for (uint32_t i = depth; i > 0; --i)
        world = LocalPoseToMatrix(poses[chain[i - 1]]) * world;

    return world;
}

// ===========================================================================
// IKSystem::SolveChain — outer loop with distance tracking (from saba::Solve)
// ===========================================================================
void IKSystem::SolveChain(
    const SkeletonAsset::IKChain& chain,
    AnimationSystem::LocalPose*   poses,
    const SkeletonAsset&          skel,
    uint32_t                      boneCount)
{
    if (chain.links.empty()) return;
    if (chain.ikBoneIndex >= boneCount || chain.targetBoneIndex >= boneCount) return;

    // Initialize per-link state
    std::vector<ChainState> states(chain.links.size());
    for (size_t li = 0; li < chain.links.size(); ++li)
    {
        auto& s = states[li];
        uint32_t bi = chain.links[li].boneIndex;
        s.animRot       = XMLoadFloat4(&poses[bi].rot);  // save original anim rotation
        s.ikRot         = XMQuaternionIdentity();
        s.saveIKRot     = XMQuaternionIdentity();
        s.prevAngle     = { 0.f, 0.f, 0.f };
        s.planeModeAngle = 0.f;

        // Reset pose to animation-only (no prior IK residue)
        XMStoreFloat4(&poses[bi].rot, s.animRot);
    }

    // The IK bone's world position is the TARGET.
    // Iterate and track convergence.
    float maxDist = std::numeric_limits<float>::max();

    for (uint32_t iter = 0; iter < chain.loopCount; ++iter)
    {
        SolveCore(iter, chain, poses, skel, states);

        // Measure distance from effector (target bone) to IK position
        XMMATRIX targetWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
        XMMATRIX ikWorld     = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
        XMVECTOR targetPos   = targetWorld.r[3];
        XMVECTOR ikPos       = ikWorld.r[3];
        float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(targetPos, ikPos)));

        if (dist < maxDist)
        {
            maxDist = dist;
            // Save best IK rotations
            for (size_t li = 0; li < chain.links.size(); ++li)
                states[li].saveIKRot = states[li].ikRot;
        }
        else
        {
            // Restore best and stop
            for (size_t li = 0; li < chain.links.size(); ++li)
            {
                uint32_t bi = chain.links[li].boneIndex;
                states[li].ikRot = states[li].saveIKRot;
                XMVECTOR finalRot = ComposeIKAnim(states[li].ikRot, states[li].animRot);
                XMStoreFloat4(&poses[bi].rot, XMQuaternionNormalize(finalRot));
            }
            break;
        }
    }
}

// ===========================================================================
// IKSystem::SolveCore — single CCD iteration (from saba::SolveCore)
// ===========================================================================
void IKSystem::SolveCore(
    uint32_t                       iteration,
    const SkeletonAsset::IKChain&  chain,
    AnimationSystem::LocalPose*    poses,
    const SkeletonAsset&           skel,
    std::vector<ChainState>&       states)
{
    // IK bone world position (the goal)
    XMMATRIX ikWorld = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
    XMVECTOR ikPos   = ikWorld.r[3];

    for (size_t li = 0; li < chain.links.size(); ++li)
    {
        const auto& link = chain.links[li];
        if (link.boneIndex >= skel.boneCount) continue;

        // Skip if chain node IS the target (would produce zero vector; from saba)
        if (link.boneIndex == chain.targetBoneIndex) continue;

        // Check for single-axis plane mode
        if (link.hasAngleLimit)
        {
            bool xActive = (link.minAngle.x != 0.f || link.maxAngle.x != 0.f);
            bool yActive = (link.minAngle.y != 0.f || link.maxAngle.y != 0.f);
            bool zActive = (link.minAngle.z != 0.f || link.maxAngle.z != 0.f);

            if (xActive && !yActive && !zActive)
            {
                SolvePlane(iteration, li, SolveAxis::X, chain, poses, skel, states);
                continue;
            }
            if (yActive && !xActive && !zActive)
            {
                SolvePlane(iteration, li, SolveAxis::Y, chain, poses, skel, states);
                continue;
            }
            if (zActive && !xActive && !yActive)
            {
                SolvePlane(iteration, li, SolveAxis::Z, chain, poses, skel, states);
                continue;
            }
        }

        // Current effector position (target bone's world position)
        XMMATRIX effWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
        XMVECTOR effPos   = effWorld.r[3];

        // Chain node's world transform & inverse
        XMMATRIX chainWorld = ComputeWorldTransform(link.boneIndex, poses, skel);
        XMMATRIX invChain   = XMMatrixInverse(nullptr, chainWorld);

        // Transform both positions into chain node's local space
        XMVECTOR chainIkPos  = XMVector3Transform(ikPos, invChain);
        XMVECTOR chainEffPos = XMVector3Transform(effPos, invChain);

        XMVECTOR chainIkVec  = XMVector3Normalize(chainIkPos);
        XMVECTOR chainEffVec = XMVector3Normalize(chainEffPos);

        float dot = XMVectorGetX(XMVector3Dot(chainEffVec, chainIkVec));
        dot = Clampf(dot, -1.f, 1.f);

        float angle = std::acosf(dot);
        if (angle < 1.0e-5f) continue;

        // Clamp per-iteration angle
        angle = Clampf(angle, -chain.angleLimit, chain.angleLimit);

        // Rotation axis (in chain node's local space)
        XMVECTOR cross = XMVector3Cross(chainEffVec, chainIkVec);
        if (XMVectorGetX(XMVector3LengthSq(cross)) < 1e-12f) continue;
        cross = XMVector3Normalize(cross);

        // Delta rotation in local space
        XMVECTOR deltaRot = XMQuaternionRotationAxis(cross, angle);

        // Combine: chainRot = ikRot * animRot * delta (saba convention)
        XMVECTOR chainRot = CombineChainRot(states[li].ikRot, states[li].animRot, deltaRot);

        // Apply angle limits
        if (link.hasAngleLimit)
        {
            // Decompose to Euler XYZ and clamp
            XMMATRIX chainRotMat = XMMatrixRotationQuaternion(chainRot);
            XMFLOAT4X4 rm;
            XMStoreFloat4x4(&rm, chainRotMat);

            XMFLOAT3 euler = DecomposeEuler(rm, states[li].prevAngle);

            XMFLOAT3 clamped;
            clamped.x = Clampf(euler.x, link.minAngle.x, link.maxAngle.x);
            clamped.y = Clampf(euler.y, link.minAngle.y, link.maxAngle.y);
            clamped.z = Clampf(euler.z, link.minAngle.z, link.maxAngle.z);

            // Per-iteration angle limit
            clamped.x = Clampf(clamped.x - states[li].prevAngle.x,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.x;
            clamped.y = Clampf(clamped.y - states[li].prevAngle.y,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.y;
            clamped.z = Clampf(clamped.z - states[li].prevAngle.z,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.z;

            states[li].prevAngle = clamped;
            chainRot = QuatFromEulerXYZ(clamped.x, clamped.y, clamped.z);
        }

        // Extract IK rotation and update pose
        states[li].ikRot = ExtractIKRot(chainRot, states[li].animRot);
        XMVECTOR finalRot = ComposeIKAnim(states[li].ikRot, states[li].animRot);
        XMStoreFloat4(&poses[link.boneIndex].rot, XMQuaternionNormalize(finalRot));
    }
}

// ===========================================================================
// IKSystem::SolvePlane — single-axis solver (from saba::SolvePlane)
// ===========================================================================
void IKSystem::SolvePlane(
    uint32_t                       iteration,
    size_t                         chainIdx,
    SolveAxis                      axis,
    const SkeletonAsset::IKChain&  chain,
    AnimationSystem::LocalPose*    poses,
    const SkeletonAsset&           skel,
    std::vector<ChainState>&       states)
{
    int axisIndex = 0;
    XMVECTOR rotateAxis = XMVectorSet(1, 0, 0, 0);
    switch (axis)
    {
    case SolveAxis::X: axisIndex = 0; rotateAxis = XMVectorSet(1, 0, 0, 0); break;
    case SolveAxis::Y: axisIndex = 1; rotateAxis = XMVectorSet(0, 1, 0, 0); break;
    case SolveAxis::Z: axisIndex = 2; rotateAxis = XMVectorSet(0, 0, 1, 0); break;
    }

    const auto& link = chain.links[chainIdx];
    auto& state = states[chainIdx];

    // IK bone world position (the goal)
    XMMATRIX ikWorld = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
    XMVECTOR ikPos   = ikWorld.r[3];

    // Effector position (target bone)
    XMMATRIX effWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
    XMVECTOR effPos   = effWorld.r[3];

    // Chain node's world transform & inverse
    XMMATRIX chainWorld = ComputeWorldTransform(link.boneIndex, poses, skel);
    XMMATRIX invChain   = XMMatrixInverse(nullptr, chainWorld);

    // Transform to local space
    XMVECTOR chainIkPos  = XMVector3Transform(ikPos, invChain);
    XMVECTOR chainEffPos = XMVector3Transform(effPos, invChain);

    XMVECTOR chainIkVec  = XMVector3Normalize(chainIkPos);
    XMVECTOR chainEffVec = XMVector3Normalize(chainEffPos);

    float dot = XMVectorGetX(XMVector3Dot(chainEffVec, chainIkVec));
    dot = Clampf(dot, -1.f, 1.f);

    float angle = std::acosf(dot);
    angle = Clampf(angle, -chain.angleLimit, chain.angleLimit);

    // Test positive and negative rotation; pick the one closer to target
    XMVECTOR rot1 = XMQuaternionRotationAxis(rotateAxis, angle);
    XMVECTOR rot2 = XMQuaternionRotationAxis(rotateAxis, -angle);
    XMVECTOR effVec1 = XMVector3Rotate(chainEffVec, rot1);
    XMVECTOR effVec2 = XMVector3Rotate(chainEffVec, rot2);
    float dot1 = XMVectorGetX(XMVector3Dot(effVec1, chainIkVec));
    float dot2 = XMVectorGetX(XMVector3Dot(effVec2, chainIkVec));

    float newAngle = state.planeModeAngle;
    if (dot1 > dot2)
        newAngle += angle;
    else
        newAngle -= angle;

    // Angle limit extraction
    float limitMin = (&link.minAngle.x)[axisIndex];
    float limitMax = (&link.maxAngle.x)[axisIndex];

    // On first iteration, try flipping sign to find valid range (from saba)
    if (iteration == 0)
    {
        if (newAngle < limitMin || newAngle > limitMax)
        {
            if (-newAngle >= limitMin && -newAngle <= limitMax)
            {
                newAngle = -newAngle;
            }
            else
            {
                float half = (limitMin + limitMax) * 0.5f;
                if (std::fabsf(half - newAngle) > std::fabsf(half + newAngle))
                    newAngle = -newAngle;
            }
        }
    }

    newAngle = Clampf(newAngle, limitMin, limitMax);
    state.planeModeAngle = newAngle;

    // IK rotation = RotateAxis(newAngle) * inv(animRot) (saba convention)
    // DXMath: ikRot = Multiply(conj(animRot), RotAxis(newAngle))
    XMVECTOR axisQuat = XMQuaternionRotationAxis(rotateAxis, newAngle);
    state.ikRot = ExtractIKRot(axisQuat, state.animRot);

    // Write combined rotation to pose
    XMVECTOR finalRot = ComposeIKAnim(state.ikRot, state.animRot);
    XMStoreFloat4(&poses[link.boneIndex].rot, XMQuaternionNormalize(finalRot));
}
