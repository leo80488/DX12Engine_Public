#pragma once

// PhysicsSystem — ECS bridge for Jolt Physics.
//
// Runs a fixed 60Hz simulation via an accumulator inside Update(). Creates
// Jolt bodies lazily for entities that have both RigidBodyComponent and
// ColliderComponent. After each simulation step, writes dynamic/kinematic
// body transforms back into LocalTransform so TransformSystem::Propagate
// sees fresh data.
//
// Ticking order inside Scene::Update() must be:
//   1) scripts / game logic
//   2) PhysicsSystem::Update(world, dt)
//   3) TransformSystem::Propagate(world)
//
// Jolt types are fully hidden via pImpl so this header stays cheap to include.

#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include "ECS/ECS.h"   // Entity for RayHit.entity

class World;

namespace DX12Physics
{
    // Result of a single-hit ray query. `hit` is the only field guaranteed
    // to be meaningful when the call returns; everything else is zero-initialised
    // on a miss so callers can use the struct unconditionally.
    struct RayHit
    {
        bool              hit      = false;
        DirectX::XMFLOAT3 point    = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 normal   = { 0.f, 1.f, 0.f };
        float             distance = 0.f;
        Entity            entity   = NullEntity;
    };

    // Result of a single-hit shape sweep (Capsule / Sphere / Box). Mirrors
    // RayHit but adds `fraction` ([0,1] along the sweep) and `bodyId` so
    // callers can filter out specific bodies on subsequent casts (e.g. the
    // character's own body during collide-and-slide).
    struct ShapeCastHit
    {
        bool              hit      = false;
        DirectX::XMFLOAT3 point    = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 normal   = { 0.f, 1.f, 0.f };
        float             distance = 0.f;
        float             fraction = 0.f;
        Entity            entity   = NullEntity;
        std::uint32_t     bodyId   = 0xffffffffu;
    };

    // Object-layer enum exposed publicly so KCC/queries can pick the layer
    // they care about (e.g. a player-ground probe restricts itself to
    // STATIC_ENV + DYNAMIC_PROP and excludes HURT_BOX/ATTACK_BOX/SENSING).
    // Values must match the internal Jolt ObjectLayer constants in
    // PhysicsSystem.cpp. Bitmask helpers compose layers for filter args.
    enum class Layer : std::uint8_t
    {
        StaticEnv   = 0,
        DynamicProp = 1,
        Character   = 2,
        Ragdoll     = 3,
        HurtBox     = 4,
        AttackBox   = 5,
        Projectile  = 6,
        Sensing     = 7,
    };

    // Layer-mask: bit N set means "include layer N in this query". Default
    // mask LAYER_MASK_DEFAULT covers physical collisions (Static + Dynamic +
    // Character + Ragdoll + Projectile), excluding query-only layers.
    using LayerMask = std::uint16_t;
    inline constexpr LayerMask MakeLayerMask(Layer l) { return LayerMask(1u << static_cast<std::uint8_t>(l)); }
    inline constexpr LayerMask LAYER_MASK_ALL     = LayerMask(0xFFFFu);
    inline constexpr LayerMask LAYER_MASK_DEFAULT =
        LayerMask((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6));   // StaticEnv | DynamicProp | Character | Ragdoll | Projectile

    class PhysicsSystem
    {
    public:
        PhysicsSystem();
        ~PhysicsSystem();

        PhysicsSystem(const PhysicsSystem&)            = delete;
        PhysicsSystem& operator=(const PhysicsSystem&) = delete;

        // Init: registers Jolt factory/types, spins up job pool, creates the
        // JPH::PhysicsSystem. Safe to call once per process (scenes share the
        // Jolt factory state).
        void Init();
        void Shutdown();

        // Legacy single-call entry point: drives an INTERNAL 60Hz accumulator
        // and runs PreAllSteps → N × StepOnce → PostAllSteps inline. Useful
        // for scripts/tests that don't want to wire up the external loop.
        // The Phase-scheduler engine loop (App::Run) uses the split entry
        // points below so FixedPhysicsPre/Post phases can run between substeps.
        void Update(World& world, float dt);

        // ---- Externalised accumulator entry points ----------------------------
        // The scheduler-driven path calls these as:
        //   PreAllSteps(world);
        //   accumulator += dt;
        //   while (accumulator >= GetFixedDt()) { StepOnce(world); accumulator -= GetFixedDt(); }
        //   PostAllSteps(world);
        // Splitting lets the scheduler insert FixedPhysicsPre/Post phase work
        // around every substep (per design §1).

        // Auto-prewarm + collider-swap rebuild + body creation + teleport
        // detection. Runs ONCE per render frame regardless of substep count.
        void PreAllSteps(World& world);

