#include "Graphics/ShadowFrustumCompute.h"

#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderPass/ShadowPass.h"

#include <algorithm>
#include <cmath>

// LightCB shape mirrors the one defined inside Renderer.cpp. The fields this
// file touches (shadowMatrix, cascadeSplits, cameraForward, shadowBias,
// shadowStrength, shadowBlendRange, shadowFrameIndex, shadowMapTexelSize,
// lightDir) must match Lighting.ps / Shadow.ps expectations.
namespace
{
    struct LightCBLayout
    {
        float     lightDir[4];
        float     lightColor[4];
        // ... truncated — only field offsets that ShadowFrustumCompute uses are
        //     referenced below via reinterpret_cast. Keep this file decoupled
        //     from the Renderer-side struct; the pointer we receive is already
        //     cast by the caller.
    };
}

using namespace DirectX;

bool ShadowFrustumCompute::AabbVs6Planes(const BoundingBox& bb,
                                         const XMVECTOR (&planes)[6])
{
    const XMVECTOR center  = XMLoadFloat3(&bb.Center);
    const XMVECTOR extents = XMLoadFloat3(&bb.Extents);
    for (int i = 0; i < 6; ++i)
    {
        const XMVECTOR p    = planes[i];
        const XMVECTOR absN = XMVectorAbs(p);
        const float    d    = XMVectorGetX(XMVector3Dot(p, center)) + XMVectorGetW(p);
        const float    r    = XMVectorGetX(XMVector3Dot(absN, extents));
        if (d - r > 0.f) return false;
    }
    return true;
}

bool ShadowFrustumCompute::IntersectsAny(const BoundingBox& bb) const
{
    if (!m_valid) return false;
    for (int sc = 0; sc < kCascadeCount; ++sc)
    {
        if (!m_aabb[sc].Intersects(bb)) continue;
        if (AabbVs6Planes(bb, m_planes[sc].planes)) return true;
    }
    return false;
}

bool ShadowFrustumCompute::Compute(World& world,
                                    const RenderCamera& cam,
                                    uint32_t vpW,
                                    uint32_t vpH)
{
    m_valid = false;
    if (vpW == 0 || vpH == 0) return false;

    const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&cam.forward));
    const XMVECTOR camPos  = XMLoadFloat3(&cam.position);
    const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMMATRIX view = XMMatrixLookToLH(camPos, forward, up);

    XMVECTOR lightDir = XMVectorZero();
    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        const LightData* ld = world.GetComponent<LightData>(e);
        if (!ld || ld->type != LightType::Directional) continue;
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (gt)
        {
            XMVECTOR localDir = XMLoadFloat3(&ld->direction);
            lightDir = XMVector3Normalize(
                XMVector3TransformNormal(localDir, XMLoadFloat4x4(&gt->matrix)));
        }
        else
        {
            lightDir = XMVector3Normalize(XMLoadFloat3(&ld->direction));
        }
        break;
    }
    if (XMVector3Equal(lightDir, XMVectorZero())) return false;

    static constexpr int   kCascades = kCascadeCount;
    static constexpr float kLambda   = 0.85f;
    const float nearZ     = cam.nearZ;
    const float shadowFar = (std::min)(cam.farZ, 400.0f);
    const float aspect    = static_cast<float>(vpW) / static_cast<float>(vpH);

    float splits[kCascades + 1];
    splits[0] = nearZ;
    for (int i = 1; i < kCascades; ++i)
    {
        const float fi = static_cast<float>(i) / static_cast<float>(kCascades);
        splits[i] = kLambda * nearZ * std::pow(shadowFar / nearZ, fi)
                  + (1.0f - kLambda) * (nearZ + (shadowFar - nearZ) * fi);
    }
    splits[kCascades] = shadowFar;

    for (int i = 0; i < kCascades; ++i)
    {
        XMMATRIX subProj = XMMatrixPerspectiveFovLH(
            cam.fov, aspect, splits[i + 1], splits[i]);
        XMMATRIX invVP = XMMatrixInverse(nullptr, XMMatrixMultiply(view, subProj));

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
            radius = (std::max)(radius, d);
        }
        const float texelSize = (2.0f * radius)
            / static_cast<float>(ShadowPass::kShadowMapSize);
        radius = std::ceil(radius / texelSize) * texelSize;

        const float pullBack = 300.0f;
        const XMVECTOR lightPos = XMVectorSubtract(
            frustumCenter, XMVectorScale(lightDir, radius + pullBack));

        XMVECTOR upVec = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        if (std::abs(XMVectorGetY(lightDir)) > 0.999f)
            upVec = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, frustumCenter, upVec);
        XMMATRIX lightProj = XMMatrixOrthographicLH(
            2.0f * radius, 2.0f * radius,
            2.0f * radius + pullBack * 2.0f, 0.0f);
        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);

        XMMATRIX invLightVP = XMMatrixInverse(nullptr, lightVP);
        static const XMVECTOR clipCorners[8] = {
            {-1,-1, 0, 1}, { 1,-1, 0, 1}, {-1, 1, 0, 1}, { 1, 1, 0, 1},
            {-1,-1, 1, 1}, { 1,-1, 1, 1}, {-1, 1, 1, 1}, { 1, 1, 1, 1},
        };
        XMFLOAT3 obbPts[8];
        for (int c = 0; c < 8; ++c)
        {
            XMVECTOR wc = XMVector4Transform(clipCorners[c], invLightVP);
            wc = XMVectorScale(wc, 1.0f / XMVectorGetW(wc));
            XMStoreFloat3(&obbPts[c], wc);
        }
        BoundingOrientedBox::CreateFromPoints(m_obb[i],  8, obbPts, sizeof(XMFLOAT3));
        BoundingBox::CreateFromPoints(        m_aabb[i], 8, obbPts, sizeof(XMFLOAT3));

        auto buildPlane = [&](int a, int b, int c, int ref) -> XMVECTOR
        {
            const XMVECTOR va = XMLoadFloat3(&obbPts[a]);
            const XMVECTOR vb = XMLoadFloat3(&obbPts[b]);
            const XMVECTOR vc = XMLoadFloat3(&obbPts[c]);
            XMVECTOR n = XMVector3Normalize(
                XMVector3Cross(XMVectorSubtract(vb, va),
                               XMVectorSubtract(vc, va)));
            const XMVECTOR vRef = XMLoadFloat3(&obbPts[ref]);
            if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(vRef, va))) > 0.f)
                n = XMVectorNegate(n);
            const float d = -XMVectorGetX(XMVector3Dot(n, va));
            return XMVectorSetW(n, d);
        };
        m_planes[i].planes[0] = buildPlane(0, 1, 2, 4);
        m_planes[i].planes[1] = buildPlane(4, 6, 5, 0);
        m_planes[i].planes[2] = buildPlane(0, 2, 4, 1);
        m_planes[i].planes[3] = buildPlane(1, 5, 3, 0);
        m_planes[i].planes[4] = buildPlane(2, 3, 6, 0);
        m_planes[i].planes[5] = buildPlane(0, 4, 1, 2);
    }

    m_valid = true;
    return true;
}
