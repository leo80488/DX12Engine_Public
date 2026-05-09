#include "Audio/Audio3DSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioComponents.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "ECS/Components.h"            // CameraComponent

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

namespace Audio
{

namespace
{

// Helper: extract world-space translation (column 3) from a row-major
// world matrix. The engine convention (per HierarchyComponents.h) is
// row-vector / row-major, so translation lives in row 3.
DirectX::XMFLOAT3 WorldPositionFromMatrix(const DirectX::XMFLOAT4X4& m)
{
    return { m._41, m._42, m._43 };
}

// Build a left-handed forward vector from yaw/pitch — matches CameraSystem's
// convention. Used as the listener orientation when the listener is the
// active camera (no GlobalTransform yet, we rely on CameraComponent).
DirectX::XMFLOAT3 ForwardFromYawPitch(float yaw, float pitch)
{
    using namespace DirectX;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw),   sy = sinf(yaw);
    return { cp * sy, sp, cp * cy };
}

} // anonymous

void Audio3DSystem::Update(World& world, float dt)
{
    if (!m_engine) return;

    // ------------------------------------------------------------------------
    // 1. Resolve listener pose. Priority order:
    //    a. Entity flagged AudioListenerComponent + GlobalTransform → use that.
    //    b. CameraComponent + AudioListenerComponent → derive from yaw/pitch.
    //    c. Plain CameraComponent (no listener tag) → fallback so 3D works
    //       out of the box on scenes that didn't tag the camera yet.
    // ------------------------------------------------------------------------
    DirectX::XMFLOAT3 listenerPos { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 listenerFwd { 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 listenerUp  { 0.f, 1.f, 0.f };
    bool              haveListener = false;

    world.ForEach<AudioListenerComponent>(
        [&](Entity e, AudioListenerComponent& lc)
        {
            if (haveListener || !lc.active) return;

            if (auto* gt = world.GetComponent<GlobalTransform>(e))
            {
                listenerPos = WorldPositionFromMatrix(gt->matrix);
                // Forward = +Z row of world matrix (row-vector convention).
                listenerFwd = { gt->matrix._31, gt->matrix._32, gt->matrix._33 };
                listenerUp  = { gt->matrix._21, gt->matrix._22, gt->matrix._23 };
                haveListener = true;
                return;
            }
            if (auto* cam = world.GetComponent<CameraComponent>(e))
            {
                listenerPos = cam->position;
                listenerFwd = ForwardFromYawPitch(cam->yaw, cam->pitch);
                listenerUp  = { 0.f, 1.f, 0.f };
                haveListener = true;
            }
        });

    if (!haveListener)
    {
        // Fallback: any camera in the world acts as the listener.
        world.ForEach<CameraComponent>(
            [&](Entity, CameraComponent& cam)
            {
                if (haveListener) return;
                listenerPos = cam.position;
                listenerFwd = ForwardFromYawPitch(cam.yaw, cam.pitch);
                listenerUp  = { 0.f, 1.f, 0.f };
                haveListener = true;
            });
    }

    if (!haveListener) return;

    X3DAUDIO_LISTENER listener{};
    listener.Position    = { listenerPos.x, listenerPos.y, listenerPos.z };
    listener.OrientFront = { listenerFwd.x, listenerFwd.y, listenerFwd.z };
    listener.OrientTop   = { listenerUp.x,  listenerUp.y,  listenerUp.z  };

    if (m_listenerCached && dt > 0.f)
    {
        listener.Velocity = {
            (listenerPos.x - m_listenerPrevPos.x) / dt,
            (listenerPos.y - m_listenerPrevPos.y) / dt,
            (listenerPos.z - m_listenerPrevPos.z) / dt };
    }
    m_listenerPrevPos = listenerPos;
    m_listenerCached  = true;

    m_engine->SetListener(listener);

    // ------------------------------------------------------------------------
    // 2. Walk every 3D AudioSource that's currently playing and update its DSP.
    // ------------------------------------------------------------------------
    auto* pool = world.GetPool<AudioSourceComponent>();
    if (!pool) return;
    auto& ents = pool->Entities();
    auto& data = pool->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = ents[i];
        const AudioSourceComponent& src = data[i];
        if (!src.is3D) continue;
        if (src.state != PlayState::Playing) continue;
        if (!src.activeVoice.IsValid())      continue;

        DirectX::XMFLOAT3 pos { 0.f, 0.f, 0.f };
        if (auto* gt = world.GetComponent<GlobalTransform>(e))
            pos = WorldPositionFromMatrix(gt->matrix);

        DirectX::XMFLOAT3 vel { 0.f, 0.f, 0.f };
        EmitterCache& cache = m_emitterCache[e];
        if (cache.valid && dt > 0.f)
        {
            vel = { (pos.x - cache.position.x) / dt,
                    (pos.y - cache.position.y) / dt,
                    (pos.z - cache.position.z) / dt };
        }
        cache.position = pos;
        cache.valid    = true;

        X3DAUDIO_EMITTER emitter{};
        emitter.Position             = { pos.x, pos.y, pos.z };
        emitter.Velocity             = { vel.x, vel.y, vel.z };
        emitter.OrientFront          = { 0.f, 0.f, 1.f };
        emitter.OrientTop            = { 0.f, 1.f, 0.f };
        emitter.ChannelCount         = 1;       // overridden inside Apply3D for stereo clips
        emitter.InnerRadius          = src.minDistance;
        emitter.InnerRadiusAngle     = X3DAUDIO_PI / 4.f;
        emitter.CurveDistanceScaler  = src.maxDistance > 0.f ? src.maxDistance : 1.f;
        emitter.DopplerScaler        = 1.f;

        m_engine->Apply3D(src.activeVoice, emitter);
    }

    // Drop cache entries for entities that no longer have AudioSourceComponent.
    // Cheap hygiene — keeps the map from growing without bound when sources
    // come and go (e.g. spawned projectile sounds).
    if (m_emitterCache.size() > n + 32)
    {
        std::erase_if(m_emitterCache, [&](const auto& kv){
            return !world.HasComponent<AudioSourceComponent>(kv.first);
        });
    }
}

} // namespace Audio
