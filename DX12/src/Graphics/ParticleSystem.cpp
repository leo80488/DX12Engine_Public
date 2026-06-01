#include "Graphics/ParticleSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "ECS/ECS.h"
#include "ECS/ParticleComponent.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

void ParticleSystem::OnEntityDestroyed(IGraphicsDevice& gfx, Entity e)
{
    auto it = m_emitterTexCache.find(e);
    if (it == m_emitterTexCache.end()) return;
    if (m_texSys && it->second.handle != Resource::kInvalidTextureHandle)
        m_texSys->Release(it->second.handle, gfx);
    m_emitterTexCache.erase(it);
}

void ParticleSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Main particle pool (DEFAULT heap, SRV + UAV) ----------------------
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxGlobalParticles) * sizeof(ParticleGPU);
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(ParticleGPU);
        if (!gfx.CreateBuffer(d, m_particlePool))
        {
            LOG_ERROR("ParticleSystem: particle pool creation failed");
            return;
        }
        m_poolSrvHandle = gfx.GetBufferSRVGpuHandle(m_particlePool);
        m_poolUavHandle = gfx.GetBufferUAVGpuHandle(m_particlePool);
    }

    // ---- Emitter upload buffer (UPLOAD heap, root CBV per dispatch) --------
    // Stride is 256 bytes so emit dispatches can bind root CBVs at
    // `emitterIdx * kEmitterSlotStride` — D3D12 requires 256B alignment for
    // CBV offsets. Real emitter data (112B) lives in the first portion of
    // each slot; the rest is zero padding.
    // Triple-buffered ring — written every frame by CollectEmitters.
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxEmittersPerFrame) * kEmitterSlotStride;
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_emitterBuffer[i]))
                m_emitterMapped[i] = gfx.MapBuffer(m_emitterBuffer[i]);
            if (!m_emitterMapped[i])
                LOG_ERROR("ParticleSystem: emitter buffer map failed (slot %u)", i);
        }
    }

    m_emitterSpawnCounts.reserve(kMaxEmittersPerFrame);

    // ---- Per-frame system CB (UPLOAD heap, root CBV) — triple-buffered -----
    if (!m_systemCB.Create(gfx, "ParticleSystem.CB"))
        LOG_ERROR("ParticleSystem: system CB create failed");

    // Initialize the pool slots to "dead" (lifetime = 0). The DEFAULT heap
    // starts zeroed by the driver on most IHVs, but be explicit: zero the
    // first update produces a "clean" pool without stale data.
    // (no-op — DEFAULT heap comes zeroed; UpdatePass's kill condition is
    // lifetime <= 0, and a zero-initialised ParticleGPU satisfies that.)

    LOG_SUCCESS("ParticleSystem: initialised (%u slots × %zu B = %zu KB)",
                kMaxGlobalParticles, sizeof(ParticleGPU),
                (kMaxGlobalParticles * sizeof(ParticleGPU)) / 1024);
}

void ParticleSystem::Shutdown(IGraphicsDevice& gfx)
{
    m_systemCB.Destroy(gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_emitterMapped[i]) { gfx.UnmapBuffer(m_emitterBuffer[i]); m_emitterMapped[i] = nullptr; }
        if (m_emitterBuffer[i].IsValid()) gfx.DestroyBuffer(m_emitterBuffer[i]);
    }
    if (m_particlePool.IsValid()) gfx.DestroyBuffer(m_particlePool);
    m_gfx = nullptr;
}

void ParticleSystem::UpdateSystemCB(float dt, uint32_t frameIndex)
{
    if (!m_gfx) return;
    auto* p = m_systemCB.Current(*m_gfx);
    if (!p) return;
    ParticleSystemCB cb{};
    cb.deltaTime     = dt;
    cb.particleCount = kMaxGlobalParticles;
    cb.frameIndex    = frameIndex;
    cb.globalGravity = m_globalGravity;
    *p = cb;
}

const RHI::GPUBuffer& ParticleSystem::GetEmitterBuffer() const
{
    const uint32_t s = m_gfx ? m_gfx->GetFrameIndex() : 0;
    return m_emitterBuffer[s < kFrameCount ? s : 0];
}

