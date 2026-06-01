// PhysicsSystem implementation. All Jolt-specific types are confined to this
// translation unit; the public header exposes only a pImpl handle.
//
// Object layers follow the gameplay split outlined in DesignMd/Collision_-
// Architecture_Reference.md §6.4: STATIC_ENV / DYNAMIC_PROP / CHARACTER /
// RAGDOLL / HURT_BOX / ATTACK_BOX / PROJECTILE / SENSING. Object layers are
// folded into two broadphase layers (STATIC and MOVING) so the broadphase
// tree stays cheap — fine-grained "should-this-pair-collide" is resolved in
// the ObjectLayerPairFilter at narrow-phase time.

#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsEvents.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/CharacterControllerComponent.h"
#include "Resource/AssetFS.h"      // mesh-collider source decode (read .meshlib / .imsh blobs)
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include "System/EventBus.h"
#include "System/TaskSystem.h"     // parallel mesh-shape prewarm

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>   // ClosestHitCollisionCollector<>: shape-cast closest-hit template
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Physics
{
// ---------------------------------------------------------------------------
// Layer definitions
// ---------------------------------------------------------------------------
namespace Layers
{
    // Eight gameplay-aware object layers. Values MUST match the public
    // DX12Physics::Layer enum in PhysicsSystem.h so caller-supplied LayerMask
    // bits line up with the internal layer indices.
    static constexpr JPH::ObjectLayer STATIC_ENV   = 0;  // walls, floors, terrain — never moves
    static constexpr JPH::ObjectLayer DYNAMIC_PROP = 1;  // boxes, debris, vehicles — full physics
    static constexpr JPH::ObjectLayer CHARACTER    = 2;  // player + NPC movement capsule
    static constexpr JPH::ObjectLayer RAGDOLL      = 3;  // death ragdoll per-bone bodies
    static constexpr JPH::ObjectLayer HURT_BOX     = 4;  // per-bone hit-detection (query only)
    static constexpr JPH::ObjectLayer ATTACK_BOX   = 5;  // weapon swing volume (query only)
    static constexpr JPH::ObjectLayer PROJECTILE   = 6;  // bullets / arrows / thrown objects
    static constexpr JPH::ObjectLayer SENSING      = 7;  // AI vision/hearing volume (query only)
    static constexpr JPH::ObjectLayer NUM_LAYERS   = 8;
}

namespace BPLayers
{
    // Only two BP layers — every layer lives in MOVING except STATIC_ENV.
    // Splitting more buckets at the BP level rarely helps for typical scenes
    // because narrow-phase already rejects unwanted pairs via the pair filter.
    static constexpr JPH::BroadPhaseLayer STATIC{ 0 };
    static constexpr JPH::BroadPhaseLayer MOVING{ 1 };
    static constexpr JPH::uint            NUM_LAYERS = 2;
}

// Symmetric pair-collision matrix. `kCollidesWith[L]` is a bitmask of layers
// that L is allowed to interact with. Keep symmetric (A in CollidesWith[B] ↔
// B in CollidesWith[A]) — Jolt may call ShouldCollide in either order and the
// query-side LayerMaskFilter relies on the same table.
static constexpr std::uint16_t LayerBit(JPH::ObjectLayer l) { return std::uint16_t(1u << l); }
static constexpr std::uint16_t kCollidesWith[Layers::NUM_LAYERS] = {
    /* STATIC_ENV   */ std::uint16_t(LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::PROJECTILE)),
    /* DYNAMIC_PROP */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::PROJECTILE)),
    /* CHARACTER    */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::PROJECTILE) | LayerBit(Layers::SENSING)),
    /* RAGDOLL      */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::RAGDOLL)   | LayerBit(Layers::PROJECTILE)),
    /* HURT_BOX     */ std::uint16_t(LayerBit(Layers::ATTACK_BOX)   | LayerBit(Layers::PROJECTILE)),
    /* ATTACK_BOX   */ std::uint16_t(LayerBit(Layers::HURT_BOX)),
    /* PROJECTILE   */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::HURT_BOX)),
    /* SENSING      */ std::uint16_t(LayerBit(Layers::CHARACTER)),
};

static constexpr JPH::BroadPhaseLayer ObjectLayerToBP(JPH::ObjectLayer l)
{
    return (l == Layers::STATIC_ENV) ? BPLayers::STATIC : BPLayers::MOVING;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        for (JPH::ObjectLayer l = 0; l < Layers::NUM_LAYERS; ++l)
            m_map[l] = ObjectLayerToBP(l);
    }
    JPH::uint            GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_map[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override
    {
        return (l == BPLayers::STATIC) ? "STATIC" : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_map[Layers::NUM_LAYERS];
};

class ObjectVsBPFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override
    {
        // For each object layer A, is there ANY layer in BP-bucket B that A
        // collides with? Pre-compute by OR-ing kCollidesWith bits whose
        // owning BP layer is B. Hot path — keep branch-free.
        if (a >= Layers::NUM_LAYERS) return false;
        const std::uint16_t mask = kCollidesWith[a];
        if (b == BPLayers::STATIC)
            return (mask & LayerBit(Layers::STATIC_ENV)) != 0;
        // BPLayers::MOVING
        const std::uint16_t movingBits = std::uint16_t(0xFFFFu & ~LayerBit(Layers::STATIC_ENV));
        return (mask & movingBits) != 0;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a >= Layers::NUM_LAYERS || b >= Layers::NUM_LAYERS) return false;
        return (kCollidesWith[a] & LayerBit(b)) != 0;
    }
};

// Query-side layer filter: include any layer whose bit is set in the public
// LayerMask. Used by CastCapsule / CastSphere so callers can carve out
// gameplay-only subsets (e.g. exclude HURT_BOX from a movement probe).
class LayerMaskFilter final : public JPH::ObjectLayerFilter
{
public:
    explicit LayerMaskFilter(std::uint16_t mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override
    {
        if (l >= Layers::NUM_LAYERS) return false;
        return (m_mask & LayerBit(l)) != 0;
    }
private:
    std::uint16_t m_mask;
};

// Body filter that excludes a single BodyID — used to skip the character's
// own body during ground probes / collide-and-slide so the cast doesn't
// instantly hit itself.
class IgnoreBodyFilter final : public JPH::BodyFilter
{
public:
    explicit IgnoreBodyFilter(JPH::BodyID ignore) : m_ignore(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        return !m_ignore.IsInvalid() ? (id != m_ignore) : true;
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return !m_ignore.IsInvalid() ? (body.GetID() != m_ignore) : true;
    }
private:
    JPH::BodyID m_ignore;
};

// ---------------------------------------------------------------------------
// Jolt trace / assert sinks — route Jolt's diagnostic output into our Logger.
// ---------------------------------------------------------------------------
static void JoltTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG_INFO("[Jolt] %s", buf);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailed(const char* expr, const char* msg, const char* file, JPH::uint line)
{
    LOG_ERROR("[Jolt assert] %s:%u: (%s) %s", file, (unsigned)line, expr, msg ? msg : "");
    return true; // trigger breakpoint
}
#endif

// ---------------------------------------------------------------------------
// Contact listener — buffers contacts on whatever worker thread Jolt calls
// us on, and drains on the main thread after the simulation step. The
// engine's EventBus is single-threaded by contract, so we can't Publish
// directly from the callback.
// ---------------------------------------------------------------------------
class PhysicsContactListener : public JPH::ContactListener
{
public:
    struct Buffered
    {
        Entity            a;
        Entity            b;
        DirectX::XMFLOAT3 point;
        DirectX::XMFLOAT3 normal;
    };

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*ioSettings*/) override
    {
        Buffered c;
        // User data is populated at body-creation time (Phase 1 below) with
        // the owning Entity; if that slot is 0 (default) the entity is
        // either editor-local or already dead — skip the contact.
        c.a = static_cast<Entity>(body1.GetUserData());
        c.b = static_cast<Entity>(body2.GetUserData());
        if (c.a == NullEntity || c.b == NullEntity) return;

        const JPH::Vec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        c.point  = { p.GetX(), p.GetY(), p.GetZ() };
        c.normal = { n.GetX(), n.GetY(), n.GetZ() };

        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending.push_back(c);
    }

    // Main-thread drain. Swap semantics so per-frame Drain costs one atomic
    // vector swap plus the processing cost on the caller side.
    void Drain(std::vector<Buffered>& out)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        out.swap(m_pending);
    }

private:
    std::mutex            m_mtx;
    std::vector<Buffered> m_pending;
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct PhysicsSystem::Impl
{
    static constexpr float        kFixedDt        = 1.0f / 60.0f;
    static constexpr JPH::uint    kMaxBodies      = 65536;
    static constexpr JPH::uint    kNumBodyMutexes = 0;  // 0 = Jolt picks a default
    static constexpr JPH::uint    kMaxBodyPairs   = 65536;
    static constexpr JPH::uint    kMaxContacts    = 10240;
    static constexpr int          kCollisionSteps = 1;

    std::unique_ptr<JPH::TempAllocatorImpl>     tempAlloc;
    std::unique_ptr<JPH::JobSystemThreadPool>   jobSys;
    std::unique_ptr<BPLayerInterfaceImpl>       bpLayerIface;
    std::unique_ptr<ObjectVsBPFilterImpl>       objVsBpFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl>  objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem>         physics;
    std::unique_ptr<PhysicsContactListener>     contactListener;

    float                                       accumulator = 0.0f;
    // Scratch buffer reused each frame so Drain doesn't allocate.
    std::vector<PhysicsContactListener::Buffered> contactScratch;

    // Cache for trimesh colliders: (meshLibPath + '#' + meshId) → built
    // JPH::MeshShape. Many entities share the same baked entry, so building
    // the shape once and reusing avoids massive duplicate Jolt geometry.
    std::unordered_map<std::string, JPH::ShapeRefC> meshShapeCache;

    // Source-file cache: meshLibPath → raw file bytes. One .meshlib typically
    // contains N entries shared by N entities; without this cache the lazy
    // body-creation path in Phase 1 re-reads the same 18 MB blob N times
    // (28 GB of memcpy for Bistro-scale scenes loaded from .iscene). The
    // shape cache above keys on (path, meshId) so it can't dedup the file
    // read by itself.
    std::unordered_map<std::string, std::vector<uint8_t>> blobCache;

    // Pose snapshots for render-side interpolation. With physics @60Hz and
    // render @180Hz, only ~1 in 3 frames actually runs a physics step — the
    // other 2 see the same Jolt pose and the object visually stutters. We
    // store the body pose at the START and END of the most recent step so
    // Phase 4 can lerp/slerp between them using accumulator/fixedDt.
    // Tracked per entity for Dynamic bodies only.
    struct PoseSnapshot
    {
        JPH::Vec3 prevPos = JPH::Vec3::sZero();
        JPH::Quat prevRot = JPH::Quat::sIdentity();
        JPH::Vec3 currPos = JPH::Vec3::sZero();
        JPH::Quat currRot = JPH::Quat::sIdentity();
    };
    std::unordered_map<Entity, PoseSnapshot> poseSnapshots;

    // Snapshot of each entity's ColliderComponent at the moment its Jolt body
    // was last built. Phase 0.6 compares against the live ColliderComponent;
    // a mismatch (shape enum, dimensions, mesh path/id) triggers destroy +
    // rebuild. Catches direct field writes from the Inspector reflection
    // layer without requiring callers to remember MarkDirty(). Cleared
    // alongside the body in DestroyEntityBody.
    std::unordered_map<Entity, ColliderComponent> colliderSnapshots;

    // ---- Kinematic Character Controller (KCC) ----
    // One JPH::CharacterVirtual per entity that owns a
    // CharacterControllerComponent. The Ref keeps the CV alive; dropping the
    // entry destroys it (CV does not register with the broadphase by itself,
    // so no separate body removal step is needed). Lifecycle mirrors
    // RigidBody: lazy create on first sight, rebuild on capsule-shape change,
    // tear down when the component disappears.
    std::unordered_map<Entity, JPH::Ref<JPH::CharacterVirtual>> kccTable;

    // Per-CV capsule descriptor we built it from. Used to detect runtime
    // capsule-resize edits and trigger a rebuild — CharacterVirtual stores
    // its shape internally and there's no cheap "swap shape" API.
    struct KccShapeSnap { float radius; float halfHeight; };
    std::unordered_map<Entity, KccShapeSnap> kccShapeSnaps;

    // ExtendedUpdateSettings is reusable across all KCCs because the
    // step-offset / snap-to-ground tuning depends on the *capsule* not on
    // the character — and any per-character override can be folded into the
    // CCC fields if needed. Built once in Init().
    JPH::CharacterVirtual::ExtendedUpdateSettings kccExtUpdate;
};

