#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/IVideoDecoder.h"

// render passes
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/HeightFogPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/VideoPass.h"
#include "RenderGraph/RenderPass/VideoQuadPass.h"
#include "RenderGraph/RenderPass/WaterPass.h"

// ECS components + systems
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/TODSystems.h"
#include "ECS/VideoComponent.h"

// STL / DirectXMath
#include <algorithm>
#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
using TerrainParamsCB = RendererDetail::TerrainParamsCB;
static_assert(sizeof(TerrainParamsCB) == 176,
    "TerrainParamsCB layout drift — sync Terrain.{ms,as,ps,shadow.ms,shadow.as}.hlsl + Renderer.h");

// Renderer_IBL.cpp — split out of Renderer.cpp (one TU per Renderer subsystem).
// All members belong to class Renderer (declared in Graphics/Renderer.h).
// The include block mirrors Renderer.cpp so every cluster keeps compiling;
// trim per-TU later if desired.
// Sky / IBL: SkyboxComponent + Atmosphere + TOD + clouds → LightCB + passes.
// ---------------------------------------------------------------------------
void Renderer::SyncSkyboxIBL(World& world)
{
    uint64_t irradianceHandle = 0;
    uint64_t radianceHandle   = 0;
    uint64_t skyboxHandle     = 0;
    uint32_t radianceMips     = 5;
    float    iblStrength      = 1.0f;

    // ---- Locate / auto-spawn singleton entities ----------------------------
    // Sky entity        = AtmosphereComponent (+ SkyboxComponent on the same
    //                     entity by convention; not required).
    // Time-of-Day ent.  = TODConfigComponent + TODOutputComponent (separate
    //                     entity so TOD is independently selectable in the
    //                     Hierarchy and doesn't clutter Sky's inspector).
    //
    // Sun / Moon light entities are NOT auto-spawned. TOD is a CONTROLLER:
    // it drives any directional LightData carrying SunLightTag / MoonLightTag.
    // Without a tagged light, TOD systems are no-ops on the light side — the
    // user's authored directional lights work as normal whether TOD is on or
    // off. Use the "Create → Sun Light" / "Moon Light" menu (or add the tag
    // via the inspector) to bring an existing light under TOD control.
    AtmosphereComponent* atmosComp = nullptr;
    {
        Entity skyEntity = NullEntity;
        auto* atmoPool = world.GetPool<AtmosphereComponent>();
        if (atmoPool && !atmoPool->Data().empty())
        {
            atmosComp = &atmoPool->Data()[0];
            skyEntity = atmoPool->Entities()[0];
        }

        if (!atmosComp)
        {
            // Upgrade an existing entity that already has a SkyboxComponent.
            world.ForEach<SkyboxComponent>([&](Entity e, SkyboxComponent&) {
                if (skyEntity == NullEntity) skyEntity = e;
            });
            if (skyEntity == NullEntity)
            {
                skyEntity = world.CreateEntity();
                world.SetName(skyEntity, "Sky");
            }
            world.AddComponent<AtmosphereComponent>(skyEntity, AtmosphereComponent{});
            atmosComp = world.GetComponent<AtmosphereComponent>(skyEntity);
        }

        // Auto-attach CloudComponent to the Sky entity. Default enabled=true
        // so a fresh world shows clouds out of the box (matches AtmosphereComponent
        // pattern). User can flip off via inspector.
        if (skyEntity != NullEntity && !world.HasComponent<CloudComponent>(skyEntity))
        {
            CloudComponent c{};
            c.enabled = true;
            world.AddComponent<CloudComponent>(skyEntity, c);
        }

        // Auto-attach HeightFogComponent DISABLED — discoverable in the
        // inspector without retroactively fogging existing scenes.
        if (skyEntity != NullEntity && !world.HasComponent<HeightFogComponent>(skyEntity))
            world.AddComponent<HeightFogComponent>(skyEntity, HeightFogComponent{});

        // Separate Time-of-Day entity (singleton).
        auto* todCfgPool = world.GetPool<TODConfigComponent>();
        if (!todCfgPool || todCfgPool->Entities().empty())
        {
            Entity tod = world.CreateEntity();
            world.SetName(tod, "Time of Day");
            world.AddComponent<TODConfigComponent>(tod, TODConfigComponent{});
            world.AddComponent<TODOutputComponent>(tod, TODOutputComponent{});
        }
        else
        {
            // Repair: ensure Output is paired with Config on the same entity.
            Entity todE = todCfgPool->Entities()[0];
            if (!world.HasComponent<TODOutputComponent>(todE))
                world.AddComponent<TODOutputComponent>(todE, TODOutputComponent{});
        }
    }

    // ---- Run TOD pipeline before the light gather --------------------------
    // Order matches the design: tick → evaluate → sync (sun, moon, atmosphere).
    // The sync systems write into LightData of the tagged sun/moon entities,
    // which the later light-gather loop in BuildScene_UploadLights reads as
    // normal directional lights.
    TODTickSystem::Update          (world, m_deltaTime);
    TODEvaluationSystem::Update    (world);
    TODSunSyncSystem::Update       (world);
    TODMoonSyncSystem::Update      (world);
    TODAtmosphereSyncSystem::Update(world);

    // Push AtmosphereComponent → SkyIBLPass (atmosphere/IBL/aerial only; TOD
    // values now flow via SunLightTag entity + TODOutputComponent below).
    if (atmosComp && m_skyIBLPass)
    {
        m_skyIBLPass->SetAtmosphereEnabled(atmosComp->atmosphereEnabled);
        m_skyIBLPass->SetSkyboxSource(
            static_cast<SkyIBLPass::SkyboxSource>(atmosComp->skyboxSource));
        m_skyIBLPass->SetIBLStrength      (atmosComp->iblStrength);
        m_skyIBLPass->SetAerialCompositeEnabled(atmosComp->aerialCompositeEnabled);
    }

    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        SkyboxComponent* sc = world.GetComponent<SkyboxComponent>(e);
        if (!sc) continue;

        if (m_texSys && m_resMgr)
        {
            auto syncTex = [&](SkyboxTexEntry& entry, const std::string& path, uint64_t& outHandle)
            {
                if (path.empty()) return;
                if (entry.path != path)
                {
                    if (entry.handle != Resource::kInvalidTextureHandle)
                        m_texSys->Release(entry.handle, m_gfx);
                    entry.path   = path;
                    entry.handle = m_texSys->Acquire(path, *m_resMgr, m_gfx);
                    outHandle    = 0;
                }
                if (entry.handle != Resource::kInvalidTextureHandle && m_texSys->IsReady(entry.handle))
                    if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                        outHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
            };

            const uint64_t prevIrr = m_skyboxTexCache[0].lastLoggedHandle;
            const uint64_t prevRad = m_skyboxTexCache[1].lastLoggedHandle;
            syncTex(m_skyboxTexCache[0], sc->irradiancePath, irradianceHandle);
            syncTex(m_skyboxTexCache[1], sc->radiancePath,   radianceHandle);
            syncTex(m_skyboxTexCache[2], sc->skyboxPath,     skyboxHandle);

            if (irradianceHandle && irradianceHandle != prevIrr)
            {
                m_skyboxTexCache[0].lastLoggedHandle = irradianceHandle;
                LOG_INFO("SyncSkyboxIBL: irradiance handle ready (gpu=0x%llX)", irradianceHandle);
            }
            if (radianceHandle && radianceHandle != prevRad)
            {
                m_skyboxTexCache[1].lastLoggedHandle = radianceHandle;
                LOG_INFO("SyncSkyboxIBL: radiance handle ready (gpu=0x%llX)", radianceHandle);
            }

            sc->irradianceGpuHandle = irradianceHandle;
            sc->radianceGpuHandle   = radianceHandle;
            sc->skyboxGpuHandle     = skyboxHandle;
        }

        radianceMips = sc->radianceMipLevels;
        // SkyboxComponent.iblStrength ignored — global setting on SkyIBLPass (Post Processing panel).
        break; // only one skybox per scene
    }

    // Global IBL strength lives on SkyIBLPass.
    if (m_skyIBLPass)
        iblStrength = m_skyIBLPass->GetIBLStrength();

    // BRDF LUT — fixed asset, loaded once.
    static constexpr const char* kBRDFLUTPath = "asset/IBL/BRDF_LUT.itex";
    uint64_t brdfLutHandle = 0;
    if (m_texSys && m_resMgr)
    {
        if (m_brdfLutEntry.path.empty())
        {
            m_brdfLutEntry.path   = kBRDFLUTPath;
            m_brdfLutEntry.handle = m_texSys->Acquire(kBRDFLUTPath, *m_resMgr, m_gfx);
        }
        if (m_brdfLutEntry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(m_brdfLutEntry.handle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(m_brdfLutEntry.handle))
                brdfLutHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
        }
    }

    // SkyIBL: atmosphere drives SH/specular/backdrop by default. Disabled fallback priority: skybox > radiance > irradiance.
    uint64_t shSourceHandle = skyboxHandle ? skyboxHandle
                            : radianceHandle ? radianceHandle
                            : irradianceHandle;
    if (m_skyIBLPass)
    {
        m_skyIBLPass->SetSourceCubemap(shSourceHandle);

        // NOTE: the AP camera push (SetCameraForAerial) moved to BeginFrame
        // AFTER UploadFrameData — reading the LightCB ring slot here picked
        // up the value from kFrameCount frames ago (and zeros on the first
        // frames), because UploadFrameData writes this frame's slot later.

        // TOD: read computed sun/moon state from TODOutputComponent (written
        // by TODEvaluationSystem earlier this frame). When TOD disabled, the
        // light gather will already have pushed the directional light into
        // LightCB; we just feed SkyIBLPass the geometric sun from output for
        // atmosphere shaders.
        const auto* todCfg = TODUtil::FindConfig(world);
        const auto* todOut = TODUtil::FindOutput(world);
        const bool  todActive = todCfg && todOut && todCfg->enabled;

        if (todActive)
        {
            // Active body (sun-by-day / moon-by-night) drives LightCB so
            // CSM + lighting + fog stay in sync with the sky. The sun/moon
            // LightData entities also got their colors set by the sync
            // systems, but the gather loop ordering is non-deterministic
            // between two directional lights; this explicit write makes the
            // active body authoritative regardless of iteration order.
            if (m_lightCB.Current(m_gfx))
            {
                auto* lb = m_lightCB.Current(m_gfx);
                lb->lightDir[0] = -todOut->activeDirection.x;
                lb->lightDir[1] = -todOut->activeDirection.y;
                lb->lightDir[2] = -todOut->activeDirection.z;
                lb->lightColor[0] = todOut->activeColor.x;
                lb->lightColor[1] = todOut->activeColor.y;
                lb->lightColor[2] = todOut->activeColor.z;
            }
            // Push to SkyIBLPass: active body for lighting consumers,
            // geometric sun for atmosphere scattering.
            m_skyIBLPass->SetSunDir(todOut->activeDirection, todOut->activeColor);
            m_skyIBLPass->SetAtmosphereSun(todOut->sunDirection, todOut->sunColor);
        }
        else if (m_lightCB.Current(m_gfx))
        {
            // TOD off → existing directional light (gathered into LightCB)
            // drives sun. Read back and push to SkyIBLPass for atmosphere
            // shaders — directional intensity = 0 propagates as zero sun
            // (Unreal-style: no sun = no atmosphere scatter, scene darkens).
            auto* lb = m_lightCB.Current(m_gfx);
            DirectX::XMFLOAT3 sunDir{ -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
            DirectX::XMFLOAT3 sunCol{ lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
            m_skyIBLPass->SetSunDir(sunDir, sunCol);
            m_skyIBLPass->SetAtmosphereSun(sunDir, sunCol);
        }
    }

    // ---- Volumetric clouds: push CloudComponent + TOD sun state ------------
    if (m_cloudPass)
    {
        // Find first CloudComponent in the world (typically attached to the
        // Sky entity; auto-spawn is intentional NOT done — clouds are opt-in).
        CloudComponent* cloudComp = nullptr;
        if (auto* p = world.GetPool<CloudComponent>(); p && !p->Data().empty())
            cloudComp = &p->Data()[0];

        if (cloudComp)
        {
            // Resolve sun direction (towards-sun) + colour. Prefer TODOutput
            // when TOD is enabled; otherwise read back from LightCB so the
            // user's authored directional light still drives cloud lighting.
            DirectX::XMFLOAT3 sunDirToSun { 0.f, 1.f, 0.f };
            DirectX::XMFLOAT3 sunRGB      { 1.f, 1.f, 1.f };
            const auto* todCfg = TODUtil::FindConfig(world);
            const auto* todOut = TODUtil::FindOutput(world);
            if (todCfg && todOut && todCfg->enabled)
            {
                sunDirToSun = todOut->activeDirection;   // already towards-body
                sunRGB      = todOut->activeColor;
            }
            else if (m_lightCB.Current(m_gfx))
            {
                auto* lb = m_lightCB.Current(m_gfx);
                sunDirToSun = { -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
                sunRGB      = {  lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
            }

            m_cloudPass->SetEnabled(cloudComp->enabled);
            m_cloudPass->SetParams (*cloudComp, m_deltaTime);
            m_cloudPass->SetSun    (sunDirToSun, sunRGB);

            if (m_lightCB.Current(m_gfx))
            {
                auto* lb = m_lightCB.Current(m_gfx);
                DirectX::XMFLOAT4X4 invVP;
                std::memcpy(&invVP, lb->invViewProj, sizeof(invVP));
                DirectX::XMFLOAT3 camPos{ lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
                m_cloudPass->SetCamera(invVP, camPos, m_camera.nearZ, m_camera.farZ);
            }
        }
        else
        {
            m_cloudPass->SetEnabled(false);
        }
    }

    // ---- Exponential height fog: push HeightFogComponent → HeightFogPass ---
    // Sun dir/colour need no plumbing — the apply shader reads LightCB, which
    // is already TOD-authoritative by this point.
    if (m_heightFogPass)
    {
        HeightFogComponent* hf = nullptr;
        if (auto* p = world.GetPool<HeightFogComponent>(); p && !p->Data().empty())
            hf = &p->Data()[0];   // singleton-by-convention, first wins

        if (hf) m_heightFogPass->SetParams(*hf);
        m_heightFogPass->SetEnabled(hf && hf->enabled);
    }

    // ---- Video — split between screen-space VideoPass and world-space ----
    // VideoQuadPass. VideoPass takes the first SCREEN-SPACE component (the
    // overlay / cinematic path); VideoQuadPass handles WORLD-SPACE quads.
    // One AddDecodeDependency per active decoder so the graphics queue
    // waits on the video queue before sampling the NV12 output.
    if (m_videoPass || m_videoQuadPass)
    {
        const VideoComponent* screenActive = nullptr;
        world.ForEach<VideoComponent>(
            [&](Entity /*e*/, const VideoComponent& vc)
        {
            if (vc.worldSpace) return;
            if (screenActive)  return;
            if (vc.state == VideoPlaybackState::Stopped) return;
            if (vc.currentDpbSlot >= vc.dpb.size())      return;
            screenActive = &vc;
        });

        if (m_videoPass)
        {
            m_videoPass->SetActiveFrame(screenActive);
            if (screenActive)
            {
                m_videoPass->SetRect(screenActive->rectUMin, screenActive->rectVMin,
                                     screenActive->rectUMax, screenActive->rectVMax);
                m_videoPass->SetAlpha(screenActive->renderAlpha);
                m_videoPass->SetColorSpace(screenActive->colorSpace);
            }
        }
        if (m_videoQuadPass)
        {
            // World-space pass iterates inline (it needs the GlobalTransform
            // per entity), so we just hand it the World pointer + camera.
            m_videoQuadPass->SetWorld(&world);
            m_videoQuadPass->SetCamera(m_view.viewProjMatrixNoJitter);
        }

        // Insert cross-queue decode wait per ACTIVE decoder (screen + world).
        // Backend issues gfxQueue->Wait(videoFence, lastDecodeValue); the
        // next ExecuteCommandLists on the graphics queue blocks GPU-side
        // until those decoded NV12 frames are retired.
        if (auto* vb = m_gfx.GetVideoBackend())
        {
            if (screenActive)
                vb->AddDecodeDependency(screenActive->decoder,
                                        screenActive->framesDecoded,
                                        RHI::CommandList{});
            world.ForEach<VideoComponent>(
                [&](Entity /*e*/, const VideoComponent& vc)
            {
                if (!vc.worldSpace) return;
                if (vc.state == VideoPlaybackState::Stopped) return;
                if (!vc.decoder.IsValid()) return;
                vb->AddDecodeDependency(vc.decoder, vc.framesDecoded,
                                        RHI::CommandList{});
            });
        }
    }

    const bool atmosphereOn = m_skyIBLPass && m_skyIBLPass->IsAtmosphereEnabled();
    // Static mode skips every compute pass — gate on atmosphereOn so stale LUTs don't leak.
    const bool skyUseSH     = atmosphereOn
                           && m_skyIBLPass && m_skyIBLPass->IsSHValid();

    // iblStrength is specular-only (see Lighting.ps.hlsl semantics). 0 is a
    // valid user choice — "no specular IBL, keep diffuse SH / DDGI". Don't
    // override it just because atmosphere is on.

    // Effective mip count may come from SkyIBLPass's pre-filtered cube vs static radiance.
    uint32_t cbRadianceMips = radianceMips;
    if (m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
        cbRadianceMips = m_skyIBLPass->GetSpecularMipCount();

    if (m_lightCB.Current(m_gfx))
    {
        auto* lb = m_lightCB.Current(m_gfx);
        lb->iblRadianceMips = cbRadianceMips;
        lb->iblStrength     = iblStrength;
        lb->iblUseSH        = skyUseSH ? 1u : 0u;
        // AP composite gated on atmosphere+valid+opt-in; first frame's ap.a=0 would multiply scene→black.
        const bool apReady = m_skyIBLPass
                          && m_skyIBLPass->IsAtmosphereEnabled()
                          && m_skyIBLPass->IsAerialValid()
                          && m_skyIBLPass->IsAerialCompositeEnabled();
        lb->aerialMaxDistKm = apReady ? m_skyIBLPass->GetAerialMaxDistanceKm() : 0.0f;
    }

    // Atmosphere → prefer live SkyIBLPass specular; static → loaded .itex (SkyIBLPass cube would be stale).
    uint64_t effectiveRadiance = radianceHandle;
    uint32_t effectiveRadianceMips = radianceMips;
    if (atmosphereOn && m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
    {
        const uint64_t spec = m_skyIBLPass->GetSpecularSrvHandle();
        if (spec)
        {
            effectiveRadiance     = spec;
            effectiveRadianceMips = m_skyIBLPass->GetSpecularMipCount();
        }
    }

    // SSR composite cache (same handle as LightingPass split-sum BRDF LUT).
    m_brdfLutSrv             = brdfLutHandle;
    // DDGI miss samples RADIANCE cube (sun disk preserved); irradiance would smear sun → L1 SH ≈ 0.
    m_skyRadianceSrvForDDGI  = effectiveRadiance;

    // Forward handles to passes
    if (m_lightingPass)
    {
        m_lightingPass->SetIBL(irradianceHandle, effectiveRadiance, effectiveRadianceMips, iblStrength);
        m_lightingPass->SetBRDFLUT(brdfLutHandle);
        m_lightingPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0,
                                 skyUseSH);
        // AP: 3D LUT + max distance. Handle 0 → 1×1×1 fallback binds (inscatter=0, T=1, no-op).
        if (m_skyIBLPass)
            m_lightingPass->SetAerialPerspective(
                m_skyIBLPass->GetAerialPerspectiveSrvHandle(),
                m_skyIBLPass->GetAerialMaxDistanceKm());
    }
    if (m_transparentPass)
    {
        // Mirror LightingPass: procedural specular cube when atmosphere on (forward agrees w/ deferred).
        // Diffuse stays on irradiance cube; iblUseSH=1 routes forward through EvalSH2.
        m_transparentPass->SetIBL(irradianceHandle, effectiveRadiance,
                                  effectiveRadianceMips, iblStrength);
        m_transparentPass->SetBRDFLUT(brdfLutHandle);
        m_transparentPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    if (m_skyboxPass)
    {
        const uint64_t staticFallback = skyboxHandle ? skyboxHandle
                                      : radianceHandle ? radianceHandle
                                      : irradianceHandle;
        const uint64_t resolved = m_skyIBLPass
            ? m_skyIBLPass->ResolveSkyboxSrvHandle(staticFallback)
            : staticFallback;
        m_skyboxPass->SetEnvMap(resolved);

        // Water reflects exactly the sky cube the SkyboxPass draws.
        if (m_waterPass)
            m_waterPass->SetSkyCube(resolved);

        // Analytic sun disk in PS for pixel-sharp result; uses whichever sun drives the atmosphere.
        if (m_skyIBLPass)
        {
            const DirectX::XMFLOAT3& sd = m_skyIBLPass->GetSunDir();
            const DirectX::XMFLOAT3& sc = m_skyIBLPass->GetSunColor();
            // Real solar half-angle ≈ 0.27° (0.00465 rad); slightly tighter for crisp point.
            m_skyboxPass->SetSun(sd, sc, /*half-angle rad*/ 0.005f,
                                 /*intensity*/ 15.0f);

            if (m_moonHandle == Resource::kInvalidTextureHandle && m_texSys && m_resMgr)
                m_moonHandle = m_texSys->Acquire(
                    "asset/Default_Texture/moon.itex", *m_resMgr, m_gfx);
            if (m_moonSRV == 0 && m_texSys
                && m_moonHandle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(m_moonHandle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(m_moonHandle))
                    m_moonSRV = m_gfx.GetTextureSRVGpuHandle(*tex);
            }

            // Moon disk + stars now read from TODOutputComponent (singleton).
            // When TOD is off, moon is hidden and night alpha is zero — author
            // can drape a static night sky via SkyboxComponent if needed.
            const auto* todOut = TODUtil::FindOutput(world);
            const bool  moonVisible = todOut && todOut->moonDiskVisible;
            const DirectX::XMFLOAT3 moonDir   = todOut ? todOut->moonDirection
                                                       : DirectX::XMFLOAT3{ 0, -1, 0 };
            const DirectX::XMFLOAT3 moonColor = todOut ? todOut->moonColor
                                                       : DirectX::XMFLOAT3{ 0, 0, 0 };
            m_skyboxPass->SetMoon(m_moonSRV, moonVisible, moonDir, moonColor,
                                  /*half-angle rad*/ 0.026f);

            // Stars drive off MOON altitude (above horizon = night).
            const float moonY = moonDir.y;
            auto smoothstepF = [](float e0, float e1, float x) {
                float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            float nightAlpha = smoothstepF(-0.20f, 0.10f, moonY);

            // Star twinkle: wraps every ~10 min for sin-phase float precision.
            static float s_starTime = 0.0f;
            s_starTime = std::fmod(s_starTime + m_deltaTime, 600.0f);

            const float starDensity    = atmosComp ? atmosComp->starDensity    : 256.0f;
            const float starBrightness = atmosComp ? atmosComp->starBrightness : 0.7f;
            m_skyboxPass->SetStars(nightAlpha, s_starTime, starDensity, starBrightness);
        }
    }
}



