#include "Graphics/TrailSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "ECS/ECS.h"
#include "ECS/TrailComponent.h"
#include "ECS/HierarchyComponents.h"
#include "System/Log.h"

#include <cstring>
#include <cmath>

void TrailSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Segment pool (DEFAULT, SR+UAV) ------------------------------------
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxTrails) * kMaxSegmentsPerTrail * sizeof(TrailSegmentGPU);
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailSegmentGPU);
        if (!gfx.CreateBuffer(d, m_segmentBuffer))
        {
            LOG_ERROR("TrailSystem: segment buffer creation failed");
            return;
        }
        m_segSrv = gfx.GetBufferSRVGpuHandle(m_segmentBuffer);
        m_segUav = gfx.GetBufferUAVGpuHandle(m_segmentBuffer);
    }

    // ---- Header pool (DEFAULT, SR+UAV) -------------------------------------
    // MUST be zero-initialised: the VS gates on `hdr.count` and the compute
    // shader ring-cursor math depends on `hdr.head`. DEFAULT heap contents
    // are undefined on allocation, so we pass an explicit zero blob as
    // initialData. Without this, garbage `count` makes the aging pass loop
    // for millions of iterations AND the VS renders bogus segment pairs.
    {
        const size_t bytes = static_cast<size_t>(kMaxTrails) * sizeof(TrailHeaderGPU);
        std::vector<uint8_t> zero(bytes, 0);

        RHI::GPUBufferDesc d{};
        d.size       = bytes;
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailHeaderGPU);
        if (!gfx.CreateBuffer(d, m_headerBuffer, zero.data()))
        {
            LOG_ERROR("TrailSystem: header buffer creation failed");
            return;
        }
        m_hdrSrv = gfx.GetBufferSRVGpuHandle(m_headerBuffer);
        m_hdrUav = gfx.GetBufferUAVGpuHandle(m_headerBuffer);
    }

    // ---- Append request buffer (UPLOAD, SR) --------------------------------
    // Structured buffer so the compute shader can index `gRequests[dispatchID]`
    // directly. Stride = natural sizeof(TrailAppendRequestGPU).
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxTrails) * sizeof(TrailAppendRequestGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailAppendRequestGPU);
        if (gfx.CreateBuffer(d, m_requestBuffer))
        {
            m_requestMapped = gfx.MapBuffer(m_requestBuffer);
            m_requestSrv    = gfx.GetBufferSRVGpuHandle(m_requestBuffer);
        }
        if (!m_requestMapped)
            LOG_ERROR("TrailSystem: request buffer map failed");
    }

    // ---- Per-frame globals CB (UPLOAD) -------------------------------------
    {
        RHI::GPUBufferDesc d{};
        d.size       = 256;
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(d, m_systemCB))
            m_systemCBMapped = gfx.MapBuffer(m_systemCB);
    }

    LOG_SUCCESS("TrailSystem: initialised (%u trails × %u segs × %zu B = %zu KB)",
                kMaxTrails, kMaxSegmentsPerTrail, sizeof(TrailSegmentGPU),
                (kMaxTrails * kMaxSegmentsPerTrail * sizeof(TrailSegmentGPU)) / 1024);
}

void TrailSystem::Shutdown(IGraphicsDevice& gfx)
{
    if (m_systemCBMapped)  { gfx.UnmapBuffer(m_systemCB);      m_systemCBMapped = nullptr; }
    if (m_requestMapped)   { gfx.UnmapBuffer(m_requestBuffer); m_requestMapped  = nullptr; }
    if (m_systemCB.IsValid())      gfx.DestroyBuffer(m_systemCB);
    if (m_requestBuffer.IsValid()) gfx.DestroyBuffer(m_requestBuffer);
    if (m_headerBuffer.IsValid())  gfx.DestroyBuffer(m_headerBuffer);
    if (m_segmentBuffer.IsValid()) gfx.DestroyBuffer(m_segmentBuffer);
    m_gfx = nullptr;
}

uint32_t TrailSystem::AllocateSlot()
{
    for (uint32_t i = 0; i < kMaxTrails; ++i)
    {
        const uint64_t bit = 1ULL << i;
        if ((m_slotUsedMask & bit) == 0)
        {
            m_slotUsedMask |= bit;
            if (i + 1 > m_maxActiveSlot) m_maxActiveSlot = i + 1;
            return i;
        }
    }
    return ~0u;
}

void TrailSystem::ReleaseSlot(uint32_t slot)
{
    if (slot >= kMaxTrails) return;
    m_slotUsedMask &= ~(1ULL << slot);
}

void TrailSystem::UpdateSystemCB(float dt)
{
    if (!m_systemCBMapped) return;
    TrailSystemCB cb{};
    cb.deltaTime    = dt;
    cb.requestCount = m_requestCountThisFrame;
    cb.maxSegments  = kMaxSegmentsPerTrail;
    cb.maxTrails    = kMaxTrails;
    std::memcpy(m_systemCBMapped, &cb, sizeof(cb));
}

void TrailSystem::CollectTrails(World& world, float dt)
{
    (void)dt;
    m_requestCountThisFrame = 0;
    if (!m_requestMapped) return;

    // Iterate TrailComponent pool directly (pool-direct iteration — see
    // memory/feedback_ecs_pool_iteration.md).
    auto* pTrail  = world.GetPool<TrailComponent>();
    auto* pGlobal = world.GetPool<GlobalTransform>();
    if (!pTrail) return;

    auto& ents = pTrail->Entities();
    auto& data = pTrail->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n && m_requestCountThisFrame < kMaxTrails; ++i)
    {
        const Entity e = ents[i];
        TrailComponent& tc = data[i];
        if (!tc.enabled) continue;

        // Lazy slot allocation.
        if (tc.trailSlot == 0xFFFFFFFFu)
        {
            tc.trailSlot = AllocateSlot();
            if (tc.trailSlot == 0xFFFFFFFFu)
            {
                // Pool exhausted; this trail simply doesn't render. We
                // don't release any other slot — a future TrailComponent
                // destructor hook would be the right place.
                continue;
            }
        }

        // Get world position.
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

        // minSampleDistance gate.
        if (tc.hasLastSample)
        {
            const float dx = worldPos.x - tc.lastSamplePos.x;
            const float dy = worldPos.y - tc.lastSamplePos.y;
            const float dz = worldPos.z - tc.lastSamplePos.z;
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < tc.minSampleDistance * tc.minSampleDistance)
                continue;
        }
        tc.lastSamplePos  = worldPos;
        tc.hasLastSample  = true;

        TrailAppendRequestGPU* slot = reinterpret_cast<TrailAppendRequestGPU*>(
            static_cast<uint8_t*>(m_requestMapped)
            + m_requestCountThisFrame * sizeof(TrailAppendRequestGPU));

        slot->position   = worldPos;
        slot->dt         = dt;
        slot->trailSlot  = tc.trailSlot;
        slot->width      = tc.width;
        slot->maxAge     = tc.maxAge;
        slot->_pad0      = 0.0f;
        slot->startColor = tc.startColor;
        slot->endColor   = tc.endColor;

        ++m_requestCountThisFrame;
    }
}