// Translate the inverted "locked" bitmask we expose to authors into Jolt's
// "allowed" enum. RigidBodyComponent::AxisLock bit ON  → corresponding
// EAllowedDOFs bit OFF. With lockedAxes == 0 the result is EAllowedDOFs::All
// (full 6-DOF), matching Jolt's default — important so non-touching bodies
// keep their original mass-properties layout.
static JPH::EAllowedDOFs ToJoltAllowedDOFs(std::uint8_t lockedAxes)
{
    using JPH::EAllowedDOFs;
    std::uint8_t dof = static_cast<std::uint8_t>(EAllowedDOFs::All);
    if (lockedAxes & RigidBodyComponent::LockTranslationX) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationX);
    if (lockedAxes & RigidBodyComponent::LockTranslationY) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationY);
    if (lockedAxes & RigidBodyComponent::LockTranslationZ) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationZ);
    if (lockedAxes & RigidBodyComponent::LockRotationX)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationX);
    if (lockedAxes & RigidBodyComponent::LockRotationY)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationY);
    if (lockedAxes & RigidBodyComponent::LockRotationZ)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationZ);
    return static_cast<EAllowedDOFs>(dof);
}

// Compare the shape-affecting fields of two ColliderComponents. Excludes the
// `generation` and any other runtime-only state — only the fields that feed
// into MakeShape().
static bool ColliderShapeEqual(const ColliderComponent& a, const ColliderComponent& b)
{
    if (a.shape != b.shape) return false;
    if (a.offset.x != b.offset.x
     || a.offset.y != b.offset.y
     || a.offset.z != b.offset.z) return false;
    if (a.rotationEulerDeg.x != b.rotationEulerDeg.x
     || a.rotationEulerDeg.y != b.rotationEulerDeg.y
     || a.rotationEulerDeg.z != b.rotationEulerDeg.z) return false;
    switch (a.shape)
    {
        case ColliderComponent::Shape::Box:
            return a.halfExtents.x == b.halfExtents.x
                && a.halfExtents.y == b.halfExtents.y
                && a.halfExtents.z == b.halfExtents.z;
        case ColliderComponent::Shape::Sphere:
            return a.radius == b.radius;
        case ColliderComponent::Shape::Capsule:
            return a.radius == b.radius && a.halfHeight == b.halfHeight;
        case ColliderComponent::Shape::Mesh:
            return a.meshLibPath   == b.meshLibPath
                && a.meshLibMeshId == b.meshLibMeshId;
    }
    return true;
}

PhysicsSystem::PhysicsSystem()  = default;
PhysicsSystem::~PhysicsSystem() { Shutdown(); }

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void PhysicsSystem::Init()
{
    if (m_impl) return;

    JPH::RegisterDefaultAllocator();

    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_impl = std::make_unique<Impl>();

    m_impl->tempAlloc = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);

    const int workerCount = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    m_impl->jobSys = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerCount);

    m_impl->bpLayerIface       = std::make_unique<BPLayerInterfaceImpl>();
    m_impl->objVsBpFilter      = std::make_unique<ObjectVsBPFilterImpl>();
    m_impl->objLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();

    m_impl->physics = std::make_unique<JPH::PhysicsSystem>();
    m_impl->physics->Init(
        Impl::kMaxBodies, Impl::kNumBodyMutexes,
        Impl::kMaxBodyPairs, Impl::kMaxContacts,
        *m_impl->bpLayerIface, *m_impl->objVsBpFilter, *m_impl->objLayerPairFilter);

    m_impl->contactListener = std::make_unique<PhysicsContactListener>();
    m_impl->physics->SetContactListener(m_impl->contactListener.get());

    // KCC ExtendedUpdateSettings defaults: keep stairs walking enabled with
    // small forward/up offsets (matches Jolt's CharacterVirtualTest), and
    // turn on stick-to-floor so downhill walks don't visibly micro-bounce.
    // Step values stay constant; per-character step *height* is honoured by
    // ExtendedUpdate via the StickToFloor / WalkStairs internal logic that
    // reads CharacterVirtual::mMaxStrength etc. — not the inSettings struct.
    m_impl->kccExtUpdate.mStickToFloorStepDown   = JPH::Vec3(0, -0.5f, 0);
    m_impl->kccExtUpdate.mWalkStairsStepUp       = JPH::Vec3(0, 0.4f, 0);
    m_impl->kccExtUpdate.mWalkStairsMinStepForward = 0.02f;
    m_impl->kccExtUpdate.mWalkStairsStepForwardTest = 0.15f;

    LOG_INFO("PhysicsSystem: initialized (Jolt %d.%d.%d, %d worker threads)",
             JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH, workerCount);
}

const std::vector<std::uint8_t>* PhysicsSystem::EnsureCollisionBlob(const std::string& path)
{
    if (!m_impl || path.empty()) return nullptr;
    auto it = m_impl->blobCache.find(path);
    if (it != m_impl->blobCache.end()) return &it->second;

    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
    {
        LOG_WARNING("PhysicsSystem::EnsureCollisionBlob — cannot read '%s'", path.c_str());
        return nullptr;
    }
    auto [ins, _] = m_impl->blobCache.emplace(path, std::move(blob));
    return &ins->second;
}

RayHit PhysicsSystem::CastRayClosest(const DirectX::XMFLOAT3& from,
                                     const DirectX::XMFLOAT3& direction,
                                     float                    maxDistance) const
{
    RayHit out;
    if (!m_impl || maxDistance <= 0.f) return out;

    // Jolt's RRayCast direction encodes BOTH heading and length — the hit
    // fraction is in [0, 1] of the ray vector — so we scale a normalized
    // direction by maxDistance to keep the public API stable regardless of
    // whether the caller normalises the input or not.
    using namespace DirectX;
    XMVECTOR dirV = XMLoadFloat3(&direction);
    const float lenSq = XMVectorGetX(XMVector3LengthSq(dirV));
    if (lenSq < 1e-12f) return out;
    dirV = XMVector3Normalize(dirV);
    const JPH::Vec3 origin(from.x, from.y, from.z);
    const JPH::Vec3 dir(XMVectorGetX(dirV) * maxDistance,
                        XMVectorGetY(dirV) * maxDistance,
                        XMVectorGetZ(dirV) * maxDistance);

    JPH::RRayCast       ray(origin, dir);
    JPH::RayCastResult  hit;
    if (!m_impl->physics->GetNarrowPhaseQuery().CastRay(ray, hit))
        return out;

    out.hit      = true;
    out.distance = hit.mFraction * maxDistance;
    const JPH::Vec3 hitPos = ray.GetPointOnRay(hit.mFraction);
    out.point  = { hitPos.GetX(), hitPos.GetY(), hitPos.GetZ() };

    // Recover normal + owning entity via a brief body lock. Body::GetWorldSpaceSurfaceNormal
    // needs a live Body reference; the lock interface keeps the body alive
    // for the duration of the scope (no actual mutex blocking on read).
    JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded())
    {
        const JPH::Body& body = lock.GetBody();
        const JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
        out.normal = { n.GetX(), n.GetY(), n.GetZ() };
        // Entity ID stamped at body creation time (settings.mUserData = e in
        // Phase 1). NullEntity defaults are fine if the body is editor-owned
        // and the user-data slot was never set.
        out.entity = static_cast<Entity>(body.GetUserData());
    }
    return out;
}

// Shared implementation for capsule / sphere casts. Builds a single-use shape,
// sweeps it along `direction * maxDistance` and returns the closest hit.
// Sphere is just a capsule with halfHeight == 0 — Jolt's CapsuleShape asserts
// on non-positive halfHeight though, so the wrapper picks SphereShape in that
// case rather than coercing to a degenerate capsule.
static ShapeCastHit DoShapeCast(JPH::PhysicsSystem& physics,
                                JPH::Ref<JPH::Shape>  shape,
                                const DirectX::XMFLOAT3& from,
                                const DirectX::XMFLOAT3& direction,
                                float                    maxDistance,
                                std::uint32_t            ignoreBodyId,
                                std::uint16_t            layerMask)
{
    ShapeCastHit out;
    if (!shape || maxDistance <= 0.f) return out;

    using namespace DirectX;
    XMVECTOR dirV = XMLoadFloat3(&direction);
    const float lenSq = XMVectorGetX(XMVector3LengthSq(dirV));
    if (lenSq < 1e-12f) return out;
    dirV = XMVector3Normalize(dirV);

    const JPH::Vec3 origin(from.x, from.y, from.z);
    const JPH::Vec3 sweep(XMVectorGetX(dirV) * maxDistance,
                          XMVectorGetY(dirV) * maxDistance,
                          XMVectorGetZ(dirV) * maxDistance);

    // sFromWorldTransform expects centre-of-mass transform; for primitive
    // shapes the COM matches the local origin so a pure translation works.
    const JPH::RMat44 startXform = JPH::RMat44::sTranslation(origin);
    JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
        shape, JPH::Vec3::sReplicate(1.f), startXform, sweep);

    JPH::ShapeCastSettings settings;
    // BackFaceMode: with collide-back-faces enabled, a capsule that starts
    // already touching the floor still produces a fraction = 0 hit, which is
    // what ground detection wants. For collide-and-slide the caller pulls
    // back by skinWidth so the initial position should already be clear.
    settings.mBackFaceModeTriangles  = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex     = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mUseShrunkenShapeAndConvexRadius = false;
    settings.mReturnDeepestPoint     = false;

    // Note: braces — without them `IgnoreBodyFilter bodyFilter(JPH::BodyID(...))`
    // hits C++'s most-vexing-parse and is read as a function declaration.
    LayerMaskFilter   layerFilter(layerMask);
    IgnoreBodyFilter  bodyFilter{ JPH::BodyID(ignoreBodyId) };

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    physics.GetNarrowPhaseQuery().CastShape(
        shapeCast, settings, origin /*base offset*/, collector,
        {} /*BP filter — accept all, narrow phase enforces*/,
        layerFilter, bodyFilter);

    if (!collector.HadHit()) return out;
    const auto& hit = collector.mHit;

    out.hit      = true;
    out.fraction = hit.mFraction;
    out.distance = hit.mFraction * maxDistance;
    out.bodyId   = hit.mBodyID2.GetIndexAndSequenceNumber();

    // Contact point on body 2 is in world space. Penetration axis points
    // from body 1 INTO body 2; negate + normalize for the surface normal.
    const JPH::Vec3 cp = hit.mContactPointOn2;
    out.point  = { cp.GetX(), cp.GetY(), cp.GetZ() };
    JPH::Vec3 n = hit.mPenetrationAxis;
    if (n.LengthSq() > 1e-12f)
        n = -n.Normalized();
    else
        n = JPH::Vec3(0, 1, 0);
    out.normal = { n.GetX(), n.GetY(), n.GetZ() };

    JPH::BodyLockRead lock(physics.GetBodyLockInterface(), hit.mBodyID2);
    if (lock.Succeeded())
        out.entity = static_cast<Entity>(lock.GetBody().GetUserData());
    return out;
}

