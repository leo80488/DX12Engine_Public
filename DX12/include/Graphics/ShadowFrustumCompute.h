#pragma once

// ShadowFrustumCompute — pure math helper for CSM.
//
// Owns the per-cascade light-frustum OBBs + 6-plane approximations used to
// decide whether a scene AABB falls inside any shadow cascade (so off-screen
// shadow casters are retained during culling). Also builds the per-cascade
// orthographic light VP matrices and writes them to LightCB + ShadowPass.
//
// No GPU resources; no backend-specific types. All inputs come in by value
// / pointer so the class can be unit-tested in isolation.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <cstdint>

#include "RenderGraph/RenderPass/ShadowPass.h"   // kCascadeCount, kShadowMapSize
#include "Graphics/RenderTypes.h"                // RenderCamera, RenderView

class World;

class ShadowFrustumCompute
{
public:
    static constexpr int kCascadeCount = ShadowPass::kCascadeCount;
    struct Planes { DirectX::XMVECTOR planes[6]; };

    // Pre-compute per-cascade light-frustum OBBs for shadow-caster culling.
    // Must run before scene culling so off-screen entities that still fall
    // inside a cascade are retained as shadow casters. Returns true when a
    // directional light was found and the frustums are populated.
    bool Compute(World& world,
                 const RenderCamera& cam,
                 uint32_t vpW,
                 uint32_t vpH);

    // Test an AABB against cascade @p i using the fast AABB-vs-AABB reject
    // followed by a 6-plane conservative test. Returns true on intersection.
    // Callers iterate cascades themselves when they need to know which cascade
    // hit (e.g. for per-cascade culling diagnostics); this helper just answers
    // "does @p bb need to cast into ANY cascade?".
    bool IntersectsAny(const DirectX::BoundingBox& bb) const;

    bool IsValid() const { return m_valid; }

    // Per-cascade accessors (needed by callers that want to test one cascade
    // at a time). Only valid when IsValid() is true.
    const DirectX::BoundingOrientedBox& GetOBB(int i)      const { return m_obb[i];  }
    const DirectX::BoundingBox&         GetAABB(int i)     const { return m_aabb[i]; }
    const Planes&                       GetPlanes(int i)   const { return m_planes[i]; }

    // Test @p bb against exactly 6 outward-normal planes. Conservative —
    // never false-rejects, may false-accept (cheap). Used inline by callers
    // that walk cascades themselves.
    static bool AabbVs6Planes(const DirectX::BoundingBox& bb,
                              const DirectX::XMVECTOR (&planes)[6]);

private:
    DirectX::BoundingOrientedBox m_obb[kCascadeCount]{};
    DirectX::BoundingBox         m_aabb[kCascadeCount]{};
    alignas(16) Planes           m_planes[kCascadeCount]{};
    bool                         m_valid = false;
};
