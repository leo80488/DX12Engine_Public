#include "Graphics/ShadowSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

// ---------------------------------------------------------------------------
// ShadowSystem::Update — practical log+uniform split, tight-sphere fit,
// texel-snap stabilization, Halton(2,3) sub-texel jitter.
// Math is preserved exactly from the previous Renderer::UpdateShadowCascades
// (see git history for the drift / acne / shimmer investigations that led
// here — the constants and the `+ i*3` decorrelation are load-bearing for
// TAA integration).
// ---------------------------------------------------------------------------
void ShadowSystem::Update(const FrameInput& in)
{
    m_hasLight = false;

    const XMVECTOR lightDir = XMVector3Normalize(
        XMVectorSet(in.lightDir.x, in.lightDir.y, in.lightDir.z, 0.0f));
    if (XMVector3Equal(lightDir, XMVectorZero()))
        return;

    m_hasLight = true;

    // ---- Practical split scheme (logarithm + uniform blend) --------------
    // Cascades 0..N-2 use the practical split between nearZ and shadowFar.
    // Cascade N-1 is the "ultra-far" cascade, which extends the coverage
    // from shadowFar out to kFarCascadeFar — it doesn't follow the log+
    // uniform formula because we want it to dedicate its texel budget to
    // the long-range macro silhouette, not to the same near-camera detail
    // the smaller cascades already cover.
    const float nearZ     = in.nearZ;
    const float farZ      = in.farZ;
    const float shadowFar = std::min(farZ, kShadowFarCap);
    const int   nearCount = kCascadeCount - 1;   // 3 in the current 4-cascade setup

    float splits[kCascadeCount + 1];
    splits[0] = nearZ;
    for (int i = 1; i < nearCount; ++i)
    {
        const float fi       = static_cast<float>(i) / static_cast<float>(nearCount);
        const float splitLog = nearZ * std::pow(shadowFar / nearZ, fi);
        const float splitUni = nearZ + (shadowFar - nearZ) * fi;
        splits[i] = kLambda * splitLog + (1.0f - kLambda) * splitUni;
    }
    splits[nearCount]     = shadowFar;       // boundary between near cascades and far cascade
    splits[kCascadeCount] = kFarCascadeFar;  // ultra-far cascade upper limit

    for (int i = 0; i < kCascadeCount; ++i)
        m_splits[i] = splits[i + 1];

    // Blend range is sized off the near-cascade span; the near→far cascade
    // boundary uses its own (wider) range so the resolution drop between
    // cascade 2 and cascade 3 doesn't read as a hard line on terrain.
    m_shadowBlendRange = (shadowFar - nearZ) * 0.08f;

    // Bump once per Update so `shadowFrameIndex + i*3` decorrelation in the
    // shader's PCF rotation stays in phase across cascades. (Previously this
    // was a file-static counter inside Renderer::UpdateShadowCascades.)
    const uint32_t frameIdx = m_shadowFrameIndex++;

    // ---- Per-cascade ortho light VP --------------------------------------
    for (int i = 0; i < kCascadeCount; ++i)
    {
        const float subNear = splits[i];
        const float subFar  = splits[i + 1];

        // Reversed Z (swapped near/far) — this projection is only used for
        // extracting the sub-frustum corners in world space.
        XMMATRIX subProj = XMMatrixPerspectiveFovLH(in.fov, in.aspect, subFar, subNear);
        XMMATRIX invVP   = XMMatrixInverse(nullptr, XMMatrixMultiply(in.view, subProj));

        static const XMVECTOR ndcCorners[8] = {
            { -1, -1, 0, 1 }, {  1, -1, 0, 1 }, {  1,  1, 0, 1 }, { -1,  1, 0, 1 },
            { -1, -1, 1, 1 }, {  1, -1, 1, 1 }, {  1,  1, 1, 1 }, { -1,  1, 1, 1 },
        };

        XMVECTOR frustumCenter = XMVectorZero();
        XMVECTOR worldCorners[8];
        for (int c = 0; c < 8; ++c)
        {
            XMVECTOR wc = XMVector4Transform(ndcCorners[c], invVP);
            wc = XMVectorScale(wc, 1.0f / XMVectorGetW(wc));
            worldCorners[c] = wc;
            frustumCenter   = XMVectorAdd(frustumCenter, wc);
        }
        frustumCenter = XMVectorScale(frustumCenter, 1.0f / 8.0f);

        float radius = 0.0f;
        for (int c = 0; c < 8; ++c)
        {
            const float d = XMVectorGetX(XMVector3Length(
                XMVectorSubtract(worldCorners[c], frustumCenter)));
            radius = std::max(radius, d);
        }

        const float smSize    = static_cast<float>(in.shadowMapSize);
        const float texelSize = (2.0f * radius) / smSize;
        m_texelWorld[i] = texelSize;  // exposed for consumers; radius itself
                                      // is left raw — tight-sphere fit is
                                      // rotation-invariant and translation-
                                      // invariant, so it's naturally stable
                                      // frame-to-frame. Quantizing it adds
                                      // discrete jumps every time the radius
                                      // crosses a bucket boundary — visible
                                      // as a full-cascade texel-grid rescale.

        const XMVECTOR lightPos = XMVectorSubtract(
            frustumCenter, XMVectorScale(lightDir, radius + kPullBack));

        XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        if (std::abs(XMVectorGetY(lightDir)) > 0.999f)
            up = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, frustumCenter, up);

        XMMATRIX lightProj = XMMatrixOrthographicLH(
            2.0f * radius, 2.0f * radius,
            2.0f * radius + kPullBack * 2.0f, 0.0f);

        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);

        // Texel-snap only. The shadow edge moves in integer-texel increments
        // when camera / sun move, which is stable under a static pixel — TAA
        // has nothing to disocclude. A previous version added Halton sub-
        // texel jitter on top to try to soften the integer-texel steps via
        // TAA integration, but sub-texel shifts of a *static* edge produce
        // per-frame value changes that TAA's neighborhood clamp rejects (no
        // motion vector to follow), so the jitter read as shimmer instead
        // of soft penumbra. Spatial-only PCF noise (per-pixel Bayer rotation
        // in shadow.hlsli) handles softening without the TAA conflict.
        {
            XMVECTOR shadowOrigin = XMVectorSet(0.f, 0.f, 0.f, 1.f);
            shadowOrigin = XMVector4Transform(shadowOrigin, lightVP);
            shadowOrigin = XMVectorScale(shadowOrigin, smSize * 0.5f);

            XMVECTOR rounded = XMVectorRound(shadowOrigin);
            XMVECTOR offset  = XMVectorSubtract(rounded, shadowOrigin);
            offset = XMVectorScale(offset, 2.0f / smSize);
            offset = XMVectorSetZ(offset, 0.f);
            offset = XMVectorSetW(offset, 0.f);

            lightProj.r[3] = XMVectorAdd(lightProj.r[3], offset);
            lightVP        = XMMatrixMultiply(lightView, lightProj);
        }
        (void)frameIdx;

        XMStoreFloat4x4(&m_matricesForPass[i],     lightVP);
        XMStoreFloat4x4(&m_matricesForLightCB[i],  XMMatrixTranspose(lightVP));
    }
}