        // One fixed-dt physics substep: prev/curr pose snapshot, KCC tick,
        // Jolt step, post-step snapshot. Runs N times per render frame
        // driven by the caller's accumulator.
        void StepOnce(World& world);

        // Drain contact events to EventBus + write Jolt's instantaneous pose
        // back into LocalTransform. Runs ONCE per render frame after every
        // substep is done.
        void PostAllSteps(World& world);

        // Fixed-physics timestep (60Hz).
        static constexpr float GetFixedDt() noexcept { return 1.0f / 60.0f; }

        // Render-only interpolation. Must run AFTER TransformSystem::Propagate
        // and BEFORE rendering. For each Dynamic body, overwrites the entity's
        // GlobalTransform.matrix with a pose lerped/slerped between the body's
        // pre-step and post-step Jolt snapshots. `renderAlpha` is the caller's
        // accumulator / fixedDt clamped to [0,1] — the scheduler populates
        // FrameContext::physicsAlpha for this.
        // LocalTransform stays at Jolt's instantaneous pose so the physics
        // feedback loop (Phase 2 teleport detection) doesn't break.
        void ApplyRenderInterpolation(World& world, float renderAlpha);

        // Drop every cached trimesh shape (used by Mesh-shape colliders that
        // re-read a .meshlib / .imsh file at body creation) AND bump the
        // generation counter on every ColliderComponent whose shape is Mesh,
        // so the runtime-swap detector in Update() destroys + rebuilds those
        // bodies on the next physics tick with shapes built from the fresh
        // .meshlib contents. Call after re-baking a collision .meshlib —
        // without the per-entity bump, existing bodies keep their old
        // (now-discarded) JPH::ShapeRefC indefinitely.
        void InvalidateMeshShapeCache(World& world);

        // Eagerly build (and cache) MeshShapes for every (path, meshId) in
        // the list, in parallel via TaskSystem. Without this, body creation
        // in Phase 1 builds shapes serially on the main thread on the first
        // Play tick — for 1k+ entities that's seconds of UI freeze and
        // gigabytes of redundant blob memcpy because each call re-reads the
        // file. This pre-warm reads each .meshlib once and parallelises the
        // Jolt MeshShape construction across workers. Blocks until done.
        struct MeshShapeRequest { std::string path; uint32_t meshId; };
        void PrewarmMeshShapes(const std::vector<MeshShapeRequest>& requests);

        // Single closest-hit raycast against every registered body (Static,
        // Kinematic, Dynamic — Kinematic bodies are pair-detected even though
        // they don't resolve contacts, which is exactly what we want for
        // ground/wall probes). `direction` does NOT need to be normalised;
        // the hit point is computed at `from + (direction/|direction|) *
        // hit.distance` with `hit.distance` in [0, maxDistance]. Returns a
        // zero-initialised RayHit (hit=false) when nothing was hit or the
        // physics system isn't ready.
        RayHit CastRayClosest(const DirectX::XMFLOAT3& from,
                              const DirectX::XMFLOAT3& direction,
                              float                    maxDistance) const;

        // Single closest-hit capsule sweep. The capsule's central segment has
        // half-height @p halfHeight (excluding the two hemispheres) and
        // radius @p radius. @p direction does NOT need to be normalised. Set
        // @p ignoreBodyId to skip one body (e.g. the character's own body
        // during ground probing or collide-and-slide). @p layerMask selects
        // which object layers participate — default excludes HurtBox /
        // AttackBox / Sensing so a movement probe doesn't catch hitboxes.
        ShapeCastHit CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                        const DirectX::XMFLOAT3& direction,
                                        float                    radius,
                                        float                    halfHeight,
                                        float                    maxDistance,
                                        std::uint32_t            ignoreBodyId = 0xffffffffu,
                                        LayerMask                layerMask    = LAYER_MASK_DEFAULT) const;

        // Single closest-hit sphere sweep — convenience wrapper around the
        // capsule cast with halfHeight = 0. Cheaper than a capsule of equal
        // radius and sometimes more appropriate (e.g. ledge-detection probe).
        ShapeCastHit CastSphereClosest(const DirectX::XMFLOAT3& from,
                                       const DirectX::XMFLOAT3& direction,
                                       float                    radius,
                                       float                    maxDistance,
                                       std::uint32_t            ignoreBodyId = 0xffffffffu,
                                       LayerMask                layerMask    = LAYER_MASK_DEFAULT) const;

        // Lookup / on-demand load the raw .meshlib bytes for a collider
        // source path. Populates the same blob cache that GetOrBuildMeshShape
        // uses, so the bytes are shared across body creation, debug viz, and
        // any other CPU-side reader. Returns nullptr only when the file is
        // missing on disk and not in any pak. The returned pointer is stable
        // until InvalidateMeshShapeCache (or PhysicsSystem destruction).
        const std::vector<std::uint8_t>* EnsureCollisionBlob(const std::string& path);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
