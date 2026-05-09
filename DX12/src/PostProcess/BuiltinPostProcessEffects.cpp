#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/ParameterStore.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"

#include <cmath>

namespace PostProcess
{

// ===========================================================================
// CAS — AMD FidelityFX Contrast Adaptive Sharpening (HDR space)
// ===========================================================================
bool CASEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.params || ctx.hdrSrv == 0) return false;
    return ctx.params->GetCAS().enabled;
}

void CASEffect::Execute(Context& ctx)
{
    const CASParams& p = ctx.params->GetCAS();

    // Push store → pass. Pass state becomes a write-through cache of the most
    // recent store value; downstream pass->Get accessors remain coherent.
    m_pass->SetEnabled(p.enabled);
    m_pass->sharpness = p.sharpness;  // public field, not a setter

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ===========================================================================
// AutoExposure — histogram EV (always runs; internal branch handles disabled)
// ===========================================================================
bool AutoExposureEffect::IsEnabled(const Context& ctx) const
{
    // Pass runs every frame even when auto-exposure is user-disabled: in that
    // case Execute() uploads `manualExposure` into the exposure buffer so
    // downstream Tonemapping never reads stale data.
    return m_pass != nullptr && ctx.params != nullptr;
}

void AutoExposureEffect::Execute(Context& ctx)
{
    const AutoExposureParams& p = ctx.params->GetAutoExposure();

    m_pass->SetEnabled(p.enabled);
    m_pass->SetManualExposure(p.manualExposure);
    m_pass->SetLuminanceRange(p.minLogLuma, p.maxLogLuma);
    m_pass->SetPercentiles(p.lowPercent, p.highPercent);
    m_pass->SetMinExposure(p.minExposure);
    m_pass->SetMaxExposure(p.maxExposure);
    m_pass->SetEVBias(p.evBias);
    m_pass->SetKeyValue(p.keyValue);

    // Frame-rate-independent exponential adaptation. rate = 1 - exp(-dt/tau)
    // is the standard one-pole smoothing coefficient; tau is user-tunable via
    // AutoExposureParams::adaptationTau.
    const float tau  = (p.adaptationTau > 0.0f) ? p.adaptationTau : 1.0e-4f;
    const float rate = (ctx.deltaTime > 0.0f)
        ? 1.0f - std::expf(-ctx.deltaTime / tau)
        : 0.0f;
    m_pass->SetAdaptationRate(rate);

    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (ctx.gfx)
        ctx.exposureSrv = ctx.gfx->GetBufferSRVGpuHandle(m_pass->GetExposureBuffer());
}

// ===========================================================================
// Bloom — Sledgehammer downsample/upsample chain
// ===========================================================================
bool BloomEffect::IsEnabled(const Context& ctx) const
{
    // BloomParams has no enable field yet — gate on input availability only.
    return m_pass != nullptr && ctx.hdrSrv != 0;
}

void BloomEffect::Execute(Context& ctx)
{
    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    ctx.bloomSrv = m_pass->GetBloomSrvHandle();
}

// ===========================================================================
// LensFlare — procedural directional-light flare (HDR additive)
// ===========================================================================
bool LensFlareEffect::IsEnabled(const Context& /*ctx*/) const
{
    // Always run when registered; the pass internally early-outs to a black
    // texture when its enabled flag is off / sun is behind the camera, so
    // ToneMapping can bind the SRV unconditionally without artifacts.
    return m_pass != nullptr;
}

void LensFlareEffect::Execute(Context& ctx)
{
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    ctx.lensFlareSrv = m_pass->GetSrvHandle();
}

// ===========================================================================
// Tonemapping — ACES + baked 3D LUT color grading (HDR → LDR)
// ===========================================================================
bool ToneMapEffect::IsEnabled(const Context& ctx) const
{
    // Always runs — it's the HDR→LDR crossing point.
    return m_pass != nullptr && ctx.params != nullptr;
}

void ToneMapEffect::Execute(Context& ctx)
{
    const TonemappingParams& p = ctx.params->GetTonemapping();

    m_pass->SetBloomStrength(p.bloomStrength);
    m_pass->SetColorGradingEnabled(p.colorGradingEnabled);
    // SetColorGradingParams is cheap when the value is unchanged (internal
    // equality check controls the LUT-dirty flag), so pushing every frame
    // is safe.
    m_pass->SetColorGradingParams(p.grading);

    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetBloomSrvHandle(ctx.bloomSrv);
    m_pass->SetExposureSrvHandle(ctx.exposureSrv);
    m_pass->SetLensFlareSrvHandle(ctx.lensFlareSrv);
    // SetViewportSize intentionally skipped — Renderer calls EnsureTexture()
    // on the main thread before kicking the compute worker, which already
    // set vpW/vpH and rebuilt the output texture when the size changed.
    m_pass->Execute(ctx.cl);
}

} // namespace PostProcess