ShapeCastHit PhysicsSystem::CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                               const DirectX::XMFLOAT3& direction,
                                               float                    radius,
                                               float                    halfHeight,
                                               float                    maxDistance,
                                               std::uint32_t            ignoreBodyId,
                                               LayerMask                layerMask) const
{
    if (!m_impl || radius <= 0.f) return {};
    JPH::Ref<JPH::Shape> shape;
    // CapsuleShape requires halfHeight > 0; for the degenerate "capsule" case
    // (caller asked for halfHeight == 0) build a SphereShape instead so Jolt
    // doesn't assert.
    if (halfHeight > 1e-4f)
        shape = JPH::Ref<JPH::Shape>(new JPH::CapsuleShape(halfHeight, radius));
    else
        shape = JPH::Ref<JPH::Shape>(new JPH::SphereShape(radius));
    return DoShapeCast(*m_impl->physics, shape, from, direction, maxDistance,
                       ignoreBodyId, static_cast<std::uint16_t>(layerMask));
}

ShapeCastHit PhysicsSystem::CastSphereClosest(const DirectX::XMFLOAT3& from,
                                              const DirectX::XMFLOAT3& direction,
                                              float                    radius,
                                              float                    maxDistance,
                                              std::uint32_t            ignoreBodyId,
                                              LayerMask                layerMask) const
{
    if (!m_impl || radius <= 0.f) return {};
    JPH::Ref<JPH::Shape> shape(new JPH::SphereShape(radius));
    return DoShapeCast(*m_impl->physics, shape, from, direction, maxDistance,
                       ignoreBodyId, static_cast<std::uint16_t>(layerMask));
}

void PhysicsSystem::InvalidateMeshShapeCache(World& world)
{
    if (!m_impl) return;
    const size_t nShapes = m_impl->meshShapeCache.size();
    const size_t nBlobs  = m_impl->blobCache.size();
    m_impl->meshShapeCache.clear();
    m_impl->blobCache.clear();   // file contents may have changed on re-bake

    // Bump generation on every active Mesh collider so the runtime-swap
    // detector tears down + rebuilds the body next tick. Without this, bodies
    // built from now-cleared cache entries keep their stale ShapeRefC and
    // visually/physically diverge from the freshly baked geometry.
    size_t nBumped = 0;
    if (auto* colPool = world.GetPool<ColliderComponent>())
    {
        auto& colData = colPool->Data();
        for (auto& col : colData)
        {
            if (col.shape != ColliderComponent::Shape::Mesh) continue;
            ++col.generation;
            ++nBumped;
        }
    }
    if (nShapes + nBlobs + nBumped > 0)
        LOG_INFO("PhysicsSystem: cleared %zu mesh shapes + %zu blob(s); "
                 "bumped %zu Mesh-collider generations for rebuild",
                 nShapes, nBlobs, nBumped);
}

