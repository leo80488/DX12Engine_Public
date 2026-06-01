#include "Resource/PostProcessConfig.h"
#include "Resource/AssetFS.h"

#include "Graphics/Renderer.h"
#include "PostProcess/PostProcessStack.h"
#include "PostProcess/ParameterStore.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "System/Log.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace DirectX;

namespace
{
    // ---------------------------------------------------------------------
    // Tiny line parser: each non-comment line is "Tag key=val key=val ...".
    // Values are plain text (no percent-encoding needed — no strings/paths).
    // ---------------------------------------------------------------------
    struct KV
    {
        std::string tag;
        std::unordered_map<std::string, std::string> map;
    };

    float GetF(const std::unordered_map<std::string, std::string>& m,
               const char* key, float def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }
    int GetI(const std::unordered_map<std::string, std::string>& m,
             const char* key, int def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }
    bool GetB(const std::unordered_map<std::string, std::string>& m,
              const char* key, bool def)
    {
        return GetI(m, key, def ? 1 : 0) != 0;
    }
    XMFLOAT3 GetF3(const std::unordered_map<std::string, std::string>& m,
                   const char* key, XMFLOAT3 def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        XMFLOAT3 v = def;
        sscanf_s(it->second.c_str(), "%f_%f_%f", &v.x, &v.y, &v.z);
        return v;
    }

