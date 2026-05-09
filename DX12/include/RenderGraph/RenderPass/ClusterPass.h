#pragma once

// ClusterPass — compute pass for clustered deferred shading.
//
// Two dispatches per frame:
//   1. ClusterBuild: compute per-cluster view-space AABBs (once per resize)
//   2. ClusterCull:  assign lights to clusters (every frame)
//
// Executed before LightingPass. LightingPass reads the resulting
// g_LightGrid and g_LightIndexList SRVs.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "ECS/Components.h"

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class ClusterPass
{
public:
    // Grid constants (must match cluster_common.hlsli)
    static constexpr uint32_t kTileX   = 16;
    static constexpr uint32_t kTileY   = 9;
    static constexpr uint32_t kSlices  = 24;
    static constexpr uint32_t kClusterCount = kTileX * kTileY * kSlices;
    static constexpr uint32_t kMaxLights        = 4096;
    static constexpr uint32_t kMaxPerCluster    = 32; // must match MAX_PER_CLUSTER in shader
    static constexpr uint32_t kMaxLightIndices  = kClusterCount * kMaxPerCluster;

    // Probe cull — smaller per-cluster cap because per-pixel probe loops are
    // pricier than per-pixel light loops. Must match MAX_PROBES_PER_CLUSTER in
    // ClusterCullProbes.cs.hlsl.
    static constexpr uint32_t kMaxProbesPerCluster = 8;
    static constexpr uint32_t kMaxProbeIndices     = kClusterCount * kMaxProbesPerCluster;

    void Init(IGraphicsDevice& gfx);

    // Resolved light with world-space position/direction (from GlobalTransform).
    struct ResolvedLight
    {
        DirectX::XMFLOAT3 position;
        float             radius;
        DirectX::XMFLOAT3 color;
        float             intensity;
        DirectX::XMFLOAT3 direction;
        float             spotAngle;
        LightType         type;
        uint32_t          shadowSliceIdx = 0xFFFFFFFFu; // atlas slice; 0xFFFFFFFF = no map
    };

    // Upload lights from CPU. Call before Execute().
    void SetLights(const std::vector<ResolvedLight>& lights);

    // Set camera params (for cluster AABB computation).
    void SetCamera(const DirectX::XMFLOAT4X4& invProj,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   float nearZ, float farZ,
                   uint32_t screenW, uint32_t screenH);

    // Dispatch compute shaders. Must be called on a COMPUTE or GRAPHICS command list
    // before LightingPass::Execute().
    void Execute(RHI::CommandList cl);

    // SRV handles for LightingPass to bind.
    uint64_t GetLightsSRVHandle()    const { return m_lightsSRV; }
    uint64_t GetLightIndexSRVHandle() const { return m_lightIndexSRV; }
    uint64_t GetLightGridSRVHandle() const { return m_lightGridSRV; }

    // Cluster AABB SRV — exposed for passes (e.g. DecalPass) that want to
    // reuse the already-built per-frame cluster AABB grid rather than
    // duplicating the ClusterBuild dispatch.
    uint64_t GetClusterAABBSRVHandle() const { return m_clusterAABBSRV; }

    uint32_t GetLightCount() const { return m_lightCount; }

    // Reflection-probe input — Renderer pushes the probe StructuredBuffer SRV
    // and active probe count each frame after BuildScene_UploadProbes. The
    // pass executes a second compute dispatch (CSCullProbes) that mirrors the
    // light cull and produces the per-cluster probe index list.
    void SetProbes(uint64_t probeBufferSRV, uint32_t probeCount)
    {
        m_probeBufferSRV = probeBufferSRV;
        m_probeCount     = probeCount;
    }
    uint64_t GetProbeIndexSRVHandle() const { return m_probeIndexSRV; }
    uint64_t GetProbeGridSRVHandle()  const { return m_probeGridSRV;  }

private:
    // CB layout matching ClusterBuild / ClusterCull / ClusterCullProbes
    // shaders. All three use b0 space2 — viewMatrix is packed at the end.
    struct alignas(16) ClusterCB
    {
        float    invProj[16];     // 64 bytes
        float    nearZ;           // 4
        float    farZ;            // 4
        uint32_t screenW;         // 4
        uint32_t screenH;         // 4
        uint32_t lightCount;      // 4
        uint32_t probeCount;      // 4 — read by ClusterCullProbes only; ignored by light cull
        uint32_t _pad[2];         // 8
        float    viewMatrix[16];  // 64 bytes
    }; // total = 160 bytes, fits in 256-byte CB

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_buildPSO;
    RHI::PipelineState m_cullPSO;
    RHI::PipelineState m_probeCullPSO;

    // Buffers
    RHI::GPUBuffer m_lightBuffer;       // StructuredBuffer<GPULight> — UPLOAD
    RHI::GPUBuffer m_clusterAABBBuffer; // RWStructuredBuffer<ClusterAABB> — DEFAULT
    RHI::GPUBuffer m_lightIndexBuffer;  // RWStructuredBuffer<uint> — DEFAULT
    RHI::GPUBuffer m_lightGridBuffer;   // RWStructuredBuffer<LightGridEntry> — DEFAULT
    RHI::GPUBuffer m_counterBuffer;     // RWStructuredBuffer<uint> — DEFAULT (single atomic counter)
    RHI::GPUBuffer m_clusterCB;         // Constant buffer — UPLOAD
    RHI::GPUBuffer m_counterResetBuf;   // 4-byte UPLOAD buffer for counter reset

    // Reflection-probe cluster outputs — same layout convention as the light
    // grid (base offset = clusterIdx * kMaxProbesPerCluster, count in grid).
    RHI::GPUBuffer m_probeIndexBuffer;  // RWStructuredBuffer<uint> — DEFAULT
    RHI::GPUBuffer m_probeGridBuffer;   // RWStructuredBuffer<ProbeGridEntry> — DEFAULT

    // GPU handles
    uint64_t m_lightsSRV      = 0;
    uint64_t m_clusterAABBSRV = 0;
    uint64_t m_clusterAABBUAV = 0;
    uint64_t m_lightIndexSRV  = 0;
    uint64_t m_lightIndexUAV  = 0;
    uint64_t m_lightGridSRV   = 0;
    uint64_t m_lightGridUAV   = 0;
    uint64_t m_counterUAV     = 0;

    uint64_t m_probeIndexSRV  = 0;
    uint64_t m_probeIndexUAV  = 0;
    uint64_t m_probeGridSRV   = 0;
    uint64_t m_probeGridUAV   = 0;
    uint64_t m_probeBufferSRV = 0;     // SRV of GPUReflectionProbe buffer (set by Renderer)
    uint32_t m_probeCount     = 0;

    // Mapped pointers
    void* m_lightMapped     = nullptr;
    void* m_clusterCBMapped = nullptr;
    void* m_counterResetMapped = nullptr;

    // State
    uint32_t m_lightCount = 0;
    bool     m_needsRebuild = true;  // rebuild AABBs on resize
    uint32_t m_lastScreenW = 0;
    uint32_t m_lastScreenH = 0;

    // Per-frame CB data
    ClusterCB m_cbData{};
};