void PhysicsSystem::Shutdown()
{
    if (!m_impl) return;

    // CharacterVirtuals hold a raw pointer back to JPH::PhysicsSystem and
    // may touch BodyInterface on destruction (inner-body removal). Drop
    // them BEFORE physics.reset() to avoid use-after-free.
    m_impl->kccTable.clear();
    m_impl->kccShapeSnaps.clear();

    // Reverse order: bodies/broadphase live inside JPH::PhysicsSystem, so dropping
    // `physics` first lets its dtor unhook from the layer interfaces cleanly.
    m_impl->physics.reset();
    m_impl->contactListener.reset();
    m_impl->objLayerPairFilter.reset();
    m_impl->objVsBpFilter.reset();
    m_impl->bpLayerIface.reset();
    m_impl->jobSys.reset();
    m_impl->tempAlloc.reset();
    m_impl.reset();

    JPH::UnregisterTypes();
    if (JPH::Factory::sInstance)
    {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    LOG_INFO("PhysicsSystem: shutdown");
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------
static JPH::Vec3 ToJolt(const DirectX::XMFLOAT3& v) { return JPH::Vec3(v.x, v.y, v.z); }
static JPH::Quat ToJoltQuat(const DirectX::XMFLOAT4& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

static DirectX::XMFLOAT3 FromJolt(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }
static DirectX::XMFLOAT4 FromJolt(JPH::QuatArg q) { return { q.GetX(), q.GetY(), q.GetZ(), q.GetW() }; }

static JPH::EMotionType ToJolt(RigidBodyComponent::Motion m)
{
    switch (m)
    {
        case RigidBodyComponent::Motion::Static:    return JPH::EMotionType::Static;
        case RigidBodyComponent::Motion::Kinematic: return JPH::EMotionType::Kinematic;
        case RigidBodyComponent::Motion::Dynamic:   return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Dynamic;
}

// Pull world-space pose for body placement. Scene-graph mesh entities have
// identity LocalTransform and inherit their pose from a parent node, so
// LocalTransform alone places every static body at the origin. Decompose
// GlobalTransform.matrix (TransformSystem::Propagate keeps it fresh) into
// translation / rotation / scale instead; fall back to LocalTransform for
// rootless entities that never got a GlobalTransform.
struct WorldPose
{
    JPH::Vec3         pos;
    JPH::Quat         rot;
    DirectX::XMFLOAT3 scale;
};

static WorldPose ReadEntityWorldPose(World& world, Entity e)
{
    WorldPose out;
    out.pos   = JPH::Vec3::sZero();
    out.rot   = JPH::Quat::sIdentity();
    out.scale = { 1.f, 1.f, 1.f };

    if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e))
    {
        using namespace DirectX;
        XMVECTOR vScale, vQuat, vTrans;
        const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
        if (XMMatrixDecompose(&vScale, &vQuat, &vTrans, M))
        {
            XMFLOAT3 t, s; XMFLOAT4 q;
            XMStoreFloat3(&t, vTrans);
            XMStoreFloat4(&q, vQuat);
            XMStoreFloat3(&s, vScale);
            out.pos   = ToJolt(t);
            out.rot   = ToJoltQuat(q);
            out.scale = s;
            return out;
        }
    }
    if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
    {
        out.pos   = ToJolt(lt->translation);
        out.rot   = ToJoltQuat(lt->rotation);
        out.scale = lt->scale;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Mesh-collider decode: read positions + indices from a .imsh / .meshlib file
// straight off disk. PhysicsSystem only needs CPU vertex/index data — the
// runtime MeshLibrary keeps GPU-only buffers so we have to go back to the
// source. Mirrors the same primitives CollisionMeshBaker uses; kept inline to
// avoid a public coupling between Physics and Tools.
// ---------------------------------------------------------------------------
static bool DecodeImshPositions(const std::vector<uint8_t>& blob,
                                std::vector<DirectX::XMFLOAT3>& outPositions,
                                std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
        return false;
    const auto* meta = Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
    const uint32_t nv = meta->vertexCount;
    const uint32_t ni = meta->indexCount;
    const uint32_t vStride = meta->vertexStride;
    if (nv == 0 || ni == 0 || vStride < sizeof(float) * 3) return false;

    const uint8_t* payload = Resource::GetPayload(blob.data());
    const uint8_t* vbBase  = payload;
    const uint8_t* ibBase  = vbBase + size_t(nv) * vStride;
    outPositions.resize(nv);
    for (uint32_t i = 0; i < nv; ++i)
        std::memcpy(&outPositions[i], vbBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));
    outIndices.resize(ni);
    std::memcpy(outIndices.data(), ibBase, size_t(ni) * sizeof(uint32_t));
    return true;
}

static bool DecodeMeshLibSlice(const std::vector<uint8_t>& blob, uint32_t meshId,
                               std::vector<DirectX::XMFLOAT3>& outPositions,
                               std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
        return false;
    const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
    if (meshId >= meta->meshCount) return false;

    const uint8_t* payload = Resource::GetPayload(blob.data());
    const auto*    entries = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
    const auto&    entry   = entries[meshId];
    if (entry.vertexCount == 0 || entry.indexCount == 0) return false;

    const uint32_t vStride   = meta->vertexStride;
    const uint8_t* vbBase    = payload + size_t(meta->meshCount) * sizeof(Resource::MeshLibraryEntry);
    const uint8_t* ibBase    = vbBase + size_t(meta->vertexCount) * vStride;
    const uint8_t* vertsBase = vbBase + size_t(entry.vertexStart) * vStride;
    const auto*    idxData   = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;

    outPositions.resize(entry.vertexCount);
    for (uint32_t i = 0; i < entry.vertexCount; ++i)
        std::memcpy(&outPositions[i], vertsBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));
    outIndices.resize(entry.indexCount);
    for (uint32_t i = 0; i < entry.indexCount; ++i)
        outIndices[i] = idxData[i] - entry.vertexStart;
    return true;
}

// Build (or look up in cache) the JPH::MeshShape for a ColliderComponent that
// references a .imsh / .meshlib file. Returns nullptr on failure.
// Takes the caches by reference rather than PhysicsSystem::Impl& because Impl
// is a private nested type and these helpers live at namespace scope.
using MeshShapeCache = std::unordered_map<std::string, JPH::ShapeRefC>;
using BlobCache      = std::unordered_map<std::string, std::vector<uint8_t>>;

static JPH::ShapeRefC GetOrBuildMeshShape(MeshShapeCache& cache,
                                          BlobCache&      blobs,
                                          const ColliderComponent& c)
{
    if (c.meshLibPath.empty()) return nullptr;

    const std::string key = c.meshLibPath + '#' + std::to_string(c.meshLibMeshId);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    // Reuse the file blob across every entity that points at this .meshlib.
    // First time we see a path we pay one 18 MB read; siblings drop straight
    // through to the decode step using the cached bytes.
    auto blobIt = blobs.find(c.meshLibPath);
    if (blobIt == blobs.end())
    {
        std::vector<uint8_t> fresh;
        if (!Resource::AssetFS::Get().ReadFile(c.meshLibPath, fresh) || fresh.empty())
        {
            LOG_ERROR("PhysicsSystem: cannot read mesh collider source '%s'",
                      c.meshLibPath.c_str());
            return nullptr;
        }
        blobIt = blobs.emplace(c.meshLibPath, std::move(fresh)).first;
    }
    const std::vector<uint8_t>& blob = blobIt->second;

    // File-extension dispatch: same convention as the renderer (.meshlib = new
    // multi-mesh format, .imsh = legacy single-mesh).
    std::string ext;
    {
        std::filesystem::path p(c.meshLibPath);
        ext = p.extension().string();
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;
    bool decoded = false;
    if      (ext == ".meshlib") decoded = DecodeMeshLibSlice(blob, c.meshLibMeshId, positions, indices);
    else if (ext == ".imsh")    decoded = DecodeImshPositions(blob, positions, indices);
    if (!decoded || positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
    {
        LOG_ERROR("PhysicsSystem: failed to decode mesh collider source '%s' (mesh=%u)",
                  c.meshLibPath.c_str(), c.meshLibMeshId);
        return nullptr;
    }

    // Jolt wants Float3 + IndexedTriangle arrays. material index 0 → default
    // physics material (we don't use per-triangle materials).
    JPH::VertexList         joltVerts;
    JPH::IndexedTriangleList joltTris;
    joltVerts.reserve(positions.size());
    for (const auto& p : positions)
        joltVerts.emplace_back(p.x, p.y, p.z);
    joltTris.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        // Skip degenerate triangles — Jolt asserts on those during shape build.
        if (indices[i] == indices[i+1] || indices[i+1] == indices[i+2] || indices[i] == indices[i+2])
            continue;
        joltTris.emplace_back(indices[i], indices[i+1], indices[i+2], 0u);
    }
    if (joltTris.empty())
    {
        LOG_ERROR("PhysicsSystem: mesh collider '%s' has no valid triangles after dedup",
                  c.meshLibPath.c_str());
        return nullptr;
    }

    JPH::MeshShapeSettings settings(std::move(joltVerts), std::move(joltTris));
    auto result = settings.Create();
    if (result.HasError())
    {
        LOG_ERROR("PhysicsSystem: Jolt MeshShape build failed for '%s': %s",
                  c.meshLibPath.c_str(), result.GetError().c_str());
        return nullptr;
    }

    JPH::ShapeRefC shape = result.Get();
    cache.emplace(key, shape);
    LOG_INFO("PhysicsSystem: built MeshShape '%s' (mesh=%u, %zu verts, %zu tris)",
             c.meshLibPath.c_str(), c.meshLibMeshId,
             positions.size(), indices.size() / 3);
    return shape;
}

static JPH::ShapeRefC MakeShape(MeshShapeCache& cache,
                                BlobCache&      blobs,
                                const ColliderComponent& c,
                                const DirectX::XMFLOAT3& scale)
{
    using namespace JPH;
    ShapeRefC inner;
    switch (c.shape)
    {
        case ColliderComponent::Shape::Box:
            inner = new BoxShape(ToJolt(c.halfExtents));
            break;
        case ColliderComponent::Shape::Sphere:
            inner = new SphereShape(c.radius);
            break;
        case ColliderComponent::Shape::Capsule:
            inner = new CapsuleShape(c.halfHeight, c.radius);
            break;
        case ColliderComponent::Shape::Mesh:
        {
            ShapeRefC mesh = GetOrBuildMeshShape(cache, blobs, c);
            if (!mesh) { inner = new BoxShape(Vec3::sReplicate(0.5f)); break; }
            // Wrap in ScaledShape only if the entity is actually scaled —
            // ScaledShape rejects unit scale via JPH_ASSERT in some builds and
            // adds an indirection we can skip for the common case.
            const bool unitScale =
                std::fabs(scale.x - 1.f) < 1e-4f &&
                std::fabs(scale.y - 1.f) < 1e-4f &&
                std::fabs(scale.z - 1.f) < 1e-4f;
            inner = unitScale ? mesh : ShapeRefC(new ScaledShape(mesh, Vec3(scale.x, scale.y, scale.z)));
            break;
        }
    }
    if (!inner) inner = new BoxShape(Vec3::sReplicate(0.5f));

    // Apply per-shape local-space offset + rotation. Jolt primitives are
    // centered at the shape origin; without this wrap a capsule on a
    // character (mesh pivot at feet) would sink half its height into the
    // floor, and a capsule that should lie on its side would always stand
    // upright. RotatedTranslatedShape also composes correctly with the
    // Mesh path's ScaledShape since we wrap last.
    const bool zeroOffset =
        std::fabs(c.offset.x) < 1e-6f &&
        std::fabs(c.offset.y) < 1e-6f &&
        std::fabs(c.offset.z) < 1e-6f;
    const bool zeroRotation =
        std::fabs(c.rotationEulerDeg.x) < 1e-6f &&
        std::fabs(c.rotationEulerDeg.y) < 1e-6f &&
        std::fabs(c.rotationEulerDeg.z) < 1e-6f;
    if (zeroOffset && zeroRotation) return inner;

    Quat rot = Quat::sIdentity();
    if (!zeroRotation)
    {
        using namespace DirectX;
        const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(c.rotationEulerDeg.x),
            XMConvertToRadians(c.rotationEulerDeg.y),
            XMConvertToRadians(c.rotationEulerDeg.z));
        XMFLOAT4 qf; XMStoreFloat4(&qf, q);
        rot = Quat(qf.x, qf.y, qf.z, qf.w);
    }

    return new RotatedTranslatedShape(
        Vec3(c.offset.x, c.offset.y, c.offset.z),
        rot,
        inner);
}

// ---------------------------------------------------------------------------
// PrewarmMeshShapes — parallel build + single file-read for all mesh
// colliders that the next physics tick will need. Defined down here so the
// static decode helpers + MakeShape are already declared above.
// ---------------------------------------------------------------------------
void PhysicsSystem::PrewarmMeshShapes(const std::vector<MeshShapeRequest>& requests)
{
    if (!m_impl || requests.empty()) return;
    const auto tStart = std::chrono::steady_clock::now();

    // Group requests by source path so each .meshlib file is read exactly once.
    std::unordered_map<std::string, std::vector<uint32_t>> byPath;
    for (const auto& r : requests)
    {
        if (r.path.empty()) continue;
        const std::string key = r.path + '#' + std::to_string(r.meshId);
        if (m_impl->meshShapeCache.count(key) != 0) continue; // already cached
        byPath[r.path].push_back(r.meshId);
    }
    if (byPath.empty())
    {
        LOG_INFO("PhysicsSystem: PrewarmMeshShapes — all %zu requests already cached",
                 requests.size());
        return;
    }

    size_t totalBuilt = 0;
    size_t totalFailed = 0;

    for (const auto& [path, meshIds] : byPath)
    {
        // 1) Read the meshlib once. 18 MB blob × N call sites is what made
        //    the lazy path so slow — this single read replaces all of them.
        std::vector<uint8_t> blob;
        if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
        {
            LOG_ERROR("PhysicsSystem: PrewarmMeshShapes — cannot read '%s'", path.c_str());
            continue;
        }

        std::string ext;
        {
            std::filesystem::path p(path);
            ext = p.extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        // 2) Parallel build. Each worker decodes its meshId slice and runs
        //    JPH::MeshShapeSettings::Create() — the BVH-build step is the
        //    expensive part and is fully thread-safe.
        const uint32_t N = static_cast<uint32_t>(meshIds.size());
        std::vector<JPH::ShapeRefC> built(N);

        TaskSystem::Get().ParallelFor(0, N, [&](uint32_t i)
        {
            std::vector<DirectX::XMFLOAT3> positions;
            std::vector<uint32_t>          indices;
            bool decoded = false;
            if      (ext == ".meshlib") decoded = DecodeMeshLibSlice(blob, meshIds[i], positions, indices);
            else if (ext == ".imsh")    decoded = DecodeImshPositions(blob, positions, indices);
            if (!decoded || positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
                return;

            JPH::VertexList          verts;
            JPH::IndexedTriangleList tris;
            verts.reserve(positions.size());
            for (const auto& p : positions)
                verts.emplace_back(p.x, p.y, p.z);
            tris.reserve(indices.size() / 3);
            for (size_t j = 0; j < indices.size(); j += 3)
            {
                if (indices[j] == indices[j+1] || indices[j+1] == indices[j+2] || indices[j] == indices[j+2])
                    continue;
                tris.emplace_back(indices[j], indices[j+1], indices[j+2], 0u);
            }
            if (tris.empty()) return;

            JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
            auto result = settings.Create();
            if (result.HasError()) return;
            built[i] = result.Get();
        }, TaskSystem::TaskPriority::High);

        // 3) Bulk insert on main thread — no mutex needed.
        for (uint32_t i = 0; i < N; ++i)
        {
            if (!built[i])
            {
                ++totalFailed;
                continue;
            }
            std::string key = path + '#' + std::to_string(meshIds[i]);
            m_impl->meshShapeCache.emplace(std::move(key), built[i]);
            ++totalBuilt;
        }
    }

    const auto tEnd  = std::chrono::steady_clock::now();
    const float ms   = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
    LOG_INFO("PhysicsSystem: PrewarmMeshShapes built %zu shapes (%zu failed) in %.1f ms",
             totalBuilt, totalFailed, ms);
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Update: legacy single-call entry point. Drives the INTERNAL accumulator
// for backward compatibility. The phase scheduler in App::Run uses the
// split entry points (PreAllSteps + StepOnce + PostAllSteps) instead so
// FixedPhysicsPre/Post phases can run between substeps.
void PhysicsSystem::Update(World& world, float dt)
{
    if (!m_impl) return;
    PreAllSteps(world);

    m_impl->accumulator += dt;
    constexpr float kMaxAccum = 0.25f;   // spiral-of-death guard (15 fixed steps)
    if (m_impl->accumulator > kMaxAccum) m_impl->accumulator = kMaxAccum;

    while (m_impl->accumulator >= Impl::kFixedDt)
    {
        StepOnce(world);
        m_impl->accumulator -= Impl::kFixedDt;
    }

    PostAllSteps(world);
}

// ---------------------------------------------------------------------------
// PreAllSteps: auto-prewarm + collider rebuild + body / KCC creation +
// Inspector-side teleport detection. Runs ONCE per render frame regardless
// of substep count.
void PhysicsSystem::PreAllSteps(World& world)
{
    if (!m_impl) return;

    // Auto-prewarm: scan ColliderComponents for any Mesh shapes whose
    // JPH::MeshShape isn't built yet. Batch-build them in parallel BEFORE
    // Phase 1 — without this, body creation would build them one at a time
    // on the main thread (the lazy GetOrBuildMeshShape path), which for a
    // freshly-loaded .iscene with thousands of mesh colliders means seconds
    // of serial BVH construction the first Play press. Cheap O(N) scan when
    // everything is already cached, so it's safe to run every frame.
    {
        std::vector<MeshShapeRequest> uncached;
        if (auto* colPool = world.GetPool<ColliderComponent>())
        {
            const auto& colData = colPool->Data();
            for (const auto& col : colData)
            {
                if (col.shape != ColliderComponent::Shape::Mesh) continue;
                if (col.meshLibPath.empty()) continue;
                const std::string key =
                    col.meshLibPath + '#' + std::to_string(col.meshLibMeshId);
                if (m_impl->meshShapeCache.count(key) != 0) continue;
                uncached.push_back({ col.meshLibPath, col.meshLibMeshId });
            }
        }
        if (!uncached.empty())
            PrewarmMeshShapes(uncached);
    }

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();

    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    // Phase 0.6 — runtime collider swap. For every entity whose body exists,
    // tear the Jolt body down if its source ColliderComponent has changed in
    // any shape-affecting way. Two trigger paths:
    //   (a) ColliderComponent::generation advanced (caller invoked MarkDirty,
    //       or InvalidateMeshShapeCache bumped Mesh entries after a re-bake)
    //   (b) Field-level diff against colliderSnapshots — catches Inspector
    //       reflection writes that bypass MarkDirty.
    // Phase 1 (below) then recreates the body from the new fields.
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId) continue;

        const Entity e = rbEntities[i];
        const ColliderComponent* col = world.GetComponent<ColliderComponent>(e);
        if (!col)
        {
            // Collider was removed: tear down the orphaned body so the next
            // Phase 1 doesn't re-attach it accidentally either.
            bodyIface.RemoveBody(JPH::BodyID(rb.bodyId));
            bodyIface.DestroyBody(JPH::BodyID(rb.bodyId));
            rb.bodyId = kInvalidPhysicsBodyId;
            rb.lastBuiltGeneration = 0;
            m_impl->colliderSnapshots.erase(e);
            m_impl->poseSnapshots.erase(e);
            continue;
        }

        bool needsRebuild = (rb.lastBuiltGeneration != col->generation);
        if (!needsRebuild && rb.lastBuiltLockedAxes != rb.lockedAxes)
        {
            // Jolt has no public BodyInterface for runtime DOF change — body
            // needs to be recreated so MotionProperties::SetMassProperties
            // recomputes the inertia tensor with the new allowed-DOF mask.
            needsRebuild = true;
        }
        if (!needsRebuild)
        {
            auto snapIt = m_impl->colliderSnapshots.find(e);
            if (snapIt == m_impl->colliderSnapshots.end()
                || !ColliderShapeEqual(snapIt->second, *col))
            {
                needsRebuild = true;
            }
        }
        if (!needsRebuild) continue;

        const JPH::BodyID id(rb.bodyId);
        bodyIface.RemoveBody(id);
        bodyIface.DestroyBody(id);
        rb.bodyId = kInvalidPhysicsBodyId;
        m_impl->colliderSnapshots.erase(e);
        m_impl->poseSnapshots.erase(e);
        LOG_INFO("PhysicsSystem: collider changed on entity %u — rebuilding body", e);
    }

    // Phase 1 — lazily create Jolt bodies for entities that don't have one yet.
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId != kInvalidPhysicsBodyId) continue;

        const Entity e = rbEntities[i];
        const ColliderComponent* col = world.GetComponent<ColliderComponent>(e);
        if (!col) continue;

        // CCC takes precedence over RigidBody on the same entity — the
        // docstring on CharacterControllerComponent says they "cannot
        // coexist". Skip RB creation so the CharacterVirtual is the sole
        // collider for character entities. Without this guard, both a
        // kinematic body and a CV exist at the same world pose; the CV's
        // penetration recovery pushes the character off its own RB,
        // producing the "magic motion" / jitter symptom that's been
        // chasing the user.
        if (world.HasComponent<CharacterControllerComponent>(e))
        {
            static std::unordered_set<Entity> s_warned;
            if (s_warned.insert(e).second)
            {
                LOG_WARNING("PhysicsSystem: entity %u has both CharacterControllerComponent "
                            "and RigidBodyComponent — skipping RB body creation (CCC wins). "
                            "Remove RigidBody + Collider from this entity to silence this "
                            "warning; CCC's CharacterVirtual handles its own collision.",
                            static_cast<uint32_t>(e));
            }
            continue;
        }

        const WorldPose wp = ReadEntityWorldPose(world, e);
        const JPH::Vec3 pos = wp.pos;
        const JPH::Quat rot = wp.rot;
        const DirectX::XMFLOAT3 scl = wp.scale;

        // Trimesh shapes are valid only on static bodies in Jolt — clamp the
        // motion type if the user paired a Mesh collider with a Dynamic /
        // Kinematic RigidBody.
        const JPH::EMotionType rawMt = ToJolt(rb.motion);
        const JPH::EMotionType mt =
            (col->shape == ColliderComponent::Shape::Mesh && rawMt != JPH::EMotionType::Static)
                ? JPH::EMotionType::Static : rawMt;
        if (mt != rawMt)
        {
            LOG_WARNING("PhysicsSystem: entity %u uses Mesh collider but motion=%d; "
                        "coercing to Static (Jolt MeshShape doesn't support dynamic bodies)",
                        e, (int)rb.motion);
            rb.motion = RigidBodyComponent::Motion::Static;
        }
        // RigidBodyComponent maps to the gameplay layer set as: Static →
        // STATIC_ENV, Dynamic/Kinematic → DYNAMIC_PROP. Character bodies are
        // not routed through RigidBodyComponent (they go through
        // CharacterControllerComponent + Jolt CharacterVirtual instead).
        const JPH::ObjectLayer layer =
            (mt == JPH::EMotionType::Static) ? Layers::STATIC_ENV : Layers::DYNAMIC_PROP;

        JPH::ShapeRefC shape = MakeShape(m_impl->meshShapeCache, m_impl->blobCache, *col, scl);
        JPH::BodyCreationSettings settings(shape, pos, rot, mt, layer);
        settings.mLinearDamping  = rb.linearDamping;
        settings.mAngularDamping = rb.angularDamping;
        settings.mFriction       = rb.friction;
        settings.mRestitution    = rb.restitution;
        settings.mGravityFactor  = rb.gravityFactor;
        // Axis locks only matter on Dynamic bodies — Static/Kinematic motion
        // doesn't run velocity integration in the first place. Bypassing it
        // for the non-Dynamic case keeps Jolt on its default fast path where
        // mass properties are skipped entirely.
        if (mt == JPH::EMotionType::Dynamic)
            settings.mAllowedDOFs = ToJoltAllowedDOFs(rb.lockedAxes);
        if (mt == JPH::EMotionType::Dynamic)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rb.mass;
        }

        // Stash the owning entity in user-data so the ContactListener can
        // identify contacts without a BodyID → Entity reverse map.
        settings.mUserData = static_cast<JPH::uint64>(e);

        const JPH::EActivation act = (mt == JPH::EMotionType::Static)
                                     ? JPH::EActivation::DontActivate
                                     : JPH::EActivation::Activate;
        const JPH::BodyID id = bodyIface.CreateAndAddBody(settings, act);
        if (id.IsInvalid())
        {
            LOG_ERROR("PhysicsSystem: CreateAndAddBody failed for entity %u (body pool full?)", e);
            continue;
        }
        rb.bodyId = id.GetIndexAndSequenceNumber();
        rb.lastBuiltGeneration = col->generation;
        rb.lastBuiltLockedAxes = rb.lockedAxes;
        // Snapshot the shape-defining fields so Phase 0.6 next frame can
        // detect direct Inspector edits without requiring MarkDirty().
        m_impl->colliderSnapshots[e] = *col;
        LOG_INFO("PhysicsSystem: created body %u for entity %u (motion=%d)",
                 id.GetIndexAndSequenceNumber(), e, (int)rb.motion);

        // Seed the interpolation snapshot so the first render frame after
        // creation doesn't lerp from a stale prev-pose at the origin.
        if (mt == JPH::EMotionType::Dynamic)
        {
            auto& snap = m_impl->poseSnapshots[e];
            snap.currPos = bodyIface.GetPosition(id);
            snap.currRot = bodyIface.GetRotation(id);
            snap.prevPos = snap.currPos;
            snap.prevRot = snap.currRot;
        }
    }

    // Phase 1c — Kinematic Character Controllers (KCC).
    // Independent of RigidBody/Collider — uses JPH::CharacterVirtual which
    // does not register a body with the broadphase. Lazy-create on first
    // sight; rebuild when capsule dimensions change (no live shape-swap API
    // on CharacterVirtual); tear down when the component is removed.
    {
        auto* ccPool = world.GetPool<CharacterControllerComponent>();

        // Tear down KCCs whose entity lost (or never had) the component.
        for (auto it = m_impl->kccTable.begin(); it != m_impl->kccTable.end(); )
        {
            const Entity e = it->first;
            if (!world.IsAlive(e) || !world.HasComponent<CharacterControllerComponent>(e))
            {
                LOG_INFO("PhysicsSystem: tearing down KCC for entity %u (component removed)", e);
                m_impl->kccShapeSnaps.erase(e);
                it = m_impl->kccTable.erase(it);
            }
            else ++it;
        }

        if (ccPool)
        {
            const auto& ccEntities = ccPool->Entities();
            auto&       ccData     = ccPool->Data();
            const size_t ncc       = ccData.size();
            for (size_t i = 0; i < ncc; ++i)
            {
                CharacterControllerComponent& cc = ccData[i];
                const Entity e = ccEntities[i];

                auto cvIt = m_impl->kccTable.find(e);
                const bool exists = (cvIt != m_impl->kccTable.end());

                // Decide whether to (re)build. Trigger paths:
                //   (a) No CV yet for this entity
                //   (b) cc.generation advanced past lastBuiltGeneration (caller
                //       called MarkDirty after editing capsule fields)
                //   (c) Field-level diff against kccShapeSnaps catches direct
                //       Inspector edits that bypassed MarkDirty
                bool needsBuild = !exists;
                if (exists)
                {
                    if (cc.generation != cc.lastBuiltGeneration)
                        needsBuild = true;
                    auto snapIt = m_impl->kccShapeSnaps.find(e);
                    if (snapIt == m_impl->kccShapeSnaps.end()
                        || snapIt->second.radius     != cc.capsuleRadius
                        || snapIt->second.halfHeight != cc.capsuleHalfHeight)
                    {
                        needsBuild = true;
                    }
                }

                if (!needsBuild) continue;

                const WorldPose wp = ReadEntityWorldPose(world, e);

                JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
                settings->mShape = JPH::ShapeRefC(new JPH::CapsuleShape(cc.capsuleHalfHeight, cc.capsuleRadius));
                settings->mUp = JPH::Vec3(0, 1, 0);
                settings->mMaxSlopeAngle = cc.maxSlopeRad;
                settings->mMass = cc.mass;
                // Lift the capsule so its bottom hemisphere touches the entity
                // pivot. Without this the entity's mesh (typically pivoted at
                // the feet) would sink half its height into the floor.
                settings->mShapeOffset = JPH::Vec3(0, cc.capsuleHalfHeight + cc.capsuleRadius, 0);
                settings->mCharacterPadding = cc.skinWidth;
                settings->mPredictiveContactDistance = 0.1f;

                JPH::Ref<JPH::CharacterVirtual> cv = new JPH::CharacterVirtual(
                    settings.GetPtr(), wp.pos, wp.rot,
                    static_cast<JPH::uint64>(e), m_impl->physics.get());

                m_impl->kccTable[e]      = cv;
                m_impl->kccShapeSnaps[e] = { cc.capsuleRadius, cc.capsuleHalfHeight };
                cc.lastBuiltGeneration   = cc.generation;
                cc.bodyId                = kInvalidPhysicsBodyId; // no inner body for now
                // Reset run-time state — caller starts at rest.
                cc.velocity     = { 0.f, 0.f, 0.f };
                cc.isGrounded   = false;
                cc.wasGrounded  = false;
                cc.groundNormal = { 0.f, 1.f, 0.f };
                cc.groundEntity = NullEntity;
                cc.timeInAir    = 0.f;

                #if KCC_DEBUG >= 1
                // Diagnostic at first build / rebuild: dump the world-pose
                // sources so we can spot LT/Global mismatches and parent
                // links. KCC rebuilding *during play* is suspicious — log
                // it loud so we know it happened.
                {
                    const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
                    const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                    const auto* parent = world.GetComponent<Parent>(e);
                    LOG_INFO("[KCC ent=%u] CV (re)build: wp.pos=(%.3f,%.3f,%.3f) "
                             "LT.t=(%.3f,%.3f,%.3f) GT[3]=(%.3f,%.3f,%.3f) parent=%u  "
                             "(if LT vs GT diverge → parented; if rebuild happens mid-play → MarkDirty unexpectedly)",
                             static_cast<uint32_t>(e),
                             wp.pos.GetX(), wp.pos.GetY(), wp.pos.GetZ(),
                             lt ? lt->translation.x : 0.f,
                             lt ? lt->translation.y : 0.f,
                             lt ? lt->translation.z : 0.f,
                             gt ? gt->matrix._41 : 0.f,
                             gt ? gt->matrix._42 : 0.f,
                             gt ? gt->matrix._43 : 0.f,
                             parent ? static_cast<uint32_t>(parent->entity) : 0u);
                }
                #endif

                LOG_INFO("PhysicsSystem: created CharacterVirtual for entity %u "
                         "(radius=%.2f halfHeight=%.2f slope=%.0fdeg)",
                         e, cc.capsuleRadius, cc.capsuleHalfHeight,
                         cc.maxSlopeRad * 57.2957795f);
            }
        }
    }

    // Phase 2 — reconcile Inspector-side edits (motion type changes, manual
    // transform edits while static/kinematic) with Jolt before simulating.
    //
    //   * Motion type changed in the UI  → SetMotionType + re-map object layer
    //   * Static / Kinematic body        → LocalTransform is authoritative,
    //                                      push it down each frame so switching
    //                                      back to Dynamic starts from the
    //                                      edited pose (not the stale Jolt one)
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId) continue;

        const JPH::BodyID      id       = JPH::BodyID(rb.bodyId);
        const JPH::EMotionType targetMt = ToJolt(rb.motion);
        const JPH::EMotionType currMt   = bodyIface.GetMotionType(id);

        if (currMt != targetMt)
        {
            bodyIface.SetMotionType(id, targetMt, JPH::EActivation::Activate);
            const JPH::ObjectLayer newLayer =
                (targetMt == JPH::EMotionType::Static) ? Layers::STATIC_ENV
                                                      : Layers::DYNAMIC_PROP;
            bodyIface.SetObjectLayer(id, newLayer);

            // Motion change re-anchors the interpolation snapshot at the
            // current Jolt pose so the first Dynamic frame doesn't lerp from
            // a stale value left over by the previous motion mode.
            if (targetMt == JPH::EMotionType::Dynamic)
            {
                auto& snap = m_impl->poseSnapshots[rbEntities[i]];
                snap.currPos = bodyIface.GetPosition(id);
                snap.currRot = bodyIface.GetRotation(id);
                snap.prevPos = snap.currPos;
                snap.prevRot = snap.currRot;
            }
        }

        if (rb.motion != RigidBodyComponent::Motion::Dynamic)
        {
            // Static / Kinematic: use the world-space pose (GlobalTransform)
            // so scene-graph children placed via parent transforms end up at
            // their visual position. LocalTransform alone is identity for
            // mesh entities under a SceneNodeTag parent.
            const WorldPose wp = ReadEntityWorldPose(world, rbEntities[i]);
            bodyIface.SetPositionAndRotation(id, wp.pos, wp.rot, JPH::EActivation::Activate);
            continue;
        }

        // Dynamic path uses LocalTransform — top-level dynamic bodies write
        // back to LocalTransform in Phase 4, so we read from the same place
        // here to detect external teleports.
        const LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        if (!lt) continue;

        const JPH::Vec3 ltPos = ToJolt(lt->translation);
        const JPH::Quat ltRot = ToJoltQuat(lt->rotation);

        {
            // Dynamic: physics is normally authoritative. But Phase 4 only pulls
            // back what Jolt just wrote — so if LocalTransform differs from Jolt's
            // current pose, somebody (Inspector, script, gizmo) edited it after
            // our last pull. Treat that as a teleport: push the edit to Jolt and
            // zero velocity so it drops from the new pose instead of carrying
            // stale momentum.
            const JPH::Vec3 joltPos = bodyIface.GetPosition(id);
            const JPH::Quat joltRot = bodyIface.GetRotation(id);

            const bool posChanged = !joltPos.IsClose(ltPos);
            const bool rotChanged =
                std::fabs(joltRot.GetX() - ltRot.GetX()) > 1e-6f ||
                std::fabs(joltRot.GetY() - ltRot.GetY()) > 1e-6f ||
                std::fabs(joltRot.GetZ() - ltRot.GetZ()) > 1e-6f ||
                std::fabs(joltRot.GetW() - ltRot.GetW()) > 1e-6f;

            if (posChanged || rotChanged)
            {
                bodyIface.SetPositionAndRotation(id, ltPos, ltRot, JPH::EActivation::Activate);
                bodyIface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());

                // Re-seed interpolation snapshot — otherwise Phase 4 would
                // lerp from the teleport's old curr-pose to the new one and
                // visibly smear across the gap for one physics frame.
                auto& snap = m_impl->poseSnapshots[rbEntities[i]];
                snap.currPos = ltPos;
                snap.currRot = ltRot;
                snap.prevPos = snap.currPos;
                snap.prevRot = snap.currRot;
            }
        }
    }

}

