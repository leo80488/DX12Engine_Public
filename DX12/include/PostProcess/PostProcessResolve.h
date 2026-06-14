#pragma once

// PostProcess resolve — turns the spatial volume set + the gameplay override
// stack into one flat ResolvedPostProcessSettings for a view. This is the §5
// algorithm of DesignMd/PostProcessVolume_Architecture.md:
//
//   result = Flatten(EngineDefaultProfile)
//   gather candidate volumes (layerMask + spatial hit) → effective weight
//   sort by priority ascending
//   for each: BlendProfileInto(result, profile, weight)   // sequential lerp
//   ApplyOverrideStack(result)                            // gameplay layer
//
// It is pure read of ECS state + write of view-local data (PostProcess::Runtime),
// so it is safe to run in the PreRender phase after the camera blend resolves.

#include "PostProcess/ResolvedPostProcessSettings.h"

#include <DirectXMath.h>
#include <cstdint>

class World;

namespace PostProcess
{

// Signed distance (world units) from @p p to the boundary of a volume whose
// bounds come from @p worldMatrix. Negative inside, 0 on the surface, positive
// outside. `isBox` selects OBB vs sphere.
float DistanceToSurface(const DirectX::XMFLOAT4X4& worldMatrix,
                        bool isBox, const DirectX::XMFLOAT3& p);

// Run the full resolve for one view and store the result in
// PostProcess::Runtime::Get().resolved. Advances + prunes the override stack by
// @p dt. @p viewLayerMask is AND-ed against each volume's layerMask.
void ResolveForView(World& world,
                    const DirectX::XMFLOAT3& cameraPos,
                    uint32_t viewLayerMask,
                    float dt);

} // namespace PostProcess
