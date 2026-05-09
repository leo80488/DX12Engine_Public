#pragma once

// BeamComponent — ECS marker for an entity that should be rendered as a
// procedural-tube heavy beam. The Renderer pairs the component with a
// BeamSystem slot (acquired on first sight, released on entity destroy)
// and uploads the control points + params to the GPU each frame.
//
// Drawing requires the entity also carry:
//   - GlobalTransform (entity world matrix; usually identity for beams whose
//                      control points are already world-space)
//   - MaterialComponent (drives shading via `useCustomShader = true` +
//                        `customShaderPath` pointing at e.g.
//                        "BeamTube_InnerCore.ps.hlsl"; userBlendMode picks
//                        opaque inner-core or additive outer-glow path)

#include "ECS/ECS.h"
#include <vector>
#include <cstdint>
#include <DirectXMath.h>

// CPU-side mirror of shaders/BeamCommon.hlsli BeamControlPoint.
// Same bit layout as Graphics/BeamSystem.h BeamControlPointGPU; we keep this
// type here so gameplay code can build beams without including engine
// internals.
struct BeamControlPoint
{
    DirectX::XMFLOAT3 position { 0, 0, 0 };
    float             radius   { 0.2f };
    DirectX::XMFLOAT4 colorTint{ 1, 1, 1, 1 };
};

struct BeamComponent
{
    // Author-supplied polyline (>= 2 points). World-space when entity transform
    // is identity (recommended); otherwise local-space → entity world matrix
    // transforms in VS.
    std::vector<BeamControlPoint> controlPoints;

    // Global multiplier on each control point's radius (lets the gameplay
    // animate "charge-up → fire" without rebuilding the whole list).
    float globalRadiusScale = 1.0f;

    // Perpendicular noise wobble (set 0 for a still beam). Amplitude is in
    // metres; speed multiplies the time argument before sampling the noise.
    float wobbleAmplitude   = 0.0f;
    float wobbleSpeed       = 1.0f;

    // Renderer-internal — DO NOT set from gameplay.
    // BeamSystem slot id; lazily acquired on first sight, released when the
    // component (or its entity) is destroyed.
    mutable uint32_t beamSlot = 0xFFFFFFFFu;
};
