#pragma once

// DecalPass — compute-only clustered decal system.
//
// Two dispatches per frame (between GBufferPass and LightingPass):
//   1. DecalClusterCull: assign decal OBBs to clusters (SOB reuse of
//      ClusterPass's cluster AABB buffer — no second AABB build needed).
//   2. DecalApply:       per-pixel blend textures into GBuffer0/1/2 via UAV.
//
// Inputs it consumes from outside:
//   - cluster AABB SRV (from ClusterPass; shared)
//   - scene depth SRV (from GBufferPass, transitioned by RG)
//   - three GBuffer UAVs (RG transitions to UNORDERED_ACCESS via Setup()
//     declarations — isUAV=true on the texture descriptors at CreateTexture)
//
// Fits into the render graph between GBufferPass and LightingPass:
//   GBufferPass → DecalPass → LightingPass
// RG transitions:
//   GBuffer RT   → UAV (DecalPass) → SRV (LightingPass)
//   SceneDepth   → SRV_compute     → SRV_pixel (no-op, both pixel+non-pixel)

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"
#include "ECS/Components.h"

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class DecalPass : public RG::RenderPass
{
public:
    // Keep small enough that one cluster's decal list fits in ~32 bytes.
    // Bump later via debug heatmap if overflow is frequent. MUST match
    // MAX_DECALS_PER_CLUSTER in decal_common.hlsli.
    static constexpr uint32_t kMaxDecals       = 512;
    static constexpr uint32_t kMaxPerCluster   = 16;

    DecalPass(RG::RGTextureHandle albedo,
              RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle depth);

    const char* GetName() const override { return "DecalPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Optional: dispatch the cluster-cull step on an external command list
    // (e.g. the graphics CL that hosts ClusterPass, so the cull overlaps
    // GBufferPass rasterisation naturally on GPU). When this is called,
    // Execute() skips its own cull dispatch and only runs the apply CS.
    //
    // Must be called AFTER SetDecals() + SetCamera() for the current frame.
    // The external CL must be submitted BEFORE (or in front of, on the same
    // queue) the command list that ultimately hosts Execute() — same-queue
    // ordering gives GPU read-after-write visibility for the cull outputs.
    void DispatchCull(RHI::CommandList cl);

    // CPU-side decal record. Renderer builds one of these per DecalComponent
    // each frame (filtering out decals with no valid textures).
    //
    // The 9 texture indices map 1:1 to DecalMaterialAsset::TextureSlot enum
    // — Renderer just copies through asset->texBindless[] with -1 for unused
    // slots. Scalar values pack into two float4s for a single 16-byte read
    // in the apply CS (see decal_common.hlsli GPUDecal layout).
    struct ResolvedDecal
    {
        DirectX::XMFLOAT4X4 worldToDecal;   // inverse of entity world transform
        DirectX::XMFLOAT3   boundsCenter;
        float               boundsRadius;
        DirectX::XMFLOAT3   decalForwardWS;

        // Texture bindless indices (-1 = unused).
        int32_t texBaseColor    = -1;
        int32_t texNormal       = -1;
        int32_t texOpacity      = -1;
        int32_t texRoughness    = -1;
        int32_t texSpecular     = -1;
        int32_t texAO           = -1;
        int32_t texBump         = -1;
        int32_t texCavity       = -1;
        int32_t texDisplacement = -1;

        uint32_t flags = 0;
        float    sortLayer = 0.0f;
        float    angleFadeStart = 0.3f;

        DirectX::XMFLOAT4 baseColorTint = { 1, 1, 1, 1 }; // rgb tint, a = overall opacity factor

        // scalars0: x=opacity, y=roughness, z=specular, w=ao
        DirectX::XMFLOAT4 scalars0 = { 1.f, 0.5f, 0.5f, 1.f };
        // scalars1: x=normalStrength, y=bumpStrength, z=cavityStrength, w=displacementScale
        DirectX::XMFLOAT4 scalars1 = { 1.f, 1.f, 1.f, 0.f };
    };

    // Upload the per-frame decal list. Call before Execute() each frame.
    void SetDecals(const std::vector<ResolvedDecal>& decals);

    // Hook ClusterPass's cluster AABB SRV so we can reuse its per-frame AABB
    // build. If not wired, DecalPass runs a no-op (no cluster AABB → no cull).
    void SetClusterAABBSRV(uint64_t srv) { m_clusterAABBSRV = srv; }

    // Debug heatmap toggle — replaces albedo output with a per-cluster
    // decal-count heatmap (green→yellow→red). Normal/surface unchanged so
    // lighting still reads real GBuffer. Driven from the editor's Debug menu.
    void SetDebugHeatmap(bool enabled) { m_debugHeatmap = enabled; }
    bool GetDebugHeatmap() const { return m_debugHeatmap; }

    // Camera info (same layout as ClusterPass — shared CB design). Must be
    // called each frame before Execute(). cameraPosWS is needed by the
    // displacement-parallax path in the apply CS.
    void SetCamera(const DirectX::XMFLOAT4X4& invProj,
                   const DirectX::XMFLOAT4X4& invViewProj,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   const DirectX::XMFLOAT3&   cameraPosWS,
                   float nearZ, float farZ,
                   uint32_t screenW, uint32_t screenH);

private:
    // CB for DecalClusterCull — same binary layout as DecalCB in the cull shader.
    struct alignas(16) DecalCullCB
    {
        float    invProj[16];      // 64
        float    nearZ;            // 4
        float    farZ;             // 4
        uint32_t screenW;          // 4
        uint32_t screenH;          // 4
        uint32_t decalCount;       // 4
        uint32_t _pad0[3];         // 12
        float    viewMatrix[16];   // 64
    }; // 160

    // CB for DecalApply — mirrors DecalApplyCB in DecalApply.cs.hlsl.
    struct alignas(16) DecalApplyCB
    {
        float    invViewProj[16];  // 64
        float    invProj[16];      // 64
        float    cameraPosWS[3];   // 12 — parallax requires the view direction
        float    nearZ;            // 4
        float    farZ;             // 4
        uint32_t screenW;          // 4
        uint32_t screenH;          // 4
        uint32_t decalCount;       // 4
        uint32_t debugMode;        // 4 — 0=off, 1=cluster heatmap
        uint32_t _pad1[2];         // 8
    }; // 176

    RG::RGTextureHandle m_albedoH;
    RG::RGTextureHandle m_normalH;
    RG::RGTextureHandle m_surfaceH;
    RG::RGTextureHandle m_depthH;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_cullPSO;
    RHI::PipelineState m_applyPSO;

    // GPU buffers
    // Decal upload — triple-buffered manual ring; struct UPLOAD heap, SR-bound.
    static constexpr uint32_t kFrameCount = 3;
    RHI::GPUBuffer m_decalBuffer[kFrameCount];     // StructuredBuffer<GPUDecal>          — UPLOAD, mapped
    RHI::GPUBuffer m_decalIndexBuf;                // RWStructuredBuffer<uint>            — DEFAULT
    RHI::GPUBuffer m_decalGridBuf;                 // RWStructuredBuffer<DecalGridEntry>  — DEFAULT
    FrameCB<DecalCullCB>  m_cullCB;                // UPLOAD CB, triple-buffered
    FrameCB<DecalApplyCB> m_applyCB;               // UPLOAD CB, triple-buffered

    void*    m_decalMapped[kFrameCount]  = {};
    uint64_t m_decalsSRV[kFrameCount]    = {};

    uint64_t m_decalIndexSRV  = 0;
    uint64_t m_decalIndexUAV  = 0;
    uint64_t m_decalGridSRV   = 0;
    uint64_t m_decalGridUAV   = 0;

    uint64_t m_clusterAABBSRV = 0;

    // Bindless texture heap base (cached from GraphicsDX12). Handed to the
    // apply CS via the new compute root param 17 (t0 space3 → g_AllTextures[]).
    uint64_t m_bindlessTexHandle = 0;

    DecalCullCB  m_cullCB_data{};
    DecalApplyCB m_applyCB_data{};
    uint32_t     m_decalCount = 0;
    bool         m_debugHeatmap = false;
    // Set by DispatchCull; consumed + cleared by Execute. Lets Renderer opt
    // into cross-CL overlap without forcing every caller to.
    bool         m_externalCullDone = false;
};