const RHI::GPUBuffer& ParticleSystem::GetSystemCB() const
{
    // FrameCB::CurrentBuffer needs a non-const gfx ref (matches the IGraphicsDevice
    // accessor signature), so const_cast through m_gfx — the operation is
    // logically const (read-only buffer ref selection).
    static const RHI::GPUBuffer s_empty{};
    if (!m_gfx) return s_empty;
    return m_systemCB.CurrentBuffer(*m_gfx);
}

void ParticleSystem::CollectEmitters(World& world, float dt, uint32_t frameIndex)
{
    m_emitterCountThisFrame = 0;
    m_emitterSpawnCounts.clear();
    m_globalGravity = { 0.0f, 0.0f, 0.0f };
    m_pendingMeshResolves.clear();
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot >= kFrameCount || !m_emitterMapped[frameSlot]) return;
    void* emitterMapped = m_emitterMapped[frameSlot];

    // Iterate ParticleEmitterComponent pool directly (the hot-path
    // pattern — see memory/feedback_ecs_pool_iteration.md).
    auto* pEmit   = world.GetPool<ParticleEmitterComponent>();
    auto* pGlobal = world.GetPool<GlobalTransform>();
    if (!pEmit) return;

    auto& ents = pEmit->Entities();
    auto& data = pEmit->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n && m_emitterCountThisFrame < kMaxEmittersPerFrame; ++i)
    {
        const Entity e = ents[i];
        ParticleEmitterComponent& ec = data[i];
        if (!ec.enabled) continue;
        if (ec.spawnRate <= 0.0f) continue;

        // Determine spawn count this frame via accumulator. Cap at a sane
        // per-emitter-per-frame budget so a misconfigured spawnRate can't
        // overrun kMaxGlobalParticles in a single frame.
        ec.spawnAccumulator += ec.spawnRate * dt;
        uint32_t spawnCount = static_cast<uint32_t>(ec.spawnAccumulator);
        ec.spawnAccumulator -= static_cast<float>(spawnCount);
        constexpr uint32_t kMaxSpawnPerEmitterPerFrame = 256;
        if (spawnCount > kMaxSpawnPerEmitterPerFrame)
            spawnCount = kMaxSpawnPerEmitterPerFrame;
        if (spawnCount == 0) continue;

        // Resolve world position from GlobalTransform (row-major layout;
        // translation in the 4th row, columns 0..2).
        DirectX::XMFLOAT3 worldPos = { 0, 0, 0 };
        if (pGlobal)
        {
            if (const GlobalTransform* gt = pGlobal->Get(e))
            {
                worldPos.x = gt->matrix._41;
                worldPos.y = gt->matrix._42;
                worldPos.z = gt->matrix._43;
            }
        }

        // Texture path → bindless idx resolution. Mirrors SyncMaterialTextures
        // pattern: cache per entity, Release old on path change, Acquire new,
        // promote to bindless idx once the async load is ready.
        if (m_texSys && m_resMgr)
        {
            auto [cacheIt, _inserted] = m_emitterTexCache.try_emplace(e);
            auto& entry = cacheIt->second;

            if (ec.texturePath != entry.path)
            {
                if (entry.handle != Resource::kInvalidTextureHandle && m_gfx)
                    m_texSys->Release(entry.handle, *m_gfx);

                entry.path = ec.texturePath;
                entry.handle = ec.texturePath.empty()
                    ? Resource::kInvalidTextureHandle
                    : m_texSys->Acquire(ec.texturePath, *m_resMgr, *m_gfx);
                ec.textureBindlessIdx = -1;
                ec.textureGpuHandle   = 0;
            }

            if (ec.textureBindlessIdx < 0
                && entry.handle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(entry.handle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                {
                    ec.textureBindlessIdx = static_cast<int32_t>(tex->handle_id);
                    if (m_gfx) ec.textureGpuHandle = m_gfx->GetTextureSRVGpuHandle(*tex);
                }
            }
        }

        // Write the emitter record into mapped upload buffer at a
        // 256B-aligned slot offset, zero the padding bytes afterward so the
        // GPU reads deterministic data.
        uint8_t* slotBytes = static_cast<uint8_t*>(emitterMapped)
                           + m_emitterCountThisFrame * kEmitterSlotStride;
        std::memset(slotBytes, 0, kEmitterSlotStride);
        ParticleEmitterGPU* slot = reinterpret_cast<ParticleEmitterGPU*>(slotBytes);

        slot->position           = worldPos;
        slot->startLifetime      = ec.startLifetime;
        slot->velocityMin        = ec.velocityMin;
        slot->startSize          = ec.startSize;
        slot->velocityMax        = ec.velocityMax;
        slot->spawnCount         = spawnCount;
        slot->startColor         = ec.startColor;
        slot->endColor           = ec.endColor;
        slot->gravity            = ec.gravity;
        slot->writeCursor        = m_writeCursor;
        // Simple deterministic seed: entity × frame. Improves per-spawn
        // variance without a GPU PRNG dependency.
        slot->randomSeed         = frameIndex * 2654435761u + static_cast<uint32_t>(e);
        slot->shapeType          = static_cast<uint32_t>(ec.shape);
        slot->visualMode         = static_cast<uint32_t>(ec.visualMode);
        slot->textureBindlessIdx = (ec.textureBindlessIdx < 0)
                                   ? 0xFFFFFFFFu
                                   : static_cast<uint32_t>(ec.textureBindlessIdx);

        // Per-shape parameter packing — see shaders/ParticleEmit.cs.hlsl for
        // the matching unpacking convention.
        slot->shapeParam0 = { 0, 0, 0, 0 };
        slot->shapeParam1 = { 0, 0, 0, 0 };
        slot->shapeParam2 = { 0, 0, 0, 0 };
        slot->shapeParam3 = { 0, 0, 0, 0 };
        switch (ec.shape)
        {
        case ParticleShape::Sphere:
            slot->shapeParam0 = { ec.sphereRadius,
                                  ec.sphereSpawnOnShell ? 1.0f : 0.0f,
                                  0.0f, 0.0f };
            break;
        case ParticleShape::Cone:
            slot->shapeParam0 = { 0.0f, ec.coneHalfAngle, 0.0f, 0.0f };
            slot->shapeParam1 = { ec.coneDirection.x,
                                  ec.coneDirection.y,
                                  ec.coneDirection.z,
                                  ec.coneLength };
            break;
        case ParticleShape::Box:
            slot->shapeParam0 = { ec.boxHalfExtents.x,
                                  ec.boxHalfExtents.y,
                                  ec.boxHalfExtents.z,
                                  0.0f };
            break;
        case ParticleShape::Circle:
            slot->shapeParam0 = { ec.circleRadius, 0.0f, 0.0f, 0.0f };
            slot->shapeParam1 = { ec.circleNormal.x,
                                  ec.circleNormal.y,
                                  ec.circleNormal.z,
                                  0.0f };
            break;
        case ParticleShape::Mesh:
            // Mesh info (worldMatrix + descriptor slot + indexCount) is
            // patched later by Renderer::ResolveParticleMeshEmitters after
            // this function returns.
            m_pendingMeshResolves.push_back(
                { m_emitterCountThisFrame, ec.meshSourceEntity });
            break;
        default: // Point — no params
            break;
        }

        // Default mesh fields in case patch doesn't run (e.g. missing source).
        DirectX::XMStoreFloat4x4(&slot->meshWorldMatrix, DirectX::XMMatrixIdentity());
        slot->meshDescSlot   = 0xFFFFFFFFu;
        slot->meshIndexCount = 0;
        slot->_padA = slot->_padB = 0;

        // Advance the global ring cursor by this emitter's spawn count.
        m_writeCursor = (m_writeCursor + spawnCount) % kMaxGlobalParticles;

        // Snapshot spawn count + first emitter's gravity for passes.
        m_emitterSpawnCounts.push_back(spawnCount);
        if (m_emitterCountThisFrame == 0)
            m_globalGravity = ec.gravity;

        ++m_emitterCountThisFrame;
    }
}

void ParticleSystem::PatchMeshEmitter(uint32_t slotIndex,
                                       uint32_t meshDescSlot,
                                       uint32_t meshIndexCount,
                                       const DirectX::XMFLOAT4X4& worldMatrix)
{
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot >= kFrameCount || !m_emitterMapped[frameSlot]) return;
    if (slotIndex >= m_emitterCountThisFrame) return;

    ParticleEmitterGPU* slot = reinterpret_cast<ParticleEmitterGPU*>(
        static_cast<uint8_t*>(m_emitterMapped[frameSlot])
        + slotIndex * kEmitterSlotStride);

    slot->meshDescSlot    = meshDescSlot;
    slot->meshIndexCount  = meshIndexCount;
    slot->meshWorldMatrix = worldMatrix;
}
