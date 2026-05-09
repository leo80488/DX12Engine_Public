#include "Graphics/TracerSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

void TracerSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Main tracer pool (DEFAULT heap, SRV + UAV) -----------------------
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxTracers) * sizeof(TracerGPU);
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TracerGPU);
        if (!gfx.CreateBuffer(d, m_pool))
        {
            LOG_ERROR("TracerSystem: pool buffer creation failed");
            return;
        }
        m_poolSrv = gfx.GetBufferSRVGpuHandle(m_pool);
        m_poolUav = gfx.GetBufferUAVGpuHandle(m_pool);
    }

    // ---- Spawn upload buffer (UPLOAD heap, SR-bound StructuredBuffer) -----
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxSpawnsPerFrame) * sizeof(TracerSpawnGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TracerSpawnGPU);
        if (gfx.CreateBuffer(d, m_spawnBuffer))
        {
            m_spawnMapped = gfx.MapBuffer(m_spawnBuffer);
            m_spawnSrv    = gfx.GetBufferSRVGpuHandle(m_spawnBuffer);
        }
        if (!m_spawnMapped)
            LOG_ERROR("TracerSystem: spawn buffer map failed");
    }

    // ---- System CB (UPLOAD heap, root CBV) --------------------------------
    {
        RHI::GPUBufferDesc d{};
        d.size       = 256;
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(d, m_systemCB))
            m_systemCBMapped = gfx.MapBuffer(m_systemCB);
        if (!m_systemCBMapped)
            LOG_ERROR("TracerSystem: system CB map failed");
    }

    m_pendingSpawns.reserve(kMaxSpawnsPerFrame);

    LOG_SUCCESS("TracerSystem: initialised (%u slots * %zu B = %zu KB)",
                kMaxTracers, sizeof(TracerGPU),
                (kMaxTracers * sizeof(TracerGPU)) / 1024);
}

void TracerSystem::Shutdown(IGraphicsDevice& gfx)
{
    if (m_systemCBMapped)  { gfx.UnmapBuffer(m_systemCB);   m_systemCBMapped = nullptr; }
    if (m_spawnMapped)     { gfx.UnmapBuffer(m_spawnBuffer); m_spawnMapped   = nullptr; }
    if (m_systemCB.IsValid())   gfx.DestroyBuffer(m_systemCB);
    if (m_spawnBuffer.IsValid())gfx.DestroyBuffer(m_spawnBuffer);
    if (m_pool.IsValid())       gfx.DestroyBuffer(m_pool);
    m_gfx = nullptr;
}

void TracerSystem::Spawn(const DirectX::XMFLOAT3& start,
                         const DirectX::XMFLOAT3& end,
                         const DirectX::XMFLOAT4& color,
                         float                    width,
                         float                    lifetime,
                         uint32_t                 noiseTexBindless)
{
    if (m_pendingSpawns.size() >= kMaxSpawnsPerFrame) return; // drop

    TracerSpawnGPU s{};
    s.startPos            = start;
    s.endPos              = end;
    s.color               = color;
    s.width               = width;
    s.lifetime            = lifetime;
    s.maxLifetime         = lifetime;
    s.spawnTime           = m_lastGlobalTime;
    s.noiseTexBindlessIdx = noiseTexBindless;
    m_pendingSpawns.push_back(s);
}

void TracerSystem::BeginFrame(float dt, uint32_t /*frameIndex*/, float globalTimeSeconds)
{
    m_lastGlobalTime = globalTimeSeconds;

    // Clamp spawn count to upload-buffer capacity.
    const uint32_t spawnCount = std::min(static_cast<uint32_t>(m_pendingSpawns.size()),
                                          kMaxSpawnsPerFrame);

    // Patch spawnTime now we know the actual frame time (Spawn is called
    // mid-frame and may have stale lastGlobalTime).
    if (spawnCount > 0 && m_spawnMapped)
    {
        for (uint32_t i = 0; i < spawnCount; ++i)
            m_pendingSpawns[i].spawnTime = globalTimeSeconds;
        std::memcpy(m_spawnMapped, m_pendingSpawns.data(),
                    spawnCount * sizeof(TracerSpawnGPU));
    }

    // System CB.
    if (m_systemCBMapped)
    {
        TracerSystemParams cb{};
        cb.deltaTime    = dt;
        cb.poolCapacity = kMaxTracers;
        cb.spawnCount   = spawnCount;
        cb.time         = globalTimeSeconds;
        cb.writeCursor  = m_writeCursor;
        std::memcpy(m_systemCBMapped, &cb, sizeof(cb));
    }

    m_spawnCountThisFrame = spawnCount;

    // Advance ring cursor for next frame.
    m_writeCursor = (m_writeCursor + spawnCount) % kMaxTracers;

    m_pendingSpawns.clear();
}