// ---------------------------------------------------------------------------
// StepOnce: one fixed-dt physics substep — prev/curr snapshot, KCC tick,
// Jolt step, post-step snapshot. Called N times per render frame by the
// caller's accumulator loop. Does NOT touch the internal accumulator.
void PhysicsSystem::StepOnce(World& world)
{
    if (!m_impl) return;

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    {
        // BEFORE the step: roll last frame's "current" into "previous".
        // After enough steps, prev/curr always bracket the last [t-dt, t]
        // physics interval — that's what Phase 4's alpha interpolates across.
        for (size_t i = 0; i < n; ++i)
        {
            const RigidBodyComponent& rb = rbData[i];
            if (rb.bodyId == kInvalidPhysicsBodyId) continue;
            if (rb.motion != RigidBodyComponent::Motion::Dynamic) continue;
            auto it = m_impl->poseSnapshots.find(rbEntities[i]);
            if (it == m_impl->poseSnapshots.end()) continue;
            it->second.prevPos = it->second.currPos;
            it->second.prevRot = it->second.currRot;
        }

        // KCC tick — must run BEFORE physics->Update so the character's
        // motion (including any pushes it applies to dynamic props) is
        // reflected in the same simulation step. Each CV maintains its own
        // collide-and-slide internally; we just feed it a target velocity.
        if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
        {
            const auto& ccEntities = ccPool->Entities();
            auto&       ccData     = ccPool->Data();
            const size_t ncc       = ccData.size();

            // Per-step diagnostic throttle (≈ twice per second @ 60Hz physics).
            // Toggle: set KCC_DEBUG to 0 to silence. Edge events (jump apply,
            // ground transition, blocked transition) still fire when KCC_DEBUG >= 1.
            #ifndef KCC_DEBUG
            #define KCC_DEBUG 1
            #endif
            #if KCC_DEBUG >= 1
            static int s_kccLogCounter = 0;
            ++s_kccLogCounter;
            const bool kccLogFire = (s_kccLogCounter % 30) == 0;
            #endif

            for (size_t i = 0; i < ncc; ++i)
            {
                CharacterControllerComponent& cc = ccData[i];
                const Entity e = ccEntities[i];
                auto cvIt = m_impl->kccTable.find(e);
                if (cvIt == m_impl->kccTable.end()) {
                    #if KCC_DEBUG >= 1
                    if (kccLogFire)
                        LOG_WARNING("[KCC ent=%u] has CharacterControllerComponent but NO CharacterVirtual in kccTable — not driven this step",
                                    static_cast<uint32_t>(e));
                    #endif
                    continue;
                }
                JPH::CharacterVirtual* cv = cvIt->second.GetPtr();

                #if KCC_DEBUG >= 1
                // Snapshot pre-step values for the per-step log line.
                const JPH::Vec3 lvPre  = cv->GetLinearVelocity();
                const JPH::Vec3 posPre = cv->GetPosition();
                const auto      gsPre  = cv->GetGroundState();
                const bool      preWasGrounded = cc.isGrounded;
                #endif

                // ---- Escape hatch (DesignMd §7 坑 10) --------------------
                // Cutscene / teleport sets this to flush inertia. Acts BEFORE
                // any velocity integration so the next sweep starts clean.
                if (cc.instantStopRequested)
                {
                    cv->SetLinearVelocity(JPH::Vec3::sZero());
                    cc.desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
                    cc.velocity                  = { 0.f, 0.f, 0.f };
                    cc.instantStopRequested      = false;
                    #if KCC_DEBUG >= 1
                    LOG_INFO("[KCC ent=%u] instant-stop consumed", static_cast<uint32_t>(e));
                    #endif
                }

                // Was the character supported on ground at the start of this
                // step? Use the CV's own state from the *previous* step —
                // that's what gates jump vs. continued fall.
                const auto prevGround = cv->GetGroundState();
                const bool wasOnGround =
                    (prevGround == JPH::CharacterBase::EGroundState::OnGround);

                // Snapshot pre-step XZ pose so we can compute actual
                // displacement (for wasBlockedLastStep detection below).
                const JPH::Vec3 prePos = cv->GetPosition();

                // ---- Off-mesh-link traversal --------------------------
                // While in LaunchedTraversal, NavAgent has pre-set the CV's
                // velocity to a ballistic launch. Motor keeps integrating
                // gravity, but does NOT overwrite the linear velocity from
                // desiredHorizontalVelocity — let the projectile fly.
                const bool launched = (cc.mode == MovementMode::LaunchedTraversal);

                // Vertical integration — KCC owns it (not Jolt's PhysicsSystem
                // gravity). Grounded → cancel any leftover -Y from the prior
                // step so we don't accumulate; airborne → add gameplay g.
                float vy = cv->GetLinearVelocity().GetY();
                if (wasOnGround && !launched)
                {
                    vy = 0.f;
                    if (cc.jumpRequested)
                    {
                        vy = cc.jumpSpeed;
                        cc.jumpRequested = false;
                        #if KCC_DEBUG >= 1
                        LOG_INFO("[KCC ent=%u] JUMP applied vy=%.2f (grounded)",
                                 static_cast<uint32_t>(e), vy);
                        #endif
                    }
                }
                else
                {
                    vy += cc.gravity * Impl::kFixedDt;
                    if (vy < -cc.maxFallSpeed) vy = -cc.maxFallSpeed;
                    // Coyote-time / "jump after stepping off a ledge" is left
                    // to gameplay code reading cc.timeInAir — don't honour
                    // jumpRequested in air here.
                    #if KCC_DEBUG >= 1
                    if (cc.jumpRequested)
                        LOG_INFO("[KCC ent=%u] jump REJECTED (airborne, wasOnGround=%d launched=%d)",
                                 static_cast<uint32_t>(e), wasOnGround ? 1 : 0, launched ? 1 : 0);
                    #endif
                    cc.jumpRequested = false;
                }

                if (launched)
                {
                    // Keep horizontal from the launch vector — only patch Y.
                    const JPH::Vec3 lvNow = cv->GetLinearVelocity();
                    cv->SetLinearVelocity(JPH::Vec3(lvNow.GetX(), vy, lvNow.GetZ()));
                }
                else
                {
                    cv->SetLinearVelocity(JPH::Vec3(
                        cc.desiredHorizontalVelocity.x, vy,
                        cc.desiredHorizontalVelocity.z));
                }

                JPH::DefaultBroadPhaseLayerFilter bpFilter(*m_impl->objVsBpFilter, Layers::CHARACTER);
                JPH::DefaultObjectLayerFilter    objFilter(*m_impl->objLayerPairFilter, Layers::CHARACTER);
                JPH::BodyFilter                  bodyFilter;   // accept all
                JPH::ShapeFilter                 shapeFilter;  // accept all

                cv->ExtendedUpdate(
                    Impl::kFixedDt,
                    JPH::Vec3::sZero(),         // gravity zero — we already applied it above
                    m_impl->kccExtUpdate,
                    bpFilter, objFilter, bodyFilter, shapeFilter,
                    *m_impl->tempAlloc);

                // Write post-step state back so gameplay can read it next frame.
                cc.wasGrounded = cc.isGrounded;
                const auto gs = cv->GetGroundState();
                cc.isGrounded =
                    (gs == JPH::CharacterBase::EGroundState::OnGround);
                if (cc.isGrounded) cc.timeInAir = 0.f;
                else               cc.timeInAir += Impl::kFixedDt;

                const JPH::Vec3 gn = cv->GetGroundNormal();
                cc.groundNormal = { gn.GetX(), gn.GetY(), gn.GetZ() };

                const JPH::Vec3 lv = cv->GetLinearVelocity();
                cc.velocity = { lv.GetX(), lv.GetY(), lv.GetZ() };

                const JPH::BodyID gbid = cv->GetGroundBodyID();
                if (!gbid.IsInvalid())
                {
                    JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), gbid);
                    cc.groundEntity = lock.Succeeded()
                        ? static_cast<Entity>(lock.GetBody().GetUserData())
                        : NullEntity;
                }
                else cc.groundEntity = NullEntity;

                // ---- Mode update + off-mesh-link landing ---------------
                // LaunchedTraversal latches until the CV touches ground; on
                // landing, flip to Walking AND raise launchFinished so the
                // NavAgent advances the path corner. Otherwise mode follows
                // grounded state directly.
                if (launched)
                {
                    if (cc.isGrounded)
                    {
                        cc.mode = MovementMode::Walking;
                        cc.launchFinished = true;
                    }
                    // else: stay LaunchedTraversal, still in the air
                }
                else
                {
                    cc.mode = cc.isGrounded ? MovementMode::Walking
                                            : MovementMode::Falling;
                }

                // ---- Blocked-this-step report (DesignMd §7 坑 2) --------
                // Compare desired horizontal displacement vs actual XZ
                // displacement. Only fire on a HARD pin (actual < 5% of
                // requested) — small slopes / step-up frames / oblique
                // slide-along-wall all reduce horizontal displacement
                // legitimately, and a tighter threshold (the old 25%)
                // produced false-positive stuck detection that constantly
                // forced replans on uneven terrain.
                const JPH::Vec3 postPos = cv->GetPosition();

                // Render-interpolation snapshot — bracket this step's
                // [prePos, postPos] so PhysicsInterpApplySystem can lerp the
                // capsule exactly like it lerps dynamic rigid bodies. Without
                // it the KCC rendered at the raw 60Hz step position while a
                // pushed box rendered interpolated (~1 step in the past); that
                // phase mismatch is what made pushing feel janky at high FPS.
                // prePos/postPos are captured fresh from the CV each step, so
                // no first-frame glitch and no separate prev←curr shift needed.
                {
                    auto& kccSnap   = m_impl->poseSnapshots[e];
                    kccSnap.prevPos = prePos;
                    kccSnap.currPos = postPos;
                }

                const float dx = postPos.GetX() - prePos.GetX();
                const float dz = postPos.GetZ() - prePos.GetZ();
                const float actualSq  = dx * dx + dz * dz;
                const float desiredX  = cc.desiredHorizontalVelocity.x;
                const float desiredZ  = cc.desiredHorizontalVelocity.z;
                const float desiredSq = (desiredX * desiredX + desiredZ * desiredZ)
                                        * (Impl::kFixedDt * Impl::kFixedDt);
                constexpr float kBlockedThreshold = 0.05f * 0.05f; // 5% of intent
                if (desiredSq > 1e-6f && actualSq < desiredSq * kBlockedThreshold)
                {
                    cc.wasBlockedLastStep = true;
                    // Use -desired as a coarse "blocked surface normal" —
                    // Jolt CharacterBase doesn't expose the wall-contact
                    // normal. Aligned-with-intent check on the consumer
                    // side is the load-bearing part anyway.
                    cc.blockedNormal = { -desiredX, 0.f, -desiredZ };
                    const float bn2 = cc.blockedNormal.x * cc.blockedNormal.x
                                    + cc.blockedNormal.z * cc.blockedNormal.z;
                    if (bn2 > 1e-6f)
                    {
                        const float inv = 1.f / std::sqrt(bn2);
                        cc.blockedNormal.x *= inv;
                        cc.blockedNormal.z *= inv;
                    }
                }
                else
                {
                    cc.wasBlockedLastStep = false;
                    cc.blockedNormal      = { 0.f, 0.f, 0.f };
                }

                #if KCC_DEBUG >= 1
                // ---- MAGIC-MOTION detector (unthrottled, edge-only) ------
                // Fires when the CV moved horizontally despite the caller
                // requesting (near-)zero velocity. Catches:
                //   * penetration recovery pushing the char out of geometry
                //   * moving kinematic platforms dragging the char along
                //   * gravity Y projected onto a slope giving horizontal slide
                //   * SOMETHING else writing to LT.translation between steps
                //     and the next PreAllSteps re-syncing CV (would manifest
                //     as a position jump too).
                {
                    const float prePos_dx = postPos.GetX() - posPre.GetX();
                    const float prePos_dz = postPos.GetZ() - posPre.GetZ();
                    const float prePos_dy = postPos.GetY() - posPre.GetY();
                    const float prePos_dxz_sq = prePos_dx*prePos_dx + prePos_dz*prePos_dz;
                    const float reqSpeed_sq =
                        cc.desiredHorizontalVelocity.x * cc.desiredHorizontalVelocity.x +
                        cc.desiredHorizontalVelocity.z * cc.desiredHorizontalVelocity.z;
                    constexpr float kMotionThreshSq = 0.05f * 0.05f;  // 5cm/step
                    constexpr float kZeroIntentSq   = 0.10f * 0.10f;  // |v|<10cm/s = "no intent"
                    if (prePos_dxz_sq > kMotionThreshSq && reqSpeed_sq < kZeroIntentSq)
                    {
                        LOG_WARNING("[KCC ent=%u] MAGIC MOTION: dXZ=%.3f dY=%+.3f "
                                    "(vDes=(%.3f,_,%.3f)≈0, mode=%d, grounded:%d→%d, groundEnt=%u)  "
                                    "→ pre=(%.3f,%.3f,%.3f) post=(%.3f,%.3f,%.3f)",
                                    static_cast<uint32_t>(e),
                                    std::sqrt(prePos_dxz_sq), prePos_dy,
                                    cc.desiredHorizontalVelocity.x,
                                    cc.desiredHorizontalVelocity.z,
                                    static_cast<int>(cc.mode),
                                    preWasGrounded ? 1 : 0,
                                    cc.isGrounded ? 1 : 0,
                                    static_cast<uint32_t>(cc.groundEntity),
                                    posPre.GetX(), posPre.GetY(), posPre.GetZ(),
                                    postPos.GetX(), postPos.GetY(), postPos.GetZ());
                    }
                }

                // ---- LT-DESYNC detector (unthrottled, edge-only) ---------
                // Compare the CV's pre-step world position against the LT
                // we WROTE last frame. If they differ by > 5cm, someone is
                // writing to LT between PostAllSteps (where we sync LT←CV)
                // and the next StepOnce (where we'd re-sync). That means
                // an external system is teleporting the entity and the CV
                // is *not* tracking it — visual will drift.
                {
                    if (const LocalTransform* ltCheck = world.GetComponent<LocalTransform>(e))
                    {
                        const float ldx = ltCheck->translation.x - posPre.GetX();
                        const float ldy = ltCheck->translation.y - posPre.GetY();
                        const float ldz = ltCheck->translation.z - posPre.GetZ();
                        if (ldx*ldx + ldy*ldy + ldz*ldz > 0.05f * 0.05f)
                        {
                            LOG_WARNING("[KCC ent=%u] LT/CV DESYNC at step start: "
                                        "LT=(%.3f,%.3f,%.3f) CV=(%.3f,%.3f,%.3f) "
                                        "(someone wrote LT between physics frames)",
                                        static_cast<uint32_t>(e),
                                        ltCheck->translation.x, ltCheck->translation.y, ltCheck->translation.z,
                                        posPre.GetX(), posPre.GetY(), posPre.GetZ());
                        }
                    }
                }

                // Per-step throttled snapshot: pre-step intent + post-step
                // realisation. The most informative single line for diagnosing
                // "I asked the agent to walk and it didn't move".
                if (kccLogFire)
                {
                    LOG_INFO("[KCC ent=%u] in: vDes=(%.2f,_,%.2f) vyIn=%.2f preGround=%d mode=%d  "
                             "out: dXZ=%.4f vel=(%.2f,%.2f,%.2f) grounded=%d blocked=%d pos=(%.2f,%.2f,%.2f)",
                             static_cast<uint32_t>(e),
                             cc.desiredHorizontalVelocity.x, cc.desiredHorizontalVelocity.z,
                             lvPre.GetY(),
                             (gsPre == JPH::CharacterBase::EGroundState::OnGround) ? 1 : 0,
                             static_cast<int>(cc.mode),
                             std::sqrt(actualSq),
                             cc.velocity.x, cc.velocity.y, cc.velocity.z,
                             cc.isGrounded ? 1 : 0,
                             cc.wasBlockedLastStep ? 1 : 0,
                             postPos.GetX(), postPos.GetY(), postPos.GetZ());
                }
                // Edge: ground state transition (independent of throttle).
                if (preWasGrounded != cc.isGrounded)
                {
                    LOG_INFO("[KCC ent=%u] ground %s (vel=(%.2f,%.2f,%.2f) timeInAir=%.2f)",
                             static_cast<uint32_t>(e),
                             cc.isGrounded ? "LANDED" : "AIRBORNE",
                             cc.velocity.x, cc.velocity.y, cc.velocity.z,
                             cc.timeInAir);
                }
                // Edge: blocked-this-step rising edge (avoid spamming if user
                // walks into a wall — log only when it FIRST trips).
                static std::unordered_map<Entity, bool> s_kccPrevBlocked;
                bool& prevBlock = s_kccPrevBlocked[e];
                if (!prevBlock && cc.wasBlockedLastStep)
                {
                    LOG_INFO("[KCC ent=%u] BLOCKED: desired=(%.2f,_,%.2f) actualXZ=%.4f normal=(%.2f,_,%.2f)",
                             static_cast<uint32_t>(e),
                             cc.desiredHorizontalVelocity.x, cc.desiredHorizontalVelocity.z,
                             std::sqrt(actualSq),
                             cc.blockedNormal.x, cc.blockedNormal.z);
                }
                prevBlock = cc.wasBlockedLastStep;
                #endif
            }
        }

        m_impl->physics->Update(
            Impl::kFixedDt, Impl::kCollisionSteps,
            m_impl->tempAlloc.get(), m_impl->jobSys.get());

        // AFTER the step: capture new pose as "current".
        for (size_t i = 0; i < n; ++i)
        {
            const RigidBodyComponent& rb = rbData[i];
            if (rb.bodyId == kInvalidPhysicsBodyId) continue;
            if (rb.motion != RigidBodyComponent::Motion::Dynamic) continue;
            auto& snap = m_impl->poseSnapshots[rbEntities[i]];
            const JPH::BodyID id(rb.bodyId);
            snap.currPos = bodyIface.GetPosition(id);
            snap.currRot = bodyIface.GetRotation(id);
        }

    }
}

