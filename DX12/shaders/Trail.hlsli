// Trail.hlsli — shared trail GPU data layouts.
// Match include/Graphics/TrailSystem.h exactly.
//
// IMPORTANT: editing this file does NOT invalidate the shader cache for
// dependent .hlsl files (ShaderLibrary only checks the .hlsl mtime). If
// you change a struct here, touch every dependent .hlsl (TrailUpdate,
// Trail.vs, Trail.ps) to trigger a recompile — or delete shader_cache/.

#ifndef TRAIL_HLSLI
#define TRAIL_HLSLI

struct TrailSegment
{
    float3 position;  // 12
    float  age;       // 4 — seconds since spawn
};                     // 16 bytes

struct TrailHeader
{
    uint   head;        // 4 — next write slot in the trail's ring
    uint   count;       // 4 — current segment count
    float  width;       // 4
    float  maxAge;      // 4
    float4 startColor;  // 16
    float4 endColor;    // 16
    uint   _reserved[8];// 32
};                       // 80 bytes

struct TrailAppendRequest
{
    float3 position;
    float  dt;
    uint   trailSlot;
    float  width;
    float  maxAge;
    uint   flags;        // bit0 = reset header (head/count→0) before append; bit1 = reset-only (skip append)
    float4 startColor;
    float4 endColor;
};                       // 64 bytes

struct TrailSystemParams
{
    float deltaTime;
    uint  requestCount;
    uint  maxSegments;
    uint  maxTrails;
};                       // 16 bytes

#endif
