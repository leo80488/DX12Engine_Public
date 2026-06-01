#pragma once

// Renderer-layer types: FrameIndex, DrawFilter, DrawPacket, DrawList, RenderView.
// No DX12 types (ID3D12*, D3D12_*, DXGI_FORMAT) appear here.
//
// DrawPacket intentionally carries NO PSO pointer.
// The Renderer analyzes mesh+material to decide filter membership and permutation.
// Each RenderPass resolves the matching PSO from its own PSOCache at draw time.

#include "Graphics/GraphicsStruct.h"
#include <cmath>
#include <DirectXCollision.h>
#include "Graphics/PermutationKey.h"

#include <cstdint>
#include <array>
#include <span>
#include <DirectXMath.h>

// ---------------------------------------------------------------------------
using FrameIndex = uint64_t;

// ---------------------------------------------------------------------------
enum class DrawFilter : uint8_t
{
    Opaque      = 0,
    Shadow      = 1,
    Transparent = 2,
    Custom      = 3,
};

// ---------------------------------------------------------------------------
struct FrustumPlane
{
    DirectX::XMFLOAT3 normal;
    float             distance;
};
using FrustumPlanes = std::array<FrustumPlane, 6>;

// Extract 6 world-space frustum planes from a world→clip matrix (viewProj)
// via the Gribb-Hartmann method. Normals point INWARD so the fast path
// FrustumTestAABB(FrustumPlanes, ...) can reject with a single dot product
// per plane. Works for both standard and reverse-Z (the clip-space volume
// 0 ≤ z ≤ 1 is identical; only the near/far mapping differs).
//
// Row-vector convention: worldPt · VP = clipPt. Memory layout row-major
// (M[row][col]). Components of clipPt are columns of VP:
//   clip_x = Σ M[r][0] · worldPt[r]
//   clip_y = Σ M[r][1] · worldPt[r]
//   clip_z = Σ M[r][2] · worldPt[r]
//   clip_w = Σ M[r][3] · worldPt[r]
// Inside frustum ⇔ -w ≤ clip_x ≤ w, -w ≤ clip_y ≤ w, 0 ≤ clip_z ≤ w
// → the 6 inward-normal plane equations are:
//   Left:   (col[3]+col[0]) · p ≥ 0
//   Right:  (col[3]-col[0]) · p ≥ 0
//   Bottom: (col[3]+col[1]) · p ≥ 0
//   Top:    (col[3]-col[1]) · p ≥ 0
//   Near:   col[2]          · p ≥ 0
//   Far:    (col[3]-col[2]) · p ≥ 0
inline FrustumPlanes ExtractFrustumPlanes(const DirectX::XMFLOAT4X4& viewProj)
{
    const auto& m = viewProj.m;
    auto makePlane = [](float a, float b, float c, float d) -> FrustumPlane
    {
        const float invLen = 1.0f / std::sqrt(a*a + b*b + c*c);
        FrustumPlane p;
        p.normal   = { a * invLen, b * invLen, c * invLen };
        p.distance = d * invLen;
        return p;
    };
    FrustumPlanes out;
    out[0] = makePlane(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]); // Left
    out[1] = makePlane(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]); // Right
    out[2] = makePlane(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]); // Bottom
    out[3] = makePlane(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]); // Top
    out[4] = makePlane(m[0][2],           m[1][2],           m[2][2],           m[3][2]);           // Near
    out[5] = makePlane(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]); // Far
    return out;
}

// Test an AABB against the BoundingFrustum. Uses DirectXCollision for correctness.
inline bool FrustumTestAABB(const DirectX::BoundingFrustum& frustum,
                             const DirectX::XMFLOAT3& aabbMin,
                             const DirectX::XMFLOAT3& aabbMax)
{
    DirectX::BoundingBox box;
    box.Center  = { (aabbMin.x + aabbMax.x) * 0.5f,
                    (aabbMin.y + aabbMax.y) * 0.5f,
                    (aabbMin.z + aabbMax.z) * 0.5f };
    box.Extents = { (aabbMax.x - aabbMin.x) * 0.5f,
                    (aabbMax.y - aabbMin.y) * 0.5f,
                    (aabbMax.z - aabbMin.z) * 0.5f };
    return frustum.Intersects(box);
}

