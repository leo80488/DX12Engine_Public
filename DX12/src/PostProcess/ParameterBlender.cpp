#include "PostProcess/ParameterBlender.h"

namespace PostProcess
{

namespace
{
    inline float Lerp(float a, float b, float t) { return a + t * (b - a); }

    // Numeric whole-struct lerp for ColorGradingParams: the struct is entirely
    // `float` / `XMFLOAT3` + padding, so treating it as a float array blends
    // every field (including padding, which is harmless) in one loop.
    ColorGradingParams LerpGrading(const ColorGradingParams& a,
                                    const ColorGradingParams& b, float t)
    {
        ColorGradingParams r;
        const float* pa = reinterpret_cast<const float*>(&a);
        const float* pb = reinterpret_cast<const float*>(&b);
        float*       pr = reinterpret_cast<float*>(&r);
        constexpr size_t n = sizeof(ColorGradingParams) / sizeof(float);
        for (size_t i = 0; i < n; ++i)
            pr[i] = Lerp(pa[i], pb[i], t);
        return r;
    }

    // Bool resolution rule: snap to the override side once weight crosses
    // 0.5 so toggles feel decisive rather than randomly flipping mid-blend.
    inline bool LerpBool(bool a, bool b, float t) { return (t >= 0.5f) ? b : a; }

    void BlendStage(CASParams& out, const CASParams& o, float t)
    {
        out.enabled   = LerpBool(out.enabled, o.enabled, t);
        out.sharpness = Lerp(out.sharpness, o.sharpness, t);
    }

    void BlendStage(AutoExposureParams& out, const AutoExposureParams& o, float t)
    {
        out.enabled        = LerpBool(out.enabled, o.enabled, t);
        out.manualExposure = Lerp(out.manualExposure, o.manualExposure, t);
        out.adaptationTau  = Lerp(out.adaptationTau,  o.adaptationTau,  t);
        out.minLogLuma     = Lerp(out.minLogLuma,     o.minLogLuma,     t);
        out.maxLogLuma     = Lerp(out.maxLogLuma,     o.maxLogLuma,     t);
        out.lowPercent     = Lerp(out.lowPercent,     o.lowPercent,     t);
        out.highPercent    = Lerp(out.highPercent,    o.highPercent,    t);
        out.minExposure    = Lerp(out.minExposure,    o.minExposure,    t);
        out.maxExposure    = Lerp(out.maxExposure,    o.maxExposure,    t);
        out.evBias         = Lerp(out.evBias,         o.evBias,         t);
        out.keyValue       = Lerp(out.keyValue,       o.keyValue,       t);
    }

    void BlendStage(BloomParams&, const BloomParams&, float)
    {
        // BloomParams has no fields yet.
    }

    void BlendStage(TonemappingParams& out, const TonemappingParams& o, float t)
    {
        out.bloomStrength       = Lerp(out.bloomStrength, o.bloomStrength, t);
        out.colorGradingEnabled = LerpBool(out.colorGradingEnabled, o.colorGradingEnabled, t);
        out.grading             = LerpGrading(out.grading, o.grading, t);
    }
}

void ParameterBlender::Blend(const ParameterStore& base,
                             const std::vector<Snapshot>& snapshots,
                             ParameterStore& out) const
{
    // Start from base. Each snapshot lerps toward its volume's override by
    // the snapshot weight. Snapshots arrive in priority-ascending order, so
    // high-priority volumes blend last and effectively dominate.
    out.GetCAS()          = base.GetCAS();
    out.GetAutoExposure() = base.GetAutoExposure();
    out.GetBloom()        = base.GetBloom();
    out.GetTonemapping()  = base.GetTonemapping();

    for (const auto& s : snapshots)
    {
        if (!s.override || s.weight <= 0.0f) continue;
        const VolumeOverride& ov = *s.override;
        const float t = s.weight;

        if (ov.cas)          BlendStage(out.GetCAS(),          *ov.cas,          t);
        if (ov.autoExposure) BlendStage(out.GetAutoExposure(), *ov.autoExposure, t);
        if (ov.bloom)        BlendStage(out.GetBloom(),        *ov.bloom,        t);
        if (ov.tonemapping)  BlendStage(out.GetTonemapping(),  *ov.tonemapping,  t);
    }
}

} // namespace PostProcess
