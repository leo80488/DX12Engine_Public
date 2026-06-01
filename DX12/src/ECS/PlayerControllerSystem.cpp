#include "ECS/PlayerControllerSystem.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/ECS.h"
#include "Input/InputSystem.h"
#include "System/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // VK_SPACE / VK_LSHIFT
#include <DirectXMath.h>
#include <cmath>
#include <unordered_map>

// =============================================================================
// PlayerController diagnostic logging
// =============================================================================
// Toggle: set PLAYER_DEBUG to 0 to silence, 1 for throttled per-frame snapshot
// + edge events, 2 for every-frame snapshot. Edge events (jump fired, ground
// transition, basis fallback) always fire when PLAYER_DEBUG >= 1 regardless
// of throttle.
#ifndef PLAYER_DEBUG
#define PLAYER_DEBUG 1
#endif

namespace
{
    struct PlayerPrev {
        bool  hadBuffer = false;
        bool  wasGrounded = false;
        bool  basisOK = true;
    };
    // Per-entity previous-frame state for edge-triggered logging. Keyed by
    // Entity (uint32). Single-threaded access from PlayerControllerSystem.
    std::unordered_map<Entity, PlayerPrev> s_playerPrev;

    inline bool ShouldThrottleLog()
    {
        static int s_counter = 0;
        ++s_counter;
        return (s_counter % 30) == 0;   // ≈ twice per second @ 60Hz
    }
}

using namespace DirectX;

namespace
{
    // Extract camera forward/right projected to XZ plane (Y zeroed, then
    // re-normalised). Used as the basis for WASD: forward = +W, right = +D.
    // Returns false when the camera entity is missing or its forward is
    // degenerate (looking straight up/down). In the degenerate case the
    // caller falls back to a zero-input frame rather than guessing — input
    // resumes the moment the camera tilts back.
    bool ResolveCameraBasis(World& world, Entity cam,
                            XMVECTOR& outForward, XMVECTOR& outRight)
    {
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(cam);
        if (!gt) return false;

        // CameraSystem builds the camera's world matrix with +Z as the
        // forward axis (matches CameraSystem::BuildViewMatrix's r[2] read).
        const XMMATRIX world_m = XMLoadFloat4x4(&gt->matrix);
        XMVECTOR fwd = world_m.r[2];

        // Project onto XZ — kill the vertical component so looking down
        // doesn't shrink WASD speed and so jumping while running doesn't
        // sneakily reduce horizontal velocity.
        fwd = XMVectorSetY(fwd, 0.f);
        const float lenSq = XMVectorGetX(XMVector3LengthSq(fwd));
        if (lenSq < 1e-6f) return false;
        fwd = XMVector3Normalize(fwd);

        const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMVECTOR right = XMVector3Cross(up, fwd);     // LH coord, right = up × forward
        right = XMVector3Normalize(right);

        outForward = fwd;
        outRight   = right;
        return true;
    }
}