// Legacy overload for FrustumPlanes (fallback).
inline bool FrustumTestAABB(const FrustumPlanes& frustum,
                             const DirectX::XMFLOAT3& aabbMin,
                             const DirectX::XMFLOAT3& aabbMax)
{
    for (const auto& p : frustum)
    {
        float px = (p.normal.x >= 0) ? aabbMax.x : aabbMin.x;
        float py = (p.normal.y >= 0) ? aabbMax.y : aabbMin.y;
        float pz = (p.normal.z >= 0) ? aabbMax.z : aabbMin.z;
        if (p.normal.x * px + p.normal.y * py + p.normal.z * pz + p.distance < 0)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
struct RenderView
{
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;         // jittered — used for 3D scene rendering
    DirectX::XMFLOAT4X4 viewProjMatrix;     // jittered — used for 3D scene rendering
    DirectX::XMFLOAT4X4 projMatrixNoJitter;     // un-jittered — use for ImGuizmo / overlay
    DirectX::XMFLOAT4X4 viewProjMatrixNoJitter; // un-jittered — use for ImGuizmo / overlay
    DirectX::XMFLOAT3   cameraPosition;
    DirectX::XMFLOAT3   cameraForward;   // unit forward vector (into scene)
    float               nearZ    = 0.1f;
    float               farZ     = 1000.f;
    float               exposure = 1.0f;
    FrustumPlanes              frustum{};
    DirectX::BoundingFrustum   boundingFrustum{}; // DirectXMath frustum for AABB tests
};

// ---------------------------------------------------------------------------
// DrawPacket — plain descriptor for one draw call (PVF version).
//
// Produced by Renderer::BuildRenderScene (mesh+material analysis).
// Consumed by RenderPass::Execute — the pass queries its own PSOCache using
// the permutation to select and batch-switch PSOs.
// No VB/IB pointers — vertex data is fetched from ByteAddressBuffer SRVs.
// ---------------------------------------------------------------------------
struct DrawPacket
{
    uint32_t       meshDescriptorIndex = 0;  // slot in global MeshDescriptor buffer
    uint32_t       instanceOffset      = 0;  // first instance in InstanceBuffer
    uint32_t       instanceCount       = 1;
    uint32_t       vertexOrIndexCount  = 0;  // passed to DrawInstanced
    uint32_t       materialIndex       = 0;
    PermutationKey permutation{};            // active shader features for this material
    DrawFilter     filter              = DrawFilter::Opaque;
    uint64_t       sortKey             = 0;
    uint64_t       texBaseColor        = 0;  // GPU SRV handle for base-color texture (0 = default white)
    uint64_t       texSurfaceMap       = 0;  // GPU SRV handle for surface map (R=roughness, G=metalness)
    uint64_t       texNormalMap        = 0;  // GPU SRV handle for normal map (0 = default flat normal)
    float          outlinePixels       = 2.0f; // per-material outline thickness (only used for DrawFilter::Custom)
    bool           screenSpaceOutline  = true; // enable screen-space edge detection for this draw
    // Editor selection outline: written into ObjectID with depth-test OFF +
    // bit-31 tag, painted yellow-green by OutlinePass's screen-space sub-pass.
    // Skips the inverted-hull sub-pass entirely.
    bool           isPickingOutline    = false;
    uint8_t        stencilRef          = 1;    // stencil value: 1=PBR, 2=NPR (written in GBuffer pass)
    // Cast-side rasterizer cull override for ShadowPass. Raw storage of
    // ShadowCullMode enum (0=Default back-cull, 1=Front, 2=None); the enum
    // definition lives in ECS/Components.h to avoid an include from the
    // Renderer-layer RenderTypes.h back up into ECS.
    uint8_t        shadowCullMode      = 0;
    // When false, ShadowPass skips this draw entirely — material opted out of
    // casting shadows via MaterialComponent::CAST_SHADOW. The packet still
    // lives in the Opaque/Transparent draw lists for GBuffer / Lighting.
    uint8_t        castShadow          = 1;
    // Phase 3 author-intent: which views this packet participates in (see
    // ViewBit in HierarchyComponents.h). Each pass AND's its own bit against
    // viewMask before submitting. Default ~0u keeps legacy emitters (lights,
    // billboards, beams that don't read VisibilityComponent) visible everywhere.
    uint32_t       viewMask            = 0xFFFFFFFFu;
    // 0 = entity opted out of the main / GBuffer pass (e.g. shadow-only proxies).
    uint8_t        renderInMainPass    = 1;
    uint32_t       prevPosElementBase  = 0xFFFFFFFFu; // TAA: prev skinned pos base element (0xFFFFFFFF = none)
    // Custom pixel shader override — 0 means "use the pass default".
    // Populated upstream (Renderer::BuildRenderScene) by resolving the
    // material's customShaderPath via ShaderLibrary::RegisterDynamic. The
    // GBuffer pass plugs it into PSODesc.psID in place of ShaderID::GBuffer_PS.
    uint32_t       customPSID          = 0;
    // Phase E: GPU VA of the packed custom-material CBV for this draw (root
    // slot 35, register b8 space0). 0 means the custom PS didn't declare a
    // cbuffer or the material didn't use a custom shader — bind is skipped.
    uint64_t       customCbvVA         = 0;
    // Phase F: GPU handle of the packed custom-material texture table (root
    // slot 36, t0-t3 space3). 0 means no custom textures — bind is skipped.
    uint64_t       customTexTable      = 0;

    // Build a sort key from permutation bits (pass-bucket), material, and depth.
    static uint64_t MakeSortKey(uint16_t permBits, uint16_t materialBits, uint32_t depthBits)
    {
        return ((uint64_t)permBits      << 48)
             | ((uint64_t)materialBits  << 32)
             | ((uint64_t)depthBits);
    }
};

using DrawList = std::span<const DrawPacket>;

// Per-PSO group for ExecuteIndirect batching.
// Contiguous range of IndirectDrawCommands sharing the same PSO + stencil ref
// + customPSID + customCbvVA. Materials with custom PSes get their own group;
// the GBuffer pass switches PSO once per group before issuing the indirect call.
struct IndirectGroup
{
    PermutationKey perm;
    uint8_t        stencilRef     = 1;
    uint32_t       customPSID     = 0;  // 0 = default GBuffer_PS
    uint64_t       customCbvVA    = 0;  // 0 = no custom CBV bind (Phase E)
    uint64_t       customTexTable = 0;  // 0 = no custom texture table (Phase F)
    uint32_t       argOffset      = 0;  // byte offset into the indirect arg buffer
    uint32_t       cmdCount       = 0;  // number of IndirectDrawCommands in this group
};

// ---------------------------------------------------------------------------
// RenderCamera — camera state forwarded from the game layer to the Renderer.
// Set per-frame via Renderer::SetCamera() before BeginFrame/Render.
// `position` + `forward` come straight from the camera entity's GlobalTransform;
// the Renderer builds the view matrix via XMMatrixLookToLH (no Euler angles).
// Defaults reproduce the original hardcoded view (eye=(4,3,5), look-at origin).
struct RenderCamera
{
    DirectX::XMFLOAT3 position = { 4.f, 3.f, 5.f };
    DirectX::XMFLOAT3 forward  = { -0.566f, -0.424f, -0.707f }; // normalized world-space view dir
    float             fov      = DirectX::XM_PI / 3.f;
    float             nearZ    = 0.1f;
    float             farZ     = 200.f;
    // False for one frame on a hard cut (Camera.HardCutTo, initial spawn,
    // cinematic shot boundary). Temporal passes (TAA, XeGTAO, VolumetricFog,
    // SSR) AND this with their internal heuristic — wired by the camera
    // stack pipeline so all temporal-history-bearing effects clear in lockstep.
    // Defaults to true so callers that don't set it preserve existing behavior.
    bool              historyValid = true;
};
