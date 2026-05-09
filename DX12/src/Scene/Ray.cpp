#include "Scene/Ray.h"
#include "Graphics/RenderTypes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

// ---------------------------------------------------------------------------
Ray Ray::FromViewport(float pixelX, float pixelY,
                      float vpW,    float vpH,
                      const RenderView& view)
{
    // Pixel → NDC (DirectX convention: X right, Y up, Z in [0,1] near→far).
    const float ndcX =  (2.f * pixelX / vpW) - 1.f;
    const float ndcY = -(2.f * pixelY / vpH) + 1.f; // Y flipped

    XMMATRIX viewProj    = XMLoadFloat4x4(&view.viewProjMatrix);
    XMVECTOR det;
    XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);

    // Unproject near (z=0) and far (z=1) clip-space points.
    XMVECTOR nearClip = XMVectorSet(ndcX, ndcY, 0.f, 1.f);
    XMVECTOR farClip  = XMVectorSet(ndcX, ndcY, 1.f, 1.f);

    XMVECTOR worldNear = XMVector4Transform(nearClip, invViewProj);
    XMVECTOR worldFar  = XMVector4Transform(farClip,  invViewProj);

    // Perspective divide.
    worldNear = XMVectorScale(worldNear, 1.f / XMVectorGetW(worldNear));
    worldFar  = XMVectorScale(worldFar,  1.f / XMVectorGetW(worldFar));

    Ray r;
    XMStoreFloat3(&r.origin,    worldNear);
    XMStoreFloat3(&r.direction, XMVector3Normalize(XMVectorSubtract(worldFar, worldNear)));
    return r;
}

// ---------------------------------------------------------------------------
bool Ray::IntersectsAABB(const XMFLOAT3&   localCenter,
                         const XMFLOAT3&   halfExtents,
                         const XMFLOAT4X4& worldMatrix,
                         float& outT) const
{
    // Transform ray into local (object) space so we can test an axis-aligned box.
    XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
    XMVECTOR det;
    XMMATRIX invWorld = XMMatrixInverse(&det, world);

    XMFLOAT3 lo, ld;
    XMStoreFloat3(&lo, XMVector3TransformCoord (XMLoadFloat3(&origin),    invWorld));
    XMStoreFloat3(&ld, XMVector3TransformNormal(XMLoadFloat3(&direction), invWorld));

    // Slab test.
    float tmin = -FLT_MAX;
    float tmax =  FLT_MAX;

    const float* c  = reinterpret_cast<const float*>(&localCenter);
    const float* he = reinterpret_cast<const float*>(&halfExtents);
    const float* o  = reinterpret_cast<const float*>(&lo);
    const float* d  = reinterpret_cast<const float*>(&ld);

    for (int i = 0; i < 3; ++i)
    {
        const float bmin = c[i] - he[i];
        const float bmax = c[i] + he[i];

        if (std::abs(d[i]) < 1e-8f)
        {
            if (o[i] < bmin || o[i] > bmax) return false;
        }
        else
        {
            float t1 = (bmin - o[i]) / d[i];
            float t2 = (bmax - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }

    if (tmax < 0.f) return false;
    outT = (tmin >= 0.f) ? tmin : tmax;
    return true;
}

// ---------------------------------------------------------------------------
bool Ray::IntersectPlane(const XMFLOAT3& planePoint,
                         const XMFLOAT3& planeNormal,
                         float& outT) const
{
    XMVECTOR n   = XMVector3Normalize(XMLoadFloat3(&planeNormal));
    XMVECTOR dir = XMLoadFloat3(&direction);

    const float denom = XMVectorGetX(XMVector3Dot(n, dir));
    if (std::abs(denom) < 1e-6f) return false;

    XMVECTOR diff = XMVectorSubtract(XMLoadFloat3(&planePoint), XMLoadFloat3(&origin));
    outT = XMVectorGetX(XMVector3Dot(diff, n)) / denom;
    return outT >= 0.f;
}

// ---------------------------------------------------------------------------
XMFLOAT3 Ray::At(float t) const
{
    XMFLOAT3 result;
    XMStoreFloat3(&result,
        XMVectorAdd(XMLoadFloat3(&origin),
                    XMVectorScale(XMLoadFloat3(&direction), t)));
    return result;
}