    bool ParseLine(const std::string& line, KV& out)
    {
        std::istringstream ls(line);
        if (!(ls >> out.tag)) return false;
        std::string tok;
        while (ls >> tok)
        {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            out.map[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
        return true;
    }
}

// =======================================================================
// Capture / Apply
// =======================================================================

void Resource::PostProcessConfig::CaptureFrom(const Renderer& rc)
{
    Renderer& r = const_cast<Renderer&>(rc); // accessors are non-const

    // CAS / Tonemapping / AutoExposure now live in the PostProcess::Stack's
    // ParameterStore — that's the authoritative source, so capture from
    // there instead of per-pass getters.
    if (const auto* stack = r.GetPostProcessStack())
    {
        const auto& params = stack->GetParameters();

        const auto& cas = params.GetCAS();
        casEnabled   = cas.enabled;
        casSharpness = cas.sharpness;

        const auto& tm = params.GetTonemapping();
        bloomStrength       = tm.bloomStrength;
        colorGradingEnabled = tm.colorGradingEnabled;
        grading             = tm.grading;

        const auto& ae = params.GetAutoExposure();
        autoExposureEnabled = ae.enabled;
        manualExposure      = ae.manualExposure;
        adaptationTau       = ae.adaptationTau;
        minLogLuma          = ae.minLogLuma;
        maxLogLuma          = ae.maxLogLuma;
        lowPercent          = ae.lowPercent;
        highPercent         = ae.highPercent;
        minExposure         = ae.minExposure;
        maxExposure         = ae.maxExposure;
        evBias              = ae.evBias;
        keyValue            = ae.keyValue;
    }

    if (auto* gt = r.GetXeGTAOPass())
    {
        gtaoEffectRadius             = gt->effectRadius;
        gtaoEffectFalloff            = gt->effectFalloffRange;   // repurposed: was effectFalloff, now falloff range [0,1]
        gtaoFinalValuePower          = gt->finalValuePower;
        gtaoSampleDistributionPower  = gt->sampleDistributionPower;
        gtaoThinOccluderCompensation = gt->thinOccluderCompensation;
        gtaoDenoiseSigma             = gt->denoiseBlurBeta;      // repurposed: was denoiseSigma, now beta
        gtaoSliceCount               = gt->sliceCount;
        gtaoStepsPerSlice            = gt->stepsPerSlice;
    }
    ssaoEnabled = r.IsSSAOEnabled();

    if (auto* ol = r.GetOutlinePass())
    {
        outlinePixels    = ol->outlinePixels;
        outlineColor     = { ol->outlineColor[0], ol->outlineColor[1], ol->outlineColor[2] };
        depthThreshold   = ol->depthThreshold;
        normalThreshold  = ol->normalThreshold;
        outlineStrength  = ol->outlineStrength;
        outlineFadeStart = ol->outlineFadeStart;
        outlineFadeEnd   = ol->outlineFadeEnd;
    }

    if (auto* vf = r.GetVolumetricFogPass())
    {
        volFogEnabled          = vf->IsEnabled();
        fogDensity             = vf->GetDensity();
        fogScattering          = vf->GetScattering();
        fogAbsorption          = vf->GetAbsorption();
        fogAnisotropy          = vf->GetAnisotropy();
        heightFogStart         = vf->GetHeightStart();
        heightFogFalloff       = vf->GetHeightFalloff();
        froxelNear             = vf->GetRangeNear();
        froxelFar              = vf->GetRangeFar();
        fogAmbientContribution = vf->GetAmbientContribution();
        temporalEnabled        = vf->IsTemporalEnabled();
        temporalAlpha          = vf->GetTemporalAlpha();
        // ambient color has no getter — leave default (set via ApplyTo).
    }

    if (auto* sk = r.GetSkyIBLPass())
    {
        atmosphereEnabled      = sk->IsAtmosphereEnabled();
        skyboxSource           = static_cast<uint32_t>(sk->GetSkyboxSource());
        iblStrength            = sk->GetIBLStrength();
        aerialCompositeEnabled = sk->IsAerialCompositeEnabled();
    }
}

void Resource::PostProcessConfig::ApplyTo(Renderer& r) const
{
    if (auto* stack = r.GetPostProcessStack())
    {
        auto& params = stack->GetParameters();

        auto& cas      = params.GetCAS();
        cas.enabled    = casEnabled;
        cas.sharpness  = casSharpness;

        auto& tm                = params.GetTonemapping();
        tm.bloomStrength        = bloomStrength;
        tm.colorGradingEnabled  = colorGradingEnabled;
        tm.grading              = grading;

        auto& ae            = params.GetAutoExposure();
        ae.enabled          = autoExposureEnabled;
        ae.manualExposure   = manualExposure;
        ae.adaptationTau    = adaptationTau;
        ae.minLogLuma       = minLogLuma;
        ae.maxLogLuma       = maxLogLuma;
        ae.lowPercent       = lowPercent;
        ae.highPercent      = highPercent;
        ae.minExposure      = minExposure;
        ae.maxExposure      = maxExposure;
        ae.evBias           = evBias;
        ae.keyValue         = keyValue;
    }

    r.SetSSAOEnabled(ssaoEnabled);
    if (auto* gt = r.GetXeGTAOPass())
    {
        gt->effectRadius              = gtaoEffectRadius;
        gt->effectFalloffRange        = gtaoEffectFalloff;
        gt->finalValuePower           = gtaoFinalValuePower;
        gt->sampleDistributionPower   = gtaoSampleDistributionPower;
        gt->thinOccluderCompensation  = gtaoThinOccluderCompensation;
        gt->denoiseBlurBeta           = gtaoDenoiseSigma;
        gt->sliceCount                = gtaoSliceCount;
        gt->stepsPerSlice             = gtaoStepsPerSlice;
    }

    if (auto* ol = r.GetOutlinePass())
    {
        ol->outlinePixels    = outlinePixels;
        ol->outlineColor[0]  = outlineColor.x;
        ol->outlineColor[1]  = outlineColor.y;
        ol->outlineColor[2]  = outlineColor.z;
        ol->depthThreshold   = depthThreshold;
        ol->normalThreshold  = normalThreshold;
        ol->outlineStrength  = outlineStrength;
        ol->outlineFadeStart = outlineFadeStart;
        ol->outlineFadeEnd   = outlineFadeEnd;
    }

    if (auto* vf = r.GetVolumetricFogPass())
    {
        vf->SetEnabled(volFogEnabled);
        vf->SetDensity(fogDensity);
        vf->SetScattering(fogScattering);
        vf->SetAbsorption(fogAbsorption);
        vf->SetAnisotropy(fogAnisotropy);
        vf->SetHeight(heightFogStart, heightFogFalloff);
        vf->SetRange(froxelNear, froxelFar);
        vf->SetAmbient(fogAmbientColor, fogAmbientContribution);
        vf->SetTemporalEnabled(temporalEnabled);
        vf->SetTemporalAlpha(temporalAlpha);
    }

    if (auto* sk = r.GetSkyIBLPass())
    {
        sk->SetAtmosphereEnabled(atmosphereEnabled);
        sk->SetSkyboxSource(static_cast<SkyIBLPass::SkyboxSource>(skyboxSource));
        sk->SetIBLStrength(iblStrength);
        sk->SetAerialCompositeEnabled(aerialCompositeEnabled);
    }
}

// =======================================================================
// Save / Load
// =======================================================================

bool Resource::SavePostProcessConfig(const PostProcessConfig& c, const std::string& path)
{
    std::ostringstream ss;
    ss << "# DX12 Engine Post-Process Config (.ippc)\n";
    ss << "PPC version=1\n";

    char buf[512];

    snprintf(buf, sizeof(buf),
        "CAS enabled=%u sharpness=%.6f\n",
        c.casEnabled ? 1u : 0u, c.casSharpness);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "ToneMap bloomStrength=%.6f colorGradingEnabled=%u"
        " exposure=%.6f contrast=%.6f brightness=%.6f"
        " liftR=%.6f liftG=%.6f liftB=%.6f"
        " gammaR=%.6f gammaG=%.6f gammaB=%.6f"
        " gainR=%.6f gainG=%.6f gainB=%.6f"
        " hueShift=%.6f saturation=%.6f vibrance=%.6f"
        " temperature=%.6f tint=%.6f"
        " vignette=%.6f grain=%.6f\n",
        c.bloomStrength, c.colorGradingEnabled ? 1u : 0u,
        c.grading.exposure, c.grading.contrast, c.grading.brightness,
        c.grading.lift.x, c.grading.lift.y, c.grading.lift.z,
        c.grading.gamma.x, c.grading.gamma.y, c.grading.gamma.z,
        c.grading.gain.x, c.grading.gain.y, c.grading.gain.z,
        c.grading.hueShift, c.grading.saturation, c.grading.vibrance,
        c.grading.temperature, c.grading.tint,
        c.grading.vignetteStrength, c.grading.filmGrain);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "AutoExp enabled=%u manual=%.6f adaptTau=%.6f"
        " minLogLuma=%.6f maxLogLuma=%.6f"
        " lowPct=%.6f highPct=%.6f minExp=%.6f maxExp=%.6f"
        " evBias=%.6f keyValue=%.6f\n",
        c.autoExposureEnabled ? 1u : 0u, c.manualExposure,
        c.adaptationTau, c.minLogLuma, c.maxLogLuma,
        c.lowPercent, c.highPercent, c.minExposure, c.maxExposure,
        c.evBias, c.keyValue);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "GTAO enabled=%u radius=%.6f falloff=%.6f finalPower=%.6f"
        " distPower=%.6f thinComp=%.6f denoise=%.6f slices=%u steps=%u\n",
        c.ssaoEnabled ? 1u : 0u,
        c.gtaoEffectRadius, c.gtaoEffectFalloff, c.gtaoFinalValuePower,
        c.gtaoSampleDistributionPower, c.gtaoThinOccluderCompensation,
        c.gtaoDenoiseSigma, c.gtaoSliceCount, c.gtaoStepsPerSlice);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "Outline pixels=%.6f color=%.6f_%.6f_%.6f"
        " depthThr=%.6f normalThr=%.6f strength=%.6f"
        " fadeStart=%.6f fadeEnd=%.6f\n",
        c.outlinePixels,
        c.outlineColor.x, c.outlineColor.y, c.outlineColor.z,
        c.depthThreshold, c.normalThreshold, c.outlineStrength,
        c.outlineFadeStart, c.outlineFadeEnd);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "VolFog enabled=%u density=%.6f scattering=%.6f absorption=%.6f"
        " anisotropy=%.6f heightStart=%.6f heightFalloff=%.6f"
        " froxelNear=%.6f froxelFar=%.6f ambient=%.6f_%.6f_%.6f"
        " ambientContrib=%.6f temporalEnabled=%u temporalAlpha=%.6f\n",
        c.volFogEnabled ? 1u : 0u,
        c.fogDensity, c.fogScattering, c.fogAbsorption, c.fogAnisotropy,
        c.heightFogStart, c.heightFogFalloff,
        c.froxelNear, c.froxelFar,
        c.fogAmbientColor.x, c.fogAmbientColor.y, c.fogAmbientColor.z,
        c.fogAmbientContribution,
        c.temporalEnabled ? 1u : 0u, c.temporalAlpha);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "SkyIBL atmosphereEnabled=%u skyboxSource=%u"
        " iblStrength=%.6f aerialComposite=%u\n",
        c.atmosphereEnabled ? 1u : 0u, c.skyboxSource,
        c.iblStrength,
        c.aerialCompositeEnabled ? 1u : 0u);
    ss << buf;

    std::ofstream f(path, std::ios::binary);
    if (!f) { LOG_ERROR("PostProcessConfig: cannot write '%s'", path.c_str()); return false; }
    const std::string text = ss.str();
    f.write(text.c_str(), static_cast<std::streamsize>(text.size()));
    LOG_SUCCESS("PostProcessConfig: saved '%s' (%zu bytes)", path.c_str(), text.size());
    return true;
}

