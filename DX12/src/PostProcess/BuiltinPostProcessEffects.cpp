#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "RenderGraph/RenderPass/DepthOfFieldPass.h"
#include "RenderGraph/RenderPass/StylizePass.h"

#include <cmath>

namespace PostProcess
{

// ===========================================================================
// DepthOfField — focus-distance CoC blur (HDR space producer; needs depth)
// ===========================================================================
bool DepthOfFieldEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0 || ctx.depthSrv == 0) return false;
    return ctx.resolved->dof.enabled;
}

void DepthOfFieldEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->dof;

    m_pass->SetEnabled(p.enabled);
    m_pass->focusDistance   = p.focusDistance;
    m_pass->focusRange      = p.focusRange;
    m_pass->transitionRange = p.transitionRange;
    m_pass->maxRadius       = p.maxRadius;

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetDepthSrv(ctx.depthSrv);
    m_pass->SetCameraPlanes(ctx.cameraNear, ctx.cameraFar);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ===========================================================================
// CAS — AMD FidelityFX Contrast Adaptive Sharpening (HDR space)
// ===========================================================================
bool CASEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    return ctx.resolved->cas.enabled;
}

void CASEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->cas;

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
    return m_pass != nullptr && ctx.resolved != nullptr;
}

void AutoExposureEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->autoExposure;

    m_pass->SetEnabled(p.enabled);
    m_pass->SetManualExposure(p.manualExposure);
    m_pass->SetLuminanceRange(p.minLogLuma, p.maxLogLuma);
    m_pass->SetPercentiles(p.lowPercent, p.highPercent);
    m_pass->SetMinExposure(p.minExposure);
    m_pass->SetMaxExposure(p.maxExposure);
    m_pass->SetEVBias(p.evBias);
    m_pass->SetKeyValue(p.keyValue);

    // Frame-rate-independent exponential adaptation. rate = 1 - exp(-dt/tau)
    // is the standard one-pole smoothing coefficient; tau is user-tunable.
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
    // Bloom always produces its chain; the composite weight (bloom.strength)
    // is applied downstream in Tonemapping, so gate only on input availability.
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
    if (ctx.resolved)
    {
        const auto& p = ctx.resolved->lensFlare;
        // Combine the artistic master toggle with the runtime sun-visibility
        // gate the Renderer already applied (sun-below-horizon / behind camera).
        m_pass->SetEnabled(p.enabled && m_pass->IsEnabled());
        m_pass->SetIntensity(p.intensity);
        m_pass->SetGhostCount(p.ghostCount);
        m_pass->SetGhostDispersal(p.ghostDispersal);
        m_pass->SetHaloWidth(p.haloWidth);
        m_pass->SetStreakLength(p.streakLength);
        m_pass->SetChromaticOffset(p.chromaticOffset);
    }

    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    ctx.lensFlareSrv = m_pass->GetSrvHandle();
}

// ===========================================================================
// Underwater — screen distortion + water tint (HDR space producer)
// ===========================================================================
bool UnderwaterEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    return ctx.resolved->underwater.enabled;
}

void UnderwaterEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->underwater;

    m_pass->SetEnabled(p.enabled);
    m_pass->strength   = p.strength;
    m_pass->scale      = p.scale;
    m_pass->speed      = p.speed;
    m_pass->tint       = p.tint;
    m_pass->tintAmount = p.tintAmount;
    m_pass->AddTime(ctx.deltaTime);   // animate the wobble

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ===========================================================================
// Stylize — NPR (Kuwahara/Posterize/Halftone/Dither/Crosshatch), HDR producer
// ===========================================================================
bool StylizeEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    const auto& r = *ctx.resolved;
    return r.pixelate.enabled  || r.kuwahara.enabled || r.posterize.enabled
        || r.halftone.enabled  || r.dither.enabled   || r.crosshatch.enabled;
}

void StylizeEffect::Execute(Context& ctx)
{
    const auto& r = *ctx.resolved;

    m_pass->pixelateOn          = r.pixelate.enabled;
    m_pass->pixelSize           = r.pixelate.size;
    m_pass->kuwaharaOn          = r.kuwahara.enabled;
    m_pass->kuwaharaRadius      = r.kuwahara.radius;
    m_pass->posterizeOn         = r.posterize.enabled;
    m_pass->posterizeLevels     = r.posterize.levels;
    m_pass->halftoneOn          = r.halftone.enabled;
    m_pass->halftoneCell        = r.halftone.cellSize;
    m_pass->halftoneAngle       = r.halftone.angle;
    m_pass->ditherOn            = r.dither.enabled;
    m_pass->ditherLevels        = r.dither.levels;
    m_pass->crosshatchOn        = r.crosshatch.enabled;
    m_pass->crosshatchDensity   = r.crosshatch.density;
    m_pass->crosshatchThickness = r.crosshatch.thickness;
    m_pass->SetEnabled(true);   // IsEnabled() already verified at least one feature

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ===========================================================================
// Tonemapping — ACES + baked 3D LUT color grading (HDR → LDR)
// ===========================================================================
bool ToneMapEffect::IsEnabled(const Context& ctx) const
{
    // Always runs — it's the HDR→LDR crossing point.
    return m_pass != nullptr && ctx.resolved != nullptr;
}

void ToneMapEffect::Execute(Context& ctx)
{
    const auto& cg = ctx.resolved->colorGrading;

    m_pass->SetBloomStrength(ctx.resolved->bloom.strength);
    m_pass->SetColorGradingEnabled(cg.enabled);
    // SetColorGradingParams is cheap when the value is unchanged (internal
    // equality check controls the LUT-dirty flag), so pushing every frame
    // is safe.
    m_pass->SetColorGradingParams(ToColorGradingParams(cg));

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