// ---------------------------------------------------------------------------
// PostAllSteps: drain contact events to EventBus + write Jolt's instantaneous
// pose back into LocalTransform. Runs ONCE per render frame after every
// substep completes.
void PhysicsSystem::PostAllSteps(World& world)
{
    if (!m_impl) return;

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    // Publish contact events accumulated on worker threads during this step.
    // Runs on the main thread so EventBus (which is not thread-safe) is fine.
    m_impl->contactListener->Drain(m_impl->contactScratch);
    for (const auto& c : m_impl->contactScratch)
    {
        if (!world.IsAlive(c.a) || !world.IsAlive(c.b)) continue;
        EventBus::Get().Publish(ContactBeganEvent{
            world.MakeHandle(c.a), world.MakeHandle(c.b), c.point, c.normal });
    }
    m_impl->contactScratch.clear();

    // Phase 4 — write Jolt's instantaneous pose back into LocalTransform.
    // Keeping LT == Jolt-current is what lets Phase 2's teleport detection
    // distinguish "user dragged me in Inspector" (LT diverges from Jolt)
    // from "physics simulated me" (LT == Jolt). Render-side interpolation
    // is applied AFTER TransformSystem::Propagate in ApplyRenderInterpolation,
    // overwriting GlobalTransform — that path doesn't feed back into Jolt.
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId)                 continue;
        if (rb.motion != RigidBodyComponent::Motion::Dynamic)   continue;

        LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        if (!lt) continue;

        const JPH::BodyID id(rb.bodyId);
        lt->translation = FromJolt(bodyIface.GetPosition(id));
        lt->rotation    = FromJolt(bodyIface.GetRotation(id));
    }

    // Phase 4b — write KCC pose back to LocalTransform. Same contract as
    // RigidBody: keep LT in sync with the physics-truth so anything that
    // teleports the entity via LocalTransform next frame is correctly
    // re-seeded into the CV (the lazy-create path treats absent CVs as new
    // entities, so direct teleport requires a separate codepath if needed).
    // Rotation is NOT written — CharacterVirtual doesn't rotate the capsule
    // and animation drivers own the entity's facing.
    if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
    {
        const auto& ccEntities = ccPool->Entities();
        const size_t ncc = ccPool->Data().size();

        #if KCC_DEBUG >= 1
        static int s_writebackCounter = 0;
        ++s_writebackCounter;
        const bool writebackLogFire = (s_writebackCounter % 60) == 0;
        #endif

        for (size_t i = 0; i < ncc; ++i)
        {
            const Entity e = ccEntities[i];
            auto cvIt = m_impl->kccTable.find(e);
            if (cvIt == m_impl->kccTable.end()) {
                #if KCC_DEBUG >= 1
                if (writebackLogFire)
                    LOG_WARNING("[KCC ent=%u] PostAllSteps writeback: NO CV in kccTable",
                                static_cast<uint32_t>(e));
                #endif
                continue;
            }

            LocalTransform* lt = world.GetComponent<LocalTransform>(e);
            if (!lt) {
                #if KCC_DEBUG >= 1
                if (writebackLogFire)
                    LOG_WARNING("[KCC ent=%u] PostAllSteps writeback: NO LocalTransform",
                                static_cast<uint32_t>(e));
                #endif
                continue;
            }

            #if KCC_DEBUG >= 1
            const DirectX::XMFLOAT3 ltBefore = lt->translation;
            #endif

            const JPH::Vec3 p = cvIt->second->GetPosition();
            lt->translation = { p.GetX(), p.GetY(), p.GetZ() };

            #if KCC_DEBUG >= 1
            if (writebackLogFire)
            {
                LOG_INFO("[KCC ent=%u] PostAllSteps writeback: LT %.3f,%.3f,%.3f → %.3f,%.3f,%.3f",
                         static_cast<uint32_t>(e),
                         ltBefore.x, ltBefore.y, ltBefore.z,
                         lt->translation.x, lt->translation.y, lt->translation.z);
            }
            #endif
        }
    }
}

