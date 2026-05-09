#include "ECS/DecalSpawner.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/DecalMaterialAsset.h"

using namespace DirectX;

// ---------------------------------------------------------------------------
void DecalSpawner::InitPool(World& world, uint32_t capacity)
{
    m_slots.clear();
    m_slots.reserve(capacity);
    m_cursor = 0;
    for (uint32_t i = 0; i < capacity; ++i)
    {
        Entity e = world.CreateEntity();
        // Identity transforms — Spawn fills in the real values on use.
        world.AddComponent<LocalTransform>(e, LocalTransform{});
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        m_slots.push_back(e);
    }
}

// ---------------------------------------------------------------------------
void DecalSpawner::OnWorldClear()
{
    // Entity IDs are recycled by World::CreateEntity after World::Clear()
    // destroys the world, so stale IDs here would collide. Drop them.
    m_slots.clear();
    m_cursor = 0;
}

// ---------------------------------------------------------------------------
Entity DecalSpawner::AcquireSlot(World& world)
{
    if (m_slots.empty())
    {
        // Fresh mode — one entity per spawn.
        Entity e = world.CreateEntity();
        world.AddComponent<LocalTransform>(e, LocalTransform{});
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        return e;
    }
    // Pool mode — round-robin. When the chosen slot still has an active
    // DecalComponent, overwriting it is the simplest recycling strategy
    // (AddComponent<T> replaces existing data). Visual artefact: the oldest
    // decal pops off when the pool fills — tune capacity to avoid this.
    Entity e = m_slots[m_cursor];
    m_cursor = (m_cursor + 1) % static_cast<uint32_t>(m_slots.size());

    // Heal a stale slot — e.g. world was Clear()ed without Renderer::OnWorldClear,
    // or the user destroyed a pool entity manually. Reallocate the slot so
    // the caller always gets a live entity.
    if (!world.IsAlive(e))
    {
        e = world.CreateEntity();
        world.AddComponent<LocalTransform>(e, LocalTransform{});
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        m_slots[(m_cursor + m_slots.size() - 1) % m_slots.size()] = e;
    }
    return e;
}

// ---------------------------------------------------------------------------
Entity DecalSpawner::Spawn(World&                                           world,
                            std::shared_ptr<Resource::DecalMaterialAsset>   material,
                            const XMFLOAT3&                                  position,
                            const XMFLOAT4&                                  rotationQuat,
                            const XMFLOAT3&                                  scale,
                            float                                             lifetime,
                            float                                             fadeOutDuration)
{
    if (!material) return NullEntity;

    Entity e = AcquireSlot(world);

    // Write LocalTransform (TransformSystem will recompute GlobalTransform on
    // the next Propagate) + GlobalTransform directly (so the decal is valid
    // THIS frame if spawned after Propagate already ran).
    LocalTransform lt;
    lt.translation = position;
    lt.rotation    = rotationQuat;
    lt.scale       = scale;
    world.AddComponent<LocalTransform>(e, lt);

    GlobalTransform gt;
    XMStoreFloat4x4(&gt.matrix, lt.ToMatrix());
    world.AddComponent<GlobalTransform>(e, gt);

    DecalComponent dc;
    dc.material = std::move(material);
    dc.lifetime = lifetime;
    dc.fadeOutDuration = fadeOutDuration;
    dc.fadeAlpha = 1.0f;
    // Pool slots must NOT self-destruct — the spawner needs the entity to
    // survive for future reuse. Fresh-mode entities can self-destruct.
    dc.destroyEntityOnExpire = m_slots.empty();
    world.AddComponent<DecalComponent>(e, std::move(dc));
    return e;
}

// ---------------------------------------------------------------------------
Entity DecalSpawner::SpawnAtSurface(World&                                         world,
                                    std::shared_ptr<Resource::DecalMaterialAsset>  material,
                                    const XMFLOAT3&                                 hitPos,
                                    const XMFLOAT3&                                 surfaceNormal,
                                    const XMFLOAT2&                                 size,
                                    float                                            depth,
                                    float                                            lifetime,
                                    float                                            fadeOutDuration,
                                    float                                            rollZ)
{
    // Build an orthonormal basis where local +Z = -surfaceNormal. That puts
    // the decal's projection axis looking INTO the surface, which is the
    // angle the Apply CS uses for its angleFade dot product.
    const XMVECTOR N = XMVector3Normalize(XMLoadFloat3(&surfaceNormal));
    XMVECTOR Z = XMVectorNegate(N);

    // Pick an "up hint" not parallel to Z to seed the cross product.
    const XMVECTOR zAbs = XMVectorAbs(Z);
    const float    zAx  = XMVectorGetX(zAbs);
    const float    zAy  = XMVectorGetY(zAbs);
    const float    zAz  = XMVectorGetZ(zAbs);
    XMVECTOR upHint = (zAy > zAx && zAy > zAz)
        ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR X = XMVector3Normalize(XMVector3Cross(upHint, Z));
    XMVECTOR Y = XMVector3Cross(Z, X);

    // Apply roll around projection axis for per-hit variation.
    if (rollZ != 0.0f)
    {
        const XMMATRIX roll = XMMatrixRotationAxis(Z, rollZ);
        X = XMVector3TransformNormal(X, roll);
        Y = XMVector3TransformNormal(Y, roll);
    }

    // Convert the XYZ basis to a quaternion. XMQuaternionRotationMatrix wants
    // a rotation matrix whose rows are the basis vectors — which is exactly
    // {X, Y, Z} for row-vector convention.
    XMMATRIX rotMat{};
    rotMat.r[0] = X;
    rotMat.r[1] = Y;
    rotMat.r[2] = Z;
    rotMat.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR quat = XMQuaternionRotationMatrix(rotMat);

    XMFLOAT4 rot;
    XMStoreFloat4(&rot, quat);

    return Spawn(world, std::move(material), hitPos, rot,
                 XMFLOAT3{ size.x, size.y, depth }, lifetime, fadeOutDuration);
}

// ---------------------------------------------------------------------------
uint32_t DecalSpawner::GetActiveCount(const World& world) const
{
    uint32_t active = 0;
    for (Entity e : m_slots)
        if (world.GetComponent<DecalComponent>(e)) ++active;
    return active;
}
