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

    // ---- Append request buffer (UPLOAD, SR) — triple-buffered manual ring --
    // Structured buffer so the compute shader can index `gRequests[dispatchID]`
    // directly. Stride = natural sizeof(TrailAppendRequestGPU).
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxTrails) * sizeof(TrailAppendRequestGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailAppendRequestGPU);
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_requestBuffer[i]))
            {
                m_requestMapped[i] = gfx.MapBuffer(m_requestBuffer[i]);
                m_requestSrv[i]    = gfx.GetBufferSRVGpuHandle(m_requestBuffer[i]);
            }
            if (!m_requestMapped[i])
                LOG_ERROR("TrailSystem: request buffer[%u] map failed", i);
        }
    }

    // ---- Per-frame globals CB (UPLOAD) — triple-buffered -------------------
    if (!m_systemCB.Create(gfx, "TrailSystem.SystemCB"))
        LOG_ERROR("TrailSystem: system CB create failed");

    LOG_SUCCESS("TrailSystem: initialised (%u trails × %u segs × %zu B = %zu KB)",
                kMaxTrails, kMaxSegmentsPerTrail, sizeof(TrailSegmentGPU),
                (kMaxTrails * kMaxSegmentsPerTrail * sizeof(TrailSegmentGPU)) / 1024);
}

void TrailSystem::Shutdown(IGraphicsDevice& gfx)
{
    m_systemCB.Destroy(gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_requestMapped[i])  { gfx.UnmapBuffer(m_requestBuffer[i]); m_requestMapped[i] = nullptr; }
        if (m_requestBuffer[i].IsValid()) gfx.DestroyBuffer(m_requestBuffer[i]);
        m_requestSrv[i] = 0;
    }
    if (m_headerBuffer.IsValid())  gfx.DestroyBuffer(m_headerBuffer);
    if (m_segmentBuffer.IsValid()) gfx.DestroyBuffer(m_segmentBuffer);
    m_gfx = nullptr;
}

const RHI::GPUBuffer& TrailSystem::GetRequestBuffer(IGraphicsDevice& gfx) const
{
    return m_requestBuffer[gfx.GetFrameIndex()];
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
    if (!m_gfx) return;
    auto* slot = m_systemCB.Current(*m_gfx);
    if (!slot) return;
    TrailSystemCB cb{};
    cb.deltaTime    = dt;
    cb.requestCount = m_requestCountThisFrame;
    cb.maxSegments  = kMaxSegmentsPerTrail;
    cb.maxTrails    = kMaxTrails;
    *slot = cb;
}

