// PhysicsSystem implementation. All Jolt-specific types are confined to this
// translation unit; the public header exposes only a pImpl handle.
//
// Object/broadphase layer setup follows Jolt's HelloWorld sample: two object
// layers (NON_MOVING / MOVING) mapped 1:1 onto two broadphase layers, with
// the canonical filter matrix (static ↔ dynamic only; dynamic ↔ everything).

#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsEvents.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "System/Log.h"
#include "System/EventBus.h"

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
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/EActivation.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace DX12Physics
{
// ---------------------------------------------------------------------------
// Layer definitions
// ---------------------------------------------------------------------------
namespace Layers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace BPLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING{ 0 };
    static constexpr JPH::BroadPhaseLayer MOVING{ 1 };
    static constexpr JPH::uint            NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        m_map[Layers::NON_MOVING] = BPLayers::NON_MOVING;
        m_map[Layers::MOVING]     = BPLayers::MOVING;
    }
    JPH::uint            GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_map[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override
    {
        return (l == BPLayers::MOVING) ? "MOVING" : "NON_MOVING";
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
        if (a == Layers::NON_MOVING) return b == BPLayers::MOVING;
        if (a == Layers::MOVING)     return true;
        return false;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        if (a == Layers::MOVING)     return true;
        return false;
    }
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
};

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

    LOG_INFO("PhysicsSystem: initialized (Jolt %d.%d.%d, %d worker threads)",
             JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH, workerCount);
}

void PhysicsSystem::Shutdown()
{
    if (!m_impl) return;

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

static JPH::ShapeRefC MakeShape(const ColliderComponent& c)
{
    using namespace JPH;
    // ShapeSettings is preferred long-term for validation, but for primitive
    // shapes the direct constructors are equivalent and simpler here.
    switch (c.shape)
    {
        case ColliderComponent::Shape::Box:
            return new BoxShape(ToJolt(c.halfExtents));
        case ColliderComponent::Shape::Sphere:
            return new SphereShape(c.radius);
        case ColliderComponent::Shape::Capsule:
            return new CapsuleShape(c.halfHeight, c.radius);
    }
    return new BoxShape(Vec3::sReplicate(0.5f));
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
void PhysicsSystem::Update(World& world, float dt)
{
    if (!m_impl) return;

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();

    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    // Phase 1 — lazily create Jolt bodies for entities that don't have one yet.
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId != kInvalidPhysicsBodyId) continue;

        const Entity e = rbEntities[i];
        const ColliderComponent* col = world.GetComponent<ColliderComponent>(e);
        if (!col) continue;

        const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        const JPH::Vec3 pos = lt ? ToJolt(lt->translation) : JPH::Vec3::sZero();
        const JPH::Quat rot = lt ? ToJoltQuat(lt->rotation) : JPH::Quat::sIdentity();

        const JPH::EMotionType  mt    = ToJolt(rb.motion);
        const JPH::ObjectLayer  layer = (mt == JPH::EMotionType::Static) ? Layers::NON_MOVING : Layers::MOVING;

        JPH::BodyCreationSettings settings(MakeShape(*col), pos, rot, mt, layer);
        settings.mLinearDamping  = rb.linearDamping;
        settings.mAngularDamping = rb.angularDamping;
        settings.mFriction       = rb.friction;
        settings.mRestitution    = rb.restitution;
        settings.mGravityFactor  = rb.gravityFactor;
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
        LOG_INFO("PhysicsSystem: created body %u for entity %u (motion=%d)",
                 id.GetIndexAndSequenceNumber(), e, (int)rb.motion);
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
                (targetMt == JPH::EMotionType::Static) ? Layers::NON_MOVING
                                                      : Layers::MOVING;
            bodyIface.SetObjectLayer(id, newLayer);
        }

        const LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        if (!lt) continue;

        const JPH::Vec3 ltPos = ToJolt(lt->translation);
        const JPH::Quat ltRot = ToJoltQuat(lt->rotation);

        if (rb.motion != RigidBodyComponent::Motion::Dynamic)
        {
            // Static / Kinematic: LocalTransform is authoritative every frame.
            bodyIface.SetPositionAndRotation(id, ltPos, ltRot, JPH::EActivation::Activate);
        }
        else
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
            }
        }
    }

    // Phase 3 — advance the simulation at fixed 60Hz with a clamped accumulator.
    m_impl->accumulator += dt;
    constexpr float kMaxAccum = 0.25f; // spiral-of-death guard (15 fixed steps)
    if (m_impl->accumulator > kMaxAccum) m_impl->accumulator = kMaxAccum;

    while (m_impl->accumulator >= Impl::kFixedDt)
    {
        m_impl->physics->Update(
            Impl::kFixedDt, Impl::kCollisionSteps,
            m_impl->tempAlloc.get(), m_impl->jobSys.get());
        m_impl->accumulator -= Impl::kFixedDt;
    }

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

    // Phase 4 — write simulated transforms back into LocalTransform for dynamic
    // bodies only. Static/Kinematic are user-driven (Phase 2 already pushed),
    // so pulling would clobber edits the user is still making in the Inspector.
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
}

} // namespace DX12Physics
