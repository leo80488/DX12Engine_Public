#pragma once

// PostProcess::PostProcessProfile — a shared, authored look made entirely of
// Overridable<T> properties. It is a Resource (referenced by ProfileHandle,
// serialized to a .ppprofile asset), NOT embedded per-volume: many volumes can
// reference the same profile, and the engine-default profile is one too.
//
// The member list of each *Settings group below is mirrored 1:1 by the flat
// ResolvedPostProcessSettings, and both are cross-checked at compile time by
// the X-macro in PostProcessProperties.inl (BlendProfileInto / Flatten /
// MakeEngineDefaultProfile reference every member by name).
//
// Field semantics match the wrapped passes:
//   cas          -> CASPass
//   autoExposure -> AutoExposurePass (histogram model)
//   bloom        -> ToneMapPass bloom composite weight
//   colorGrading -> ToneMapPass 3D LUT bake (ColorGradingParams)
//   lensFlare    -> LensFlarePass

#include "PostProcess/Overridable.h"
#include "Resource/ResourceHandle.h"

#include <DirectXMath.h>

namespace PostProcess
{

// Generational handle to a PostProcessProfile resource (see ProfileSystem).
using ProfileHandle = Resource::Handle;

struct CASSettings
{
    Overridable<bool>  enabled;
    Overridable<float> sharpness;
};

struct AutoExposureSettings
{
    Overridable<bool>  enabled;
    Overridable<float> manualExposure;
    Overridable<float> adaptationTau;
    Overridable<float> minLogLuma;
    Overridable<float> maxLogLuma;
    Overridable<float> lowPercent;
    Overridable<float> highPercent;
    Overridable<float> minExposure;
    Overridable<float> maxExposure;
    Overridable<float> evBias;
    Overridable<float> keyValue;
};

struct BloomSettings
{
    Overridable<float> strength;
};

struct ColorGradingSettings
{
    Overridable<bool>              enabled;
    Overridable<float>             exposure;
    Overridable<float>             contrast;
    Overridable<float>             brightness;
    Overridable<DirectX::XMFLOAT3> lift;
    Overridable<DirectX::XMFLOAT3> gamma;
    Overridable<DirectX::XMFLOAT3> gain;
    Overridable<float>             hueShift;
    Overridable<float>             saturation;
    Overridable<float>             vibrance;
    Overridable<float>             temperature;
    Overridable<float>             tint;
    Overridable<float>             vignetteStrength;
    Overridable<float>             filmGrain;
};

struct LensFlareSettings
{
    Overridable<bool>     enabled;
    Overridable<float>    intensity;
    Overridable<uint32_t> ghostCount;
    Overridable<float>    ghostDispersal;
    Overridable<float>    haloWidth;
    Overridable<float>    streakLength;
    Overridable<float>    chromaticOffset;
};

struct UnderwaterSettings
{
    Overridable<bool>              enabled;
    Overridable<float>             strength;
    Overridable<float>             scale;
    Overridable<float>             speed;
    Overridable<DirectX::XMFLOAT3> tint;
    Overridable<float>             tintAmount;
};

struct DepthOfFieldSettings
{
    Overridable<bool>  enabled;
    Overridable<float> focusDistance;
    Overridable<float> focusRange;
    Overridable<float> transitionRange;
    Overridable<float> maxRadius;
};

struct PixelateSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> size;
};

struct KuwaharaSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> radius;
};

struct PosterizeSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> levels;
};

struct HalftoneSettings
{
    Overridable<bool>  enabled;
    Overridable<float> cellSize;
    Overridable<float> angle;
};

struct DitherSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> levels;
};

struct CrosshatchSettings
{
    Overridable<bool>  enabled;
    Overridable<float> density;
    Overridable<float> thickness;
};

// The shared resource. Defaulted members override nothing (overrideState=false)
// — exactly what a freshly-created bounded volume wants. The engine default is
// produced by MakeEngineDefaultProfile() (all properties overriding).
struct PostProcessProfile
{
    CASSettings          cas;
    AutoExposureSettings autoExposure;
    BloomSettings        bloom;
    ColorGradingSettings colorGrading;
    LensFlareSettings    lensFlare;
    UnderwaterSettings   underwater;
    DepthOfFieldSettings dof;
    PixelateSettings     pixelate;
    KuwaharaSettings     kuwahara;
    PosterizeSettings    posterize;
    HalftoneSettings     halftone;
    DitherSettings       dither;
    CrosshatchSettings   crosshatch;

    // True if at least one property in the profile overrides. Used to skip
    // volumes that contribute nothing.
    bool HasAnyOverride() const;
};

// The base of every resolve: a profile with EVERY property overriding and set
// to the engine's shipping defaults (values come straight from the X-macro).
inline PostProcessProfile MakeEngineDefaultProfile()
{
    PostProcessProfile p;
#define PP_BOOL(g, m, l, d)            p.g.m.value = (d);                              p.g.m.overrideState = true;
#define PP_FLOAT(g, m, l, d, a, b)     p.g.m.value = (d);                              p.g.m.overrideState = true;
#define PP_FLOAT3(g, m, l, dx, dy, dz) p.g.m.value = DirectX::XMFLOAT3{dx, dy, dz};    p.g.m.overrideState = true;
#define PP_COLOR(g, m, l, dr, dg, db)  p.g.m.value = DirectX::XMFLOAT3{dr, dg, db};    p.g.m.overrideState = true;
#define PP_UINT(g, m, l, d, a, b)      p.g.m.value = static_cast<uint32_t>(d);         p.g.m.overrideState = true;
#include "PostProcess/PostProcessProperties.inl"
    return p;
}

inline bool PostProcessProfile::HasAnyOverride() const
{
    const PostProcessProfile& p = *this;
    bool any = false;
#define PP_BOOL(g, m, l, d)            any = any || p.g.m.overrideState;
#define PP_FLOAT(g, m, l, d, a, b)     any = any || p.g.m.overrideState;
#define PP_FLOAT3(g, m, l, dx, dy, dz) any = any || p.g.m.overrideState;
#define PP_COLOR(g, m, l, dr, dg, db)  any = any || p.g.m.overrideState;
#define PP_UINT(g, m, l, d, a, b)      any = any || p.g.m.overrideState;
#include "PostProcess/PostProcessProperties.inl"
    return any;
}

} // namespace PostProcess
