// TrailUpdate.cs.hlsl — two passes fused into one dispatch:
//
//  Pass A (thread 0..maxTrails-1): age every existing segment by dt.
//  Pass B (thread 0..requestCount-1): append new segment at each active
//                                     trail's head cursor.
//
// We dispatch max(maxTrails, requestCount) threads and each thread decides
// which work it does based on its id. This keeps the shader to ONE
// dispatch (no extra command-list overhead) while letting aging apply to
// trails that didn't emit a new sample this frame.
//
// Root signature (space2):
//   b0 → TrailSystemParams CB
//   t0 → StructuredBuffer<TrailAppendRequest> gRequests  (UPLOAD SRV)
//   u0 → RWStructuredBuffer<TrailSegment> gSegments
//   u1 → RWStructuredBuffer<TrailHeader>  gHeaders

#include "Trail.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    TrailSystemParams gParams;
};

StructuredBuffer<TrailAppendRequest>   gRequests : register(t0, space2);
RWStructuredBuffer<TrailSegment>       gSegments : register(u0, space2);
RWStructuredBuffer<TrailHeader>        gHeaders  : register(u1, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tid = id.x;

    // ---- Pass A: age all segments of every trail ---------------------------
    // One thread per trail slot. Walks the trail's segment range and adds dt
    // to each age field. Segment count is capped at maxSegments so at most
    // kMaxSegments iterations per thread (cheap).
    if (tid < gParams.maxTrails)
    {
        TrailHeader hdr = gHeaders[tid];
        // Age every live segment. We still iterate a fixed count to keep
        // execution uniform across the wave; dead slots (age already past
        // maxAge) get their age bumped harmlessly.
        for (uint s = 0; s < hdr.count; ++s)
        {
            const uint segIdx = tid * gParams.maxSegments + s;
            TrailSegment seg = gSegments[segIdx];
            seg.age += gParams.deltaTime;
            gSegments[segIdx] = seg;
        }
    }

    // ---- Pass B: append new segment per append request ---------------------
    if (tid < gParams.requestCount)
    {
        TrailAppendRequest req = gRequests[tid];

        TrailHeader hdr = gHeaders[req.trailSlot];

        // Slot reuse: a trail slot recycled from a destroyed trail still holds
        // the dead trail's head/count, which would draw frozen ghost segments
        // (and append on top of them). flags bit0 zeroes the ring so the slot
        // starts clean. bit1 is a reset-only request (a freed slot with no new
        // owner this frame) — clear the header so nothing renders, then bail.
        if (req.flags & 1u) { hdr.head = 0; hdr.count = 0; }
        if (req.flags & 2u)
        {
            hdr.count = 0;
            hdr.head  = 0;
            gHeaders[req.trailSlot] = hdr;
            return;
        }

        // New segment goes at head (ring cursor).
        const uint writeLocal = hdr.head;
        const uint writeGlobal = req.trailSlot * gParams.maxSegments + writeLocal;

        TrailSegment seg;
        seg.position = req.position;
        seg.age      = 0.0;  // just spawned
        gSegments[writeGlobal] = seg;

        // Advance ring cursor + bump count (clamped at maxSegments).
        hdr.head  = (writeLocal + 1) % gParams.maxSegments;
        if (hdr.count < gParams.maxSegments) hdr.count += 1;

        // Refresh per-frame render params (width / colour / maxAge).
        hdr.width      = req.width;
        hdr.maxAge     = req.maxAge;
        hdr.startColor = req.startColor;
        hdr.endColor   = req.endColor;

        gHeaders[req.trailSlot] = hdr;
    }
}
