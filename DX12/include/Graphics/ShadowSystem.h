#pragma once

// ShadowSystem — CPU-side cascaded shadow map (CSM) math.
//
// Owns cascade split computation, per-cascade light view-projection matrix
// construction, texel-grid stabilization, and sub-texel Halton jitter. Does
// NOT own GPU resources — ShadowPass still owns the Texture2DArray, the
// per-cascade upload CBs, the indirect buffer, PSO, and barrier state.
//
// Usage (per frame, on the Renderer thread, before any consumer reads shadow
// state):
//
//     ShadowSystem::FrameInput fi{ view, cameraForward, lightDir,
//                                  nearZ, farZ, fov, aspect, shadowMapSize };
//     shadowSys.Update(fi);
//     if (shadowSys.HasValidLight()) {
//         // Copy CascadeMatricesTransposed() + CascadeSplits() etc. into LightCB.
//     }
//
// Matches the Particle/Trail pattern used elsewhere: System owns data/logic,
// paired Pass owns rendering.

#include <DirectXMath.h>
#include <cstdint>

class IGraphicsDevice;

class ShadowSystem
{
public:
    static constexpr int   kCascadeCount = 4;
    static constexpr float kLambda       = 0.85f;
    // Cascades 0..2 are the standard "near" CSM cascades — practical
    // log+uniform split between camera near plane and kShadowFarCap.
    static constexpr float kShadowFarCap = 400.0f;
    // Cascade 3 is a dedicated "ultra-far" cascade for terrain self-shadow
    // (mountain silhouettes onto the plain at sunrise/sunset, distant
    // peaks shadowing each other). It picks up where cascade 2 ends and
    // extends to kFarCascadeFar in view-Z. Tune higher for bigger worlds.
    static constexpr float kFarCascadeFar = 2000.0f;
    static constexpr float kPullBack     = 300.0f;

    struct FrameInput
    {
        DirectX::XMMATRIX view;
        DirectX::XMFLOAT3 cameraForward;
        DirectX::XMFLOAT3 lightDir;        // caller pulls from LightCB
        float             nearZ  = 0.1f;
        float             farZ   = 1000.0f;
        float             fov    = DirectX::XM_PIDIV4;
        float             aspect = 1.0f;
        uint32_t          shadowMapSize = 4096;
    };

    void Init(IGraphicsDevice& /*gfx*/) {}         // reserved hook; no GPU state today
    void Update(const FrameInput& in);

    const float*                CascadeSplits()             const { return m_splits; }
    const DirectX::XMFLOAT4X4*  CascadeMatricesTransposed() const { return m_matricesForLightCB; }
    const DirectX::XMFLOAT4X4*  CascadeMatricesForPass()    const { return m_matricesForPass; }
    // World-space metres per shadow-map texel for each cascade. Consumers use
    // this to scale normal bias / receiver bias so both stay visually constant
    // across cascades regardless of the cascade's physical extent.
    const float*                CascadeTexelWorldSize()     const { return m_texelWorld; }
    uint32_t                    ShadowFrameIndex()          const { return m_shadowFrameIndex; }
    float                       ShadowBlendRange()          const { return m_shadowBlendRange; }
    bool                        HasValidLight()             const { return m_hasLight; }

    static constexpr float ShadowBias()     { return 0.0003f; }
    static constexpr float ShadowStrength() { return 1.0f; }
    // Receiver-side normal offset strength, in *shadow-map texels of world*.
    // 0 disables. ~1.5 is a sane default — pushes a grazing-angle receiver
    // about one and a half texels along its normal before the light-space
    // sample, which is enough to clear quantisation acne without producing
    // a visible peter-pan gap on most geometry. Read by Lighting.ps via
    // LightCB.shadowNormalOffset.
    static constexpr float ShadowNormalOffset() { return 1.5f; }

private:
    float               m_splits[kCascadeCount]             {};
    float               m_texelWorld[kCascadeCount]         {};
    DirectX::XMFLOAT4X4 m_matricesForLightCB[kCascadeCount] {};
    DirectX::XMFLOAT4X4 m_matricesForPass   [kCascadeCount] {};
    uint32_t            m_shadowFrameIndex = 0;
    float               m_shadowBlendRange = 0.0f;
    bool                m_hasLight         = false;
};