void TrailSystem::CollectTrails(World& world, float dt)
{
    m_requestCountThisFrame = 0;
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot >= kFrameCount || !m_requestMapped[frameSlot]) return;

    // Append-request emitter — writes one TrailAppendRequestGPU into the upload
    // ring. flags drives reset/reset-only behaviour (see TrailRequestFlag).
    auto emitRequest = [&](uint32_t slot, const DirectX::XMFLOAT3& worldPos,
                           const TrailComponent& tc, uint32_t flags)
    {
        if (m_requestCountThisFrame >= kMaxTrails) return;
        TrailAppendRequestGPU* req = reinterpret_cast<TrailAppendRequestGPU*>(
            static_cast<uint8_t*>(m_requestMapped[frameSlot])
            + m_requestCountThisFrame * sizeof(TrailAppendRequestGPU));
        req->position   = worldPos;
        req->dt         = dt;
        req->trailSlot  = slot;
        req->width      = tc.width;
        req->maxAge     = tc.maxAge;
        req->flags      = flags;
        req->startColor = tc.startColor;
        req->endColor   = tc.endColor;
        ++m_requestCountThisFrame;
    };

    // Iterate TrailComponent pool directly (pool-direct iteration — see
    // memory/feedback_ecs_pool_iteration.md).
    auto* pTrail  = world.GetPool<TrailComponent>();
    auto* pGlobal = world.GetPool<GlobalTransform>();

    // ---- Reconcile slot ownership from LIVE components --------------------
    // Historically ReleaseSlot was never called, so a TrailComponent destroyed
    // by LifetimeSystem (the common case for notify-spawned VFX trails) leaked
    // its slot forever — only 64 slots EVER, then the pool jammed. We instead
    // rebuild the used-slot mask from the live pool each frame: any slot whose
    // owner vanished is implicitly freed here, and gets a reset-only GPU
    // request below so its stale ribbon stops drawing.
    uint64_t liveMask = 0;
    if (pTrail)
    {
        auto& data = pTrail->Data();
        for (const TrailComponent& tc : data)
            if (tc.trailSlot != 0xFFFFFFFFu && tc.trailSlot < kMaxTrails)
                liveMask |= (1ULL << tc.trailSlot);
    }
    const uint64_t freed = m_slotUsedMask & ~liveMask;
    m_slotUsedMask  = liveMask;
    m_maxActiveSlot = 0;                  // recomputed over live + new slots below

    uint64_t allocatedThisFrame = 0;
    uint64_t touchedMask        = 0;      // slots that got an append or reset request

    if (pTrail)
    {
        auto& ents = pTrail->Entities();
        auto& data = pTrail->Data();
        const size_t n = data.size();

        for (size_t i = 0; i < n && m_requestCountThisFrame < kMaxTrails; ++i)
        {
            const Entity e = ents[i];
            TrailComponent& tc = data[i];

            // Disabled trails keep their leased slot + frozen ribbon (matches
            // legacy behaviour); count them toward the draw range but emit no
            // sample.
            if (tc.trailSlot != 0xFFFFFFFFu && tc.trailSlot < kMaxTrails &&
                tc.trailSlot + 1 > m_maxActiveSlot)
                m_maxActiveSlot = tc.trailSlot + 1;
            if (!tc.enabled) continue;

            uint32_t flags = 0;

            // Lazy slot allocation. A recycled slot still holds the previous
            // trail's head/count, so its first append MUST reset the header.
            if (tc.trailSlot == 0xFFFFFFFFu)
            {
                tc.trailSlot = AllocateSlot();
                if (tc.trailSlot == 0xFFFFFFFFu)
                    continue;             // pool full this frame; render nothing
                allocatedThisFrame |= (1ULL << tc.trailSlot);
                flags |= TrailRequest_ResetHeader;
                tc.hasLastSample = false; // sample immediately at the seeded pose
                if (tc.trailSlot + 1 > m_maxActiveSlot) m_maxActiveSlot = tc.trailSlot + 1;
            }

            // World position = GlobalTransform translation column.
            DirectX::XMFLOAT3 worldPos = { 0, 0, 0 };
            if (pGlobal)
                if (const GlobalTransform* gt = pGlobal->Get(e))
                {
                    worldPos.x = gt->matrix._41;
                    worldPos.y = gt->matrix._42;
                    worldPos.z = gt->matrix._43;
                }

            // minSampleDistance gate — skip duplicate points for a still trail.
            if (tc.hasLastSample)
            {
                const float dx = worldPos.x - tc.lastSamplePos.x;
                const float dy = worldPos.y - tc.lastSamplePos.y;
                const float dz = worldPos.z - tc.lastSamplePos.z;
                if (dx*dx + dy*dy + dz*dz < tc.minSampleDistance * tc.minSampleDistance)
                {
                    // Gated out, but a just-allocated slot still needs its stale
                    // header cleared, else it draws ghost segments.
                    if (flags & TrailRequest_ResetHeader)
                    {
                        emitRequest(tc.trailSlot, worldPos, tc, TrailRequest_ResetOnly);
                        touchedMask |= (1ULL << tc.trailSlot);
                    }
                    continue;
                }
            }
            tc.lastSamplePos = worldPos;
            tc.hasLastSample = true;

            emitRequest(tc.trailSlot, worldPos, tc, flags);
            touchedMask |= (1ULL << tc.trailSlot);
        }
    }

    // ---- Clear headers of slots freed this frame that no live trail reused.
    //      Without this the destroyed trail's frozen ribbon keeps rendering
    //      until its slot happens to be reallocated.
    const uint64_t freedNotReused = freed & ~allocatedThisFrame & ~touchedMask;
    if (freedNotReused)
    {
        TrailComponent dummy{};           // width/colours unused for reset-only
        for (uint32_t s = 0; s < kMaxTrails && m_requestCountThisFrame < kMaxTrails; ++s)
            if (freedNotReused & (1ULL << s))
                emitRequest(s, { 0, 0, 0 }, dummy, TrailRequest_ResetOnly);
    }
}