void PlayerControllerSystem::Update(World& world, Entity mainCamera, float dt)
{
    auto* playerPool = world.GetPool<PlayerComponent>();
    if (!playerPool) {
#if PLAYER_DEBUG >= 1
        if (ShouldThrottleLog())
            LOG_INFO("[Player] no PlayerComponent pool — system idle");
#endif
        return;
    }
    auto* ccPool = world.GetPool<CharacterControllerComponent>();
    if (!ccPool) {
#if PLAYER_DEBUG >= 1
        if (ShouldThrottleLog())
            LOG_INFO("[Player] no CharacterControllerComponent pool — system idle");
#endif
        return;
    }

    const Input& input = Input::Get();
    const bool   sprint = input.IsKeyDown(VK_LSHIFT);
    // Read WASD as raw +1/-1 axis values; (D-A, W-S) → world-space XZ after
    // we multiply by the camera basis.
    const float  ix = input.Axis('D', 'A');
    const float  iz = input.Axis('W', 'S');
    const bool   jumpEdge = input.WasKeyPressed(VK_SPACE);

    const auto& entities = playerPool->Entities();
    auto&       players  = playerPool->Data();
    const size_t n = players.size();

#if PLAYER_DEBUG >= 1
    const bool throttleFire = ShouldThrottleLog();
    if (throttleFire)
    {
        LOG_INFO("[Player] frame snapshot: nPlayers=%zu input(ix=%.1f iz=%.1f sprint=%d jumpEdge=%d) mainCamHint=%u",
                 n, ix, iz, sprint ? 1 : 0, jumpEdge ? 1 : 0,
                 static_cast<uint32_t>(mainCamera));
    }
#endif

    for (size_t i = 0; i < n; ++i)
    {
        PlayerComponent& pc = players[i];
        const Entity e = entities[i];

        CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(e);
        if (!cc) {
#if PLAYER_DEBUG >= 1
            if (throttleFire)
                LOG_WARNING("[Player ent=%u] has PlayerComponent but NO CharacterControllerComponent — input ignored", e);
#endif
            continue;
        }

        // Camera basis. Falls back to world +Z forward / +X right when the
        // resolve fails (camera missing / looking straight down). Picking
        // *some* basis is preferable to freezing input — the player can at
        // least move while the camera reorients.
        // Resolve the optional explicit camera ref; fall back to App-supplied
        // mainCamera when the player has no override or its GUID can't be
        // resolved (target destroyed, never loaded, etc.).
        const Entity resolvedCam = pc.cameraEntity.Resolve(world);
        const Entity camE = (resolvedCam != NullEntity) ? resolvedCam : mainCamera;
        XMVECTOR fwd, right;
        const bool basisOK = ResolveCameraBasis(world, camE, fwd, right);
        if (!basisOK)
        {
            fwd   = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            right = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        }
#if PLAYER_DEBUG >= 1
        // Edge: basis state changed (e.g. camera entity disappeared mid-scene).
        auto& prev = s_playerPrev[e];
        if (prev.basisOK != basisOK)
        {
            LOG_INFO("[Player ent=%u] camera basis %s (camEntity=%u)",
                     e, basisOK ? "RESOLVED" : "FALLBACK (camera missing or looking ±Y)",
                     static_cast<uint32_t>(camE));
            prev.basisOK = basisOK;
        }
#endif

        // Compose horizontal direction. Don't normalise here — diagonal
        // (1,1) gives √2 length which the CCC then multiplies by speed.
        // Normalise so diagonal isn't faster than cardinal.
        XMVECTOR dir = XMVectorAdd(XMVectorScale(right, ix), XMVectorScale(fwd, iz));
        const float dirLenSq = XMVectorGetX(XMVector3LengthSq(dir));
        if (dirLenSq > 1e-6f)
            dir = XMVector3Normalize(dir);
        else
            dir = XMVectorZero();

        // Air control: while airborne, scale the input contribution by
        // pc.airControl. We DON'T fade in the previous-frame velocity here
        // — that lives implicitly in the CCC's vertical state and the
        // ground physics, and adding momentum carry on top of CCC's
        // horizontal-overwrite design tends to make jumps feel slippery.
        // Designers wanting "drift" can lower airControl past 0.4.
        const float controlScale = cc->isGrounded ? 1.f : pc.airControl;
        const float speed        = (sprint ? pc.runSpeed : pc.walkSpeed) * controlScale;
        const XMVECTOR vel       = XMVectorScale(dir, speed);

        XMStoreFloat3(&cc->desiredHorizontalVelocity, vel);
        // Y component is irrelevant to the CCC (it overwrites Y from
        // gravity/jump), but zero it out anyway so callers that snapshot
        // the struct elsewhere see a clean XZ vector.
        cc->desiredHorizontalVelocity.y = 0.f;

#if PLAYER_DEBUG >= 1
        if (throttleFire || PLAYER_DEBUG >= 2)
        {
            XMFLOAT3 fwdF, rightF;
            XMStoreFloat3(&fwdF,   fwd);
            XMStoreFloat3(&rightF, right);
            LOG_INFO("[Player ent=%u] camFwd=(%.2f,%.2f,%.2f) camRight=(%.2f,%.2f,%.2f) "
                     "speed=%.2f isGrounded=%d controlScale=%.2f → desiredVel=(%.2f,%.2f,%.2f)",
                     e,
                     fwdF.x, fwdF.y, fwdF.z,
                     rightF.x, rightF.y, rightF.z,
                     speed, cc->isGrounded ? 1 : 0, controlScale,
                     cc->desiredHorizontalVelocity.x,
                     cc->desiredHorizontalVelocity.y,
                     cc->desiredHorizontalVelocity.z);
        }
#endif

        // ---- Face movement direction -------------------------------------
        // Rotate the player's LocalTransform.rotation toward the movement
        // heading. PhysicsSystem only writes back translation for CCCs, so
        // any rotation we set here survives the physics step. Skip below
        // facingMinSpeed so an idle player stays facing wherever they last
        // moved instead of snapping to a stale heading. atan2(velX, velZ)
        // matches CameraSystem's yaw convention (yaw rotates +Z forward
        // toward +X by sin/cos).
        if (pc.turnRate > 0.f)
        {
            const float horizSpeed = std::sqrt(cc->desiredHorizontalVelocity.x * cc->desiredHorizontalVelocity.x
                                             + cc->desiredHorizontalVelocity.z * cc->desiredHorizontalVelocity.z);
            if (horizSpeed >= pc.facingMinSpeed)
            {
                LocalTransform* lt = world.GetComponent<LocalTransform>(e);
                if (lt)
                {
                    const float targetYaw = std::atan2(cc->desiredHorizontalVelocity.x,
                                                       cc->desiredHorizontalVelocity.z);
                    // Recover current yaw from quaternion. Assumes pure
                    // Y-axis rotation (no pitch/roll on the player — that's
                    // animation's job, not the controller's). Rotating +Z
                    // by the current quat then atan2-ing the result is
                    // robust even when the quat was built elsewhere.
                    const XMVECTOR currQ   = XMLoadFloat4(&lt->rotation);
                    const XMVECTOR currFwd = XMVector3Rotate(XMVectorSet(0,0,1,0), currQ);
                    XMFLOAT3 cf; XMStoreFloat3(&cf, currFwd);
                    const float currYaw = std::atan2(cf.x, cf.z);

                    // Shortest-arc delta in (-π, +π].
                    constexpr float kPi  = 3.14159265358979323846f;
                    constexpr float kTau = 2.f * kPi;
                    float dyaw = targetYaw - currYaw;
                    while (dyaw >  kPi) dyaw -= kTau;
                    while (dyaw < -kPi) dyaw += kTau;

                    const float maxStep = pc.turnRate * dt;
                    if (dyaw >  maxStep) dyaw =  maxStep;
                    if (dyaw < -maxStep) dyaw = -maxStep;

                    const float newYaw = currYaw + dyaw;
                    const XMVECTOR newQ = XMQuaternionRotationAxis(
                        XMVectorSet(0.f, 1.f, 0.f, 0.f), newYaw);
                    XMStoreFloat4(&lt->rotation, newQ);
                }
            }
        }

        // ---- Jump pipeline -----------------------------------------------
        //   1. Decay any existing buffer timer.
        //   2. New press refreshes the buffer to its full window.
        //   3. Fire if buffer is active AND we're grounded OR still inside
        //      the coyote window. Firing clears the buffer.
        if (pc.jumpBufferTimer > 0.f)
            pc.jumpBufferTimer = std::max(0.f, pc.jumpBufferTimer - dt);
        if (jumpEdge)
        {
            pc.jumpBufferTimer = pc.jumpBufferTime;
#if PLAYER_DEBUG >= 1
            LOG_INFO("[Player ent=%u] jump pressed (buffer→%.2fs)", e, pc.jumpBufferTime);
#endif
        }

        const bool canJump =
            cc->isGrounded ||
            (cc->timeInAir <= pc.coyoteTime && cc->velocity.y <= 0.f);
        if (pc.jumpBufferTimer > 0.f && canJump)
        {
            cc->jumpRequested  = true;
            pc.jumpBufferTimer = 0.f;
#if PLAYER_DEBUG >= 1
            LOG_INFO("[Player ent=%u] JUMP fired (grounded=%d timeInAir=%.2f coyoteTime=%.2f vy=%.2f)",
                     e, cc->isGrounded ? 1 : 0, cc->timeInAir, pc.coyoteTime, cc->velocity.y);
#endif
        }

#if PLAYER_DEBUG >= 1
        // Edge: ground transitions (useful for debugging stuck-in-air / never-grounded states).
        if (prev.wasGrounded != cc->isGrounded)
        {
            LOG_INFO("[Player ent=%u] ground %s (timeInAir=%.2f vel=(%.2f,%.2f,%.2f))",
                     e, cc->isGrounded ? "LANDED" : "AIRBORNE",
                     cc->timeInAir,
                     cc->velocity.x, cc->velocity.y, cc->velocity.z);
            prev.wasGrounded = cc->isGrounded;
        }
#endif
    }
}
