#include "UI/WorldSpaceUISystem.h"
#include "UI/WorldSpaceUI.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace DirectX;

namespace UI
{
    // Project (x, y, z, 1) by a row-major DirectX-style view-proj matrix
    // (row-vector convention: clip = pos × M). Returns clip.w as the
    // camera-space depth (signed; <=0 means behind camera).
    static float ProjectToCameraDepth(const XMFLOAT4X4& m,
                                       float x, float y, float z)
    {
        const float* p = reinterpret_cast<const float*>(&m);
        // For column-major access: m[r][c] = m._{r+1}{c+1}
        // Camera-space depth = clip.w = pos × column 3 of M.
        return p[3]*x + p[7]*y + p[11]*z + p[15];
    }

    void WorldSpaceUISystem::Tick(World& world, const WorldSpaceUIView& view, float dt)
    {
        // ---- Drive WorldSpaceUI fade / scale / cull --------------------
        if (auto* pool = world.GetPool<WorldSpaceUIComponent>())
        {
            const auto& ents = pool->Entities();
            auto&       data = pool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                WorldSpaceUIComponent& c = data[i];
                c.isCulled      = true;        // default: don't render
                c.computedAlpha = 0.0f;
                c.computedScale = 1.0f;

                if (!view.valid) continue;

                const Entity e = ents[i];
                const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                if (!gt) continue;

                // Add per-frame DamageNumber bobble offset if present.
                float wx = gt->matrix.m[3][0];
                float wy = gt->matrix.m[3][1];
                float wz = gt->matrix.m[3][2];
                if (auto* dn = world.GetComponent<DamageNumberComponent>(e))
                {
                    wx += dn->currentOffset.x;
                    wy += dn->currentOffset.y;
                    wz += dn->currentOffset.z;
                }

                const float depth = ProjectToCameraDepth(view.viewProjMatrix, wx, wy, wz);
                if (depth <= 0.f && c.hideWhenBehindCamera) continue;

                if (c.fadeNear > 0.f && depth < c.fadeNear) continue;
                if (c.fadeFar  > 0.f && depth > c.fadeFar)  continue;

                // ConstantWorld scale: at this depth the world quad is
                // already physically correct via projection — `computedScale`
                // is the multiplier on baseSize.x/y. For ConstantWorld we
                // just emit the world size (multiplier=1). For ConstantPixel
                // we emit a multiplier such that the projected size in
                // pixels stays constant regardless of depth.
                if (c.scalingMode == ScalingMode::ConstantPixel)
                {
                    // pixel_height = world_height * pixelsPerWorldAtUnitDistance / depth
                    // We want pixel_height == baseSize.y (treated as pixel size).
                    // → world_height = depth * baseSize.y / pixelsPerWorldAtUnitDistance.
                    // pixelsPerWorldAtUnitDistance for FOV f, canvasH H:
                    //   = H / (2 * tan(f / 2))
                    // We don't have FOV directly — recover it by projecting a unit
                    // up vector from depth=1: m23 / m22 ≈ tan(fov/2) for std proj.
                    // Cheaper: project a small offset and back out the pixel ratio.
                    // For simplicity: assume canvasSize.y / 2 ≈ tan(fov/2)*pixelsPerUnit
                    // and use the relation pixelHeight ≈ worldHeight * canvasH / (depth * 2 * tanHalfFov).
                    // Here we use a 60° default; small inaccuracy is acceptable for
                    // ConstantPixel because users tweak baseSize anyway.
                    constexpr float kHalfFovTan = 0.5773f; // tan(30°), matches engine default 60°
                    const float pixelsPerWorld =
                        (view.canvasSize.y * 0.5f) / (depth * kHalfFovTan);
                    if (pixelsPerWorld > 0.0001f)
                        c.computedScale = 1.0f / pixelsPerWorld;
                    else
                        c.computedScale = 1.0f;
                }
                else // ConstantWorld
                {
                    c.computedScale = 1.0f;
                }

                // Soft fade — smoothstep-ish 10% bands at the endpoints
                // so things appear/disappear gracefully instead of popping.
                float alpha = 1.0f;
                if (c.fadeFar > 0.f)
                {
                    const float band = std::max(0.5f, c.fadeFar * 0.1f);
                    if (depth > c.fadeFar - band)
                        alpha *= std::max(0.f, (c.fadeFar - depth) / band);
                }
                if (c.fadeNear > 0.f)
                {
                    const float band = std::max(0.5f, c.fadeNear * 0.5f);
                    if (depth < c.fadeNear + band)
                        alpha *= std::max(0.f, (depth - c.fadeNear) / band);
                }
                c.computedAlpha = alpha;
                c.isCulled      = false;
            }
        }

        // ---- DamageNumber lifecycle -------------------------------------
        // Integrate velocity into currentOffset, decrement lifetime, fade
        // alpha through computedAlpha multiplier, destroy expired entries.
        if (auto* dnPool = world.GetPool<DamageNumberComponent>())
        {
            const auto& ents = dnPool->Entities();
            auto&       data = dnPool->Data();
            std::vector<Entity> expired;
            expired.reserve(8);

            for (size_t i = 0; i < data.size(); ++i)
            {
                DamageNumberComponent& dn = data[i];
                dn.lifetime -= dt;
                dn.currentOffset.x += dn.velocity.x * dt;
                dn.currentOffset.y += dn.velocity.y * dt;
                dn.currentOffset.z += dn.velocity.z * dt;

                if (dn.lifetime <= 0.f)
                {
                    expired.push_back(ents[i]);
                    continue;
                }

                // Fade in last 30% of lifetime — multiply into the
                // WorldSpaceUI's computedAlpha.
                if (auto* ws = world.GetComponent<WorldSpaceUIComponent>(ents[i]))
                {
                    const float t = dn.lifetime / std::max(0.001f, dn.totalLifetime);
                    const float fadeIn = std::min(1.0f, (1.0f - t) * 6.0f); // pop-in over first 16%
                    const float fadeOut = std::clamp(t / 0.3f, 0.0f, 1.0f);  // dissolve last 30%
                    ws->computedAlpha *= std::min(fadeIn, fadeOut);
                }
            }

            for (Entity e : expired)
                world.DestroyEntity(e);
        }
    }

} // namespace UI