void PhysicsSystem::ApplyRenderInterpolation(World& world, float renderAlpha)
{
    if (!m_impl) return;
    // renderAlpha is the CALLER's accumulator / fixedDt — the scheduler
    // populates FrameContext::physicsAlpha for this. Clamp defensively so
    // a stale value can't slerp out of the [prev, curr] bracket.
    const float alpha = std::clamp(renderAlpha, 0.0f, 1.0f);

    auto* rbPool = world.GetPool<RigidBodyComponent>();
    if (!rbPool) return;
    auto& rbEntities = rbPool->Entities();
    auto& rbData     = rbPool->Data();
    const size_t n   = rbData.size();

    for (size_t i = 0; i < n; ++i)
    {
        const RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId)                 continue;
        if (rb.motion != RigidBodyComponent::Motion::Dynamic)   continue;

        auto snapIt = m_impl->poseSnapshots.find(rbEntities[i]);
        if (snapIt == m_impl->poseSnapshots.end()) continue;
        const auto& snap = snapIt->second;

        // Interpolated WORLD pose (Jolt operates in world space).
        const JPH::Vec3 interpPos = snap.prevPos + (snap.currPos - snap.prevPos) * alpha;
        const JPH::Quat interpRot = snap.prevRot.SLERP(snap.currRot, alpha);

        GlobalTransform* gt = world.GetComponent<GlobalTransform>(rbEntities[i]);
        if (!gt) continue;

        // Jolt doesn't carry scale; preserve whatever TransformSystem just
        // composed from LocalTransform.scale.
        const LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        const DirectX::XMFLOAT3 scl =
            lt ? lt->scale : DirectX::XMFLOAT3{ 1.f, 1.f, 1.f };

        using namespace DirectX;
        const XMFLOAT4 rotF = FromJolt(interpRot);
        const XMFLOAT3 posF = FromJolt(interpPos);
        const XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scl));
        const XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rotF));
        const XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&posF));
        XMStoreFloat4x4(&gt->matrix, S * R * T);
    }

    // ---- CharacterController (KCC) — interpolate POSITION only -------------
    // The capsule's facing + scale live in LocalTransform (animation / AI own
    // rotation; the KCC never rotates the capsule), and TransformSystem just
    // composed those into gt->matrix. We replace ONLY the translation row with
    // the interpolated physics position, so the character renders with the same
    // ~1-step latency as dynamic bodies — eliminating the phase mismatch that
    // made pushing a (interpolated) box with the (previously un-interpolated)
    // character look janky. Position lerp only — rotation isn't physics-driven.
    if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
    {
        const auto& ccEnts = ccPool->Entities();
        const size_t ncc   = ccPool->Data().size();
        for (size_t i = 0; i < ncc; ++i)
        {
            const Entity e = ccEnts[i];
            auto snapIt = m_impl->poseSnapshots.find(e);
            if (snapIt == m_impl->poseSnapshots.end()) continue;
            GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;

            const auto& snap = snapIt->second;
            const JPH::Vec3 ip = snap.prevPos + (snap.currPos - snap.prevPos) * alpha;
            // Overwrite only the translation row (row-major _4x); keep the
            // rotation/scale TransformSystem composed from LocalTransform.
            gt->matrix._41 = ip.GetX();
            gt->matrix._42 = ip.GetY();
            gt->matrix._43 = ip.GetZ();
        }
    }
}

} // namespace DX12Physics
