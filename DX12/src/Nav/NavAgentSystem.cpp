#include "Nav/NavAgentSystem.h"
#include "Nav/NavMeshSystem.h"
#include "Nav/NavComponents.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"           // LocalTransform, GlobalTransform
#include "ECS/CharacterControllerComponent.h"  // CCC.desiredHorizontalVelocity / wasBlockedLastStep / mode

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace Nav
{

namespace {

inline float Dist2XZ(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
{
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx*dx + dz*dz;
}

// Quaternion rotating +Z onto the unit XZ-direction of `dir`. Engine
// convention (mesh-forward = +Z) matches Hierarchy widgets and view matrices.
DirectX::XMFLOAT4 LookRotationFromDir(float dirX, float dirZ)
{
    using namespace DirectX;
    const float lenXZ = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (lenXZ < 1e-5f)
        return { 0.f, 0.f, 0.f, 1.f };
    const float yaw = std::atan2(dirX, dirZ);
    XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.f, yaw, 0.f);
    XMFLOAT4 out;
    XMStoreFloat4(&out, q);
    return out;
}

DirectX::XMFLOAT4 NLerpQuat(const DirectX::XMFLOAT4& a,
                            const DirectX::XMFLOAT4& b,
                            float t)
{
    using namespace DirectX;
    XMVECTOR va = XMLoadFloat4(&a);
    XMVECTOR vb = XMLoadFloat4(&b);
    if (XMVectorGetX(XMVector4Dot(va, vb)) < 0.0f) vb = XMVectorNegate(vb);
    XMVECTOR mixed = XMVectorAdd(XMVectorScale(va, 1.f - t), XMVectorScale(vb, t));
    mixed = XMQuaternionNormalize(mixed);
    XMFLOAT4 out;
    XMStoreFloat4(&out, mixed);
    return out;
}

// Resolve the world-space point this agent should face, given its
// facingMode + lookTarget + facingTarget. Returns false if "face nothing"
// (Manual mode, or FaceTarget without a valid lookTarget/facingTarget).
bool ResolveFacingPoint(const ::World& world,
                       const NavAgentComponent& agent,
                       DirectX::XMFLOAT3& outPoint)
{
    switch (agent.facingMode)
    {
        case NavFacingMode::Manual:
            return false;

        case NavFacingMode::FaceTarget:
            if (agent.useLookAt)
            {
                outPoint = agent.lookTarget;
                return true;
            }
            if (agent.facingTarget != NullEntity)
            {
                if (const auto* gt = world.GetComponent<GlobalTransform>(agent.facingTarget))
                {
                    using namespace DirectX;
                    const XMMATRIX m = XMLoadFloat4x4(&gt->matrix);
                    outPoint = { XMVectorGetX(m.r[3]),
                                 XMVectorGetY(m.r[3]),
                                 XMVectorGetZ(m.r[3]) };
                    return true;
                }
            }
            return false;

        case NavFacingMode::FaceMovement:
        default:
            // Steering direction is decided by the caller — return false so
            // the caller's per-frame steering vector wins.
            return false;
    }
}

} // namespace

void NavAgentTick(::World& world, NavMeshSystem& nav, float dt)
{
    if (dt <= 0.f) return;
    const bool navReady = nav.IsReady();

    world.ForEach<NavAgentComponent>([&](Entity e, NavAgentComponent& agent)
    {
        LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        if (!lt) return;

        // Motor selection — CCC is the new, KCC-aware path; absence of CCC
        // falls back to the legacy "NavAgent writes LocalTransform directly"
        // pipeline so older enemy entities (kinematic RigidBody only, no
        // CharacterVirtual) keep working. The DesignMd canonical answer is
        // CCC; the legacy path is a compatibility ramp.
        CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(e);

        // ---- Off-mesh-link handshake (CCC only; legacy path has no mode) -
        if (cc && cc->mode == MovementMode::LaunchedTraversal)
        {
            agent.state = NavAgentState::TraversingOffMeshLink;
            if (cc->launchFinished)
            {
                cc->launchFinished = false;
                if (agent.nextWaypoint < agent.path.size())
                    ++agent.nextWaypoint;
                agent.state = NavAgentState::Following;
            }
            return;
        }

        // ---- Facing helpers ---------------------------------------------
        auto ApplyFacingTowards = [&](float fx, float fz) {
            const DirectX::XMFLOAT4 want = LookRotationFromDir(fx, fz);
            const float t = std::min(1.f, agent.turnRate * dt);
            lt->rotation = NLerpQuat(lt->rotation, want, t);
        };
        auto ApplyResolvedFacing = [&]() -> bool {
            DirectX::XMFLOAT3 pt;
            if (!ResolveFacingPoint(world, agent, pt)) return false;
            const float fx = pt.x - lt->translation.x;
            const float fz = pt.z - lt->translation.z;
            const float lenSq = fx * fx + fz * fz;
            if (lenSq < 1e-6f) return true;
            const float inv = 1.f / std::sqrt(lenSq);
            ApplyFacingTowards(fx * inv, fz * inv);
            return true;
        };

        // ---- Idle / nav not ready ---------------------------------------
        const bool wantMove = agent.hasDestination && navReady;
        if (!wantMove)
        {
            if (cc) cc->desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
            // Legacy path: simply don't write translation — entity stays put.
            ApplyResolvedFacing();
            if (!agent.path.empty())
            {
                agent.path.clear();
                agent.nextWaypoint = 0;
                agent.pathValid = false;
            }
            agent.state = NavAgentState::Idle;
            return;
        }

        // ---- Repath gate -------------------------------------------------
        const bool destinationDrifted =
            Dist2XZ(agent.destination, agent.lastPlannedTarget)
            > agent.repathDistance * agent.repathDistance;
        const bool needRepath =
            agent.pathDirty ||
            !agent.pathValid ||
            agent.path.empty() ||
            agent.nextWaypoint >= agent.path.size() ||
            destinationDrifted;

        if (needRepath)
        {
            agent.state = NavAgentState::Computing;
            PathResult pr = nav.FindPath(lt->translation, agent.destination);
            agent.path              = std::move(pr.waypoints);
            agent.nextWaypoint      = 0;
            agent.pathValid         = pr.valid;
            agent.lastPlannedTarget = agent.destination;
            agent.pathDirty         = false;
            agent.stuckTimer        = 0.f;
            if (!agent.pathValid || agent.path.empty())
            {
                if (cc) cc->desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
                ApplyResolvedFacing();
                agent.state = NavAgentState::Failed;
                return;
            }
            if (agent.path.size() > 1) agent.nextWaypoint = 1;
        }

        if (agent.nextWaypoint >= agent.path.size())
        {
            if (cc) cc->desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
            ApplyResolvedFacing();
            agent.state = NavAgentState::Arrived;
            return;
        }

        agent.state = NavAgentState::Following;

        // ---- Steering geometry ------------------------------------------
        const DirectX::XMFLOAT3& wp = agent.path[agent.nextWaypoint];
        const float dx = wp.x - lt->translation.x;
        const float dy = wp.y - lt->translation.y;
        const float dz = wp.z - lt->translation.z;
        const float dist2   = dx*dx + dz*dz;
        const float arrive2 = agent.arriveRadius * agent.arriveRadius;

        if (dist2 < arrive2)
        {
            // Reached corner — advance. Halt this tick so velocity recomputes
            // toward the new waypoint next frame.
            if (cc)
            {
                cc->desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
            }
            else
            {
                // Legacy: snap to waypoint (XZ + Y) so we don't oscillate.
                lt->translation = wp;
            }
            ++agent.nextWaypoint;
            ApplyResolvedFacing();
            return;
        }

        const float dist    = std::sqrt(dist2);
        const float invDist = 1.f / dist;
        const float dirX    = dx * invDist;
        const float dirZ    = dz * invDist;

        // Arrival slowdown — only on the last corner.
        float speed = agent.speed;
        const bool lastCorner = (agent.nextWaypoint + 1 == agent.path.size());
        if (lastCorner && agent.slowdownRadius > 1e-3f && dist < agent.slowdownRadius)
            speed *= std::max(0.f, dist / agent.slowdownRadius);

        if (cc)
        {
            // ---- CCC path (KCC executor reads desiredHorizontalVelocity) -
            cc->desiredHorizontalVelocity = { dirX * speed, 0.f, dirZ * speed };

            // Stuck detection via CCC.wasBlockedLastStep — see PhysicsSystem
            // KCC step for the source signal.
            if (cc->wasBlockedLastStep)
            {
                const float align =
                    cc->blockedNormal.x * (-dirX) + cc->blockedNormal.z * (-dirZ);
                if (align > 0.5f)
                {
                    agent.stuckTimer += dt;
                    if (agent.stuckTimer > 0.5f)
                    {
                        agent.pathDirty  = true;
                        agent.stuckTimer = 0.f;
                    }
                }
            }
            else
            {
                agent.stuckTimer = std::max(0.f, agent.stuckTimer - dt);
            }
        }
        else
        {
            // ---- Legacy path (no CCC) — write LocalTransform directly ----
            // Same step-advance + Y smoothing as the old NavAgentSystem, so
            // existing enemy entities (kinematic RigidBody, no CV) continue
            // to move. No ground-snap raycast here — add a CCC if you need
            // sweep-and-slide, stairs, or single-triangle ground accuracy.
            const float step = std::min(speed * dt, dist);
            lt->translation.x += dirX * step;
            lt->translation.z += dirZ * step;
            lt->translation.y += dy * std::min(1.f, dt * 8.f);
        }

        // ---- Rotation ----------------------------------------------------
        if (!agent.rotateToFacing) return;
        if (!ApplyResolvedFacing())
        {
            if (agent.facingMode == NavFacingMode::FaceMovement)
                ApplyFacingTowards(dirX, dirZ);
        }
    });
}

} // namespace Nav