bool Resource::LoadPostProcessConfig(const std::string& path, PostProcessConfig& out)
{
    std::vector<uint8_t> bytes;
    if (!::Resource::AssetFS::Get().ReadFile(path, bytes))
    { LOG_WARNING("PostProcessConfig: cannot open '%s'", path.c_str()); return false; }

    std::istringstream f(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        KV kv;
        if (!ParseLine(line, kv)) continue;
        const auto& m = kv.map;

        if (kv.tag == "CAS")
        {
            out.casEnabled   = GetB(m, "enabled",   out.casEnabled);
            out.casSharpness = GetF(m, "sharpness", out.casSharpness);
        }
        else if (kv.tag == "ToneMap")
        {
            out.bloomStrength        = GetF(m, "bloomStrength", out.bloomStrength);
            out.colorGradingEnabled  = GetB(m, "colorGradingEnabled", out.colorGradingEnabled);
            out.grading.exposure     = GetF(m, "exposure",   out.grading.exposure);
            out.grading.contrast     = GetF(m, "contrast",   out.grading.contrast);
            out.grading.brightness   = GetF(m, "brightness", out.grading.brightness);
            out.grading.lift.x       = GetF(m, "liftR",  out.grading.lift.x);
            out.grading.lift.y       = GetF(m, "liftG",  out.grading.lift.y);
            out.grading.lift.z       = GetF(m, "liftB",  out.grading.lift.z);
            out.grading.gamma.x      = GetF(m, "gammaR", out.grading.gamma.x);
            out.grading.gamma.y      = GetF(m, "gammaG", out.grading.gamma.y);
            out.grading.gamma.z      = GetF(m, "gammaB", out.grading.gamma.z);
            out.grading.gain.x       = GetF(m, "gainR",  out.grading.gain.x);
            out.grading.gain.y       = GetF(m, "gainG",  out.grading.gain.y);
            out.grading.gain.z       = GetF(m, "gainB",  out.grading.gain.z);
            out.grading.hueShift     = GetF(m, "hueShift",     out.grading.hueShift);
            out.grading.saturation   = GetF(m, "saturation",   out.grading.saturation);
            out.grading.vibrance     = GetF(m, "vibrance",     out.grading.vibrance);
            out.grading.temperature  = GetF(m, "temperature",  out.grading.temperature);
            out.grading.tint         = GetF(m, "tint",         out.grading.tint);
            out.grading.vignetteStrength = GetF(m, "vignette", out.grading.vignetteStrength);
            out.grading.filmGrain    = GetF(m, "grain",        out.grading.filmGrain);
        }
        else if (kv.tag == "AutoExp")
        {
            out.autoExposureEnabled = GetB(m, "enabled",    out.autoExposureEnabled);
            out.manualExposure      = GetF(m, "manual",     out.manualExposure);
            out.adaptationTau  = GetF(m, "adaptTau",    out.adaptationTau);
            out.minLogLuma     = GetF(m, "minLogLuma",  out.minLogLuma);
            out.maxLogLuma     = GetF(m, "maxLogLuma",  out.maxLogLuma);
            out.lowPercent     = GetF(m, "lowPct",      out.lowPercent);
            out.highPercent    = GetF(m, "highPct",     out.highPercent);
            out.minExposure    = GetF(m, "minExp",      out.minExposure);
            out.maxExposure    = GetF(m, "maxExp",      out.maxExposure);
            out.evBias         = GetF(m, "evBias",      out.evBias);
            out.keyValue       = GetF(m, "keyValue",    out.keyValue);
        }
        else if (kv.tag == "GTAO")
        {
            out.ssaoEnabled                 = GetB(m, "enabled",     out.ssaoEnabled);
            out.gtaoEffectRadius            = GetF(m, "radius",      out.gtaoEffectRadius);
            out.gtaoEffectFalloff           = GetF(m, "falloff",     out.gtaoEffectFalloff);
            out.gtaoFinalValuePower         = GetF(m, "finalPower",  out.gtaoFinalValuePower);
            out.gtaoSampleDistributionPower = GetF(m, "distPower",   out.gtaoSampleDistributionPower);
            out.gtaoThinOccluderCompensation= GetF(m, "thinComp",    out.gtaoThinOccluderCompensation);
            out.gtaoDenoiseSigma            = GetF(m, "denoise",     out.gtaoDenoiseSigma);
            out.gtaoSliceCount              = static_cast<uint32_t>(GetI(m, "slices", out.gtaoSliceCount));
            out.gtaoStepsPerSlice           = static_cast<uint32_t>(GetI(m, "steps",  out.gtaoStepsPerSlice));
        }
        else if (kv.tag == "Outline")
        {
            out.outlinePixels    = GetF(m, "pixels",    out.outlinePixels);
            out.outlineColor     = GetF3(m, "color",    out.outlineColor);
            out.depthThreshold   = GetF(m, "depthThr",  out.depthThreshold);
            out.normalThreshold  = GetF(m, "normalThr", out.normalThreshold);
            out.outlineStrength  = GetF(m, "strength",  out.outlineStrength);
            out.outlineFadeStart = GetF(m, "fadeStart", out.outlineFadeStart);
            out.outlineFadeEnd   = GetF(m, "fadeEnd",   out.outlineFadeEnd);
        }
        else if (kv.tag == "VolFog")
        {
            out.volFogEnabled          = GetB(m, "enabled",      out.volFogEnabled);
            out.fogDensity             = GetF(m, "density",      out.fogDensity);
            out.fogScattering          = GetF(m, "scattering",   out.fogScattering);
            out.fogAbsorption          = GetF(m, "absorption",   out.fogAbsorption);
            out.fogAnisotropy          = GetF(m, "anisotropy",   out.fogAnisotropy);
            out.heightFogStart         = GetF(m, "heightStart",  out.heightFogStart);
            out.heightFogFalloff       = GetF(m, "heightFalloff",out.heightFogFalloff);
            out.froxelNear             = GetF(m, "froxelNear",   out.froxelNear);
            out.froxelFar              = GetF(m, "froxelFar",    out.froxelFar);
            out.fogAmbientColor        = GetF3(m, "ambient",     out.fogAmbientColor);
            out.fogAmbientContribution = GetF(m, "ambientContrib", out.fogAmbientContribution);
            out.temporalEnabled        = GetB(m, "temporalEnabled", out.temporalEnabled);
            out.temporalAlpha          = GetF(m, "temporalAlpha",   out.temporalAlpha);
        }
        else if (kv.tag == "SkyIBL")
        {
            out.atmosphereEnabled      = GetB(m, "atmosphereEnabled", out.atmosphereEnabled);
            out.skyboxSource           = static_cast<uint32_t>(GetI(m, "skyboxSource", out.skyboxSource));
            out.iblStrength            = GetF(m, "iblStrength",      out.iblStrength);
            out.aerialCompositeEnabled = GetB(m, "aerialComposite",  out.aerialCompositeEnabled);
        }
    }
    return true;
}
