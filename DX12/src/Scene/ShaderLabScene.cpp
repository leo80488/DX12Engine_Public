#include "Scene/ShaderLabScene.h"

#include "ECS/Components.h"
#include "ECS/CameraSystem.h"
#include "ECS/SkyboxComponent.h"
#include "Graphics/Renderer.h"
#include "Scene/MeshSpawner.h"
#include "System/Log.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace
{
    // Three-point rig used by the default preset and the panel "reset" button.
    // Field order matches LightData declaration: radius, color, intensity,
    // direction, spotAngle, type, castsShadow.
    LightData MakeDirLight(DirectX::XMFLOAT3 dir,
                           DirectX::XMFLOAT3 color,
                           float intensity)
    {
        LightData ld;
        ld.color     = color;
        ld.intensity = intensity;
        ld.direction = dir;
        ld.type      = LightType::Directional;
        return ld;
    }

    const LightData kKey  = MakeDirLight({ 0.45f, -0.80f,  0.40f }, { 1.00f, 0.95f, 0.88f }, 1.0f);
    const LightData kFill = MakeDirLight({-0.60f, -0.40f, -0.30f }, { 0.78f, 0.86f, 1.00f }, 0.4f);
    const LightData kRim  = MakeDirLight({ 0.10f, -0.20f, -0.95f }, { 1.00f, 1.00f, 1.00f }, 0.6f);
}

void ShaderLabScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->world)
    {
        LOG_ERROR("ShaderLabScene: Init received null GameModeContext / World");
        return;
    }
    LOG_INFO("ShaderLabScene: Init — minimal shader test scene");

    World& world = *m_ctx->world;

    // ---- Camera --------------------------------------------------------------
    m_cameraEntity = world.CreateEntity();
    world.SetName(m_cameraEntity, "ShaderLab Camera");
    CameraControllerComponent camCtrl{};
    world.AddComponent<CameraComponent>(m_cameraEntity, CameraComponent{});
    world.AddComponent<CameraControllerComponent>(m_cameraEntity, camCtrl);
    world.AddComponent<LocalTransform>(m_cameraEntity,
        CameraSystem::MakeTransform(camCtrl, { 4.f, 3.f, 5.f }));
    world.AddComponent<GlobalTransform>(m_cameraEntity, GlobalTransform{});
    m_spawnedEntities.push_back(m_cameraEntity);

    // ---- 3-point light rig --------------------------------------------------
    m_keyLight = world.CreateEntity();
    world.SetName(m_keyLight, "Key Light");
    world.AddComponent<LightData>(m_keyLight, kKey);
    m_spawnedEntities.push_back(m_keyLight);

    m_fillLight = world.CreateEntity();
    world.SetName(m_fillLight, "Fill Light");
    world.AddComponent<LightData>(m_fillLight, kFill);
    m_spawnedEntities.push_back(m_fillLight);

    m_rimLight = world.CreateEntity();
    world.SetName(m_rimLight, "Rim Light");
    world.AddComponent<LightData>(m_rimLight, kRim);
    m_spawnedEntities.push_back(m_rimLight);

    // ---- Skybox + IBL -------------------------------------------------------
    m_skyboxEntity = world.CreateEntity();
    world.SetName(m_skyboxEntity, "Skybox");
    SkyboxComponent sc;
    sc.irradiancePath    = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
    sc.radiancePath      = "asset/IBL/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
    sc.skyboxPath        = "asset/IBL/autumn_field_puresky/autumn_field_puresky_skybox.itex";
    sc.radianceMipLevels = 7;
    sc.iblStrength       = 1.0f;
    world.AddComponent<SkyboxComponent>(m_skyboxEntity, sc);
    m_spawnedEntities.push_back(m_skyboxEntity);

    // ---- Test mesh (procedural sphere by default) ---------------------------
    m_meshEntity = MeshSpawner::Spawn(m_meshType, world);
    if (m_meshEntity != NullEntity)
        m_spawnedEntities.push_back(m_meshEntity);

    // Scan asset/IBL for available HDRI sets so the panel combo is populated.
    RescanHDRIList();

    // NPR ramp editor — owns its own GPU texture; init now so the panel
    // always has a valid SRV to display when the user toggles it open.
    m_rampEditor.Init(m_ctx->gfx);
}

void ShaderLabScene::Update(float dt)
{
    if (!m_ctx || !m_ctx->world) return;

    if (m_turntableEnabled)
        ApplyTurntable(*m_ctx->world, dt);
}

void ShaderLabScene::Shutdown()
{
    if (!m_ctx || !m_ctx->world)
    {
        LOG_INFO("ShaderLabScene: Shutdown (world already gone)");
        return;
    }
    m_rampEditor.Shutdown(m_ctx->gfx);
    World& world = *m_ctx->world;
    for (Entity e : m_spawnedEntities)
        if (world.IsAlive(e)) world.DestroyEntity(e);
    m_spawnedEntities.clear();
    m_meshEntity = m_cameraEntity = NullEntity;
    m_keyLight = m_fillLight = m_rimLight = m_skyboxEntity = NullEntity;
    LOG_INFO("ShaderLabScene: Shutdown");
}

// ---------------------------------------------------------------------------
// OnUIRender — single ImGui window with the MVP knobs. Lives here (not in
// EditorLayer) per design doc §6.3: tool-specific UI in tool source.
// ---------------------------------------------------------------------------
void ShaderLabScene::OnUIRender(GameModeContext& ctx)
{
    if (!ctx.world) return;
    World& world = *ctx.world;

    if (!ImGui::Begin("ShaderLab"))
    {
        ImGui::End();
        return;
    }

    // ---- Mesh ---------------------------------------------------------------
    {
        static const char* kMeshNames[] = { "Cube", "Sphere", "Cone", "Plane", "Torus" };
        if (ImGui::Combo("Test Mesh", &m_meshType, kMeshNames, IM_ARRAYSIZE(kMeshNames)))
            ApplyMeshSwap(world);
    }

    ImGui::Separator();

    // ---- Lighting preset ----------------------------------------------------
    {
        static const char* kPresetNames[] = { "Three-Point", "Key Only", "Pure Black" };
        if (ImGui::Combo("Lighting", &m_lightPreset, kPresetNames, IM_ARRAYSIZE(kPresetNames)))
            ApplyLightPreset(world);
    }

    ImGui::Separator();

    // ---- HDRI ---------------------------------------------------------------
    if (m_hdriNames.empty())
    {
        ImGui::TextDisabled("No HDRI sets found in asset/IBL/");
    }
    else
    {
        // ImGui::Combo wants a flat array of c-strings; build it on the fly.
        std::vector<const char*> items;
        items.reserve(m_hdriNames.size());
        for (const auto& n : m_hdriNames) items.push_back(n.c_str());
        if (ImGui::Combo("HDRI", &m_hdriIndex,
                         items.data(), static_cast<int>(items.size())))
            ApplyHDRISwap(world);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) RescanHDRIList();

    ImGui::Separator();

    // ---- Camera turntable ---------------------------------------------------
    ImGui::Checkbox("Turntable", &m_turntableEnabled);
    ImGui::SliderFloat("Speed (rad/s)",  &m_turntableSpeed,   -2.0f, 2.0f);
    ImGui::SliderFloat("Orbit Pitch",    &m_orbitPitch,       -1.5f, 1.5f);
    ImGui::SliderFloat("Orbit Distance", &m_orbitDistance,     0.5f, 20.0f);
    if (ImGui::Button("Reset Yaw")) m_orbitYaw = 0.0f;

    ImGui::Separator();

    // ---- Frame capture ------------------------------------------------------
    // Saves the post-tonemap viewport as PNG so the user can flip back and
    // forth between before/after a shader edit (design doc §5.2).
    if (ImGui::Button("Capture Frame"))
    {
        const std::string path = MakeCapturePath();
        if (ctx.renderer.CaptureViewportToPNG(path.c_str()))
        {
            m_captureHistory.insert(m_captureHistory.begin(), path);
            if (m_captureHistory.size() > kMaxCaptureHistory)
                m_captureHistory.resize(kMaxCaptureHistory);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("→ captures/*.png");

    if (!m_captureHistory.empty())
    {
        ImGui::TextDisabled("Recent (click to copy path):");
        for (const auto& p : m_captureHistory)
        {
            ImGui::PushID(p.c_str());
            if (ImGui::Selectable(p.c_str()))
                ImGui::SetClipboardText(p.c_str());
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // ---- NPR ramp editor toggle (design doc §5.3) ---------------------------
    ImGui::Checkbox("NPR Ramp Editor", &m_rampEditorVisible);

    // Live-binding: shove the editor's bindless index into the sphere's
    // RAMPMAP slot. Pair the toggle with switching the material's Shader
    // Type to "NPR Ramp" in the inspector so the lighting pass routes
    // through the ramp branch.
    ImGui::Checkbox("Bind ramp to sphere RAMPMAP slot", &m_rampOverrideEnabled);

    if (m_rampOverrideEnabled
        && m_meshEntity != NullEntity
        && world.IsAlive(m_meshEntity))
    {
        if (auto* mat = world.GetComponent<MaterialComponent>(m_meshEntity))
        {
            auto& slot = mat->textures[MaterialComponent::RAMPMAP];

            // Capture the original bindless index once so we can restore it
            // when the user toggles the override off.
            if (!m_rampOverrideOriginalCaptured)
            {
                m_rampOverrideOriginalBindless = slot.bindlessIndex;
                m_rampOverrideOriginalCaptured = true;
            }

            const int32_t editorIdx = m_rampEditor.GetBindlessIndex();
            if (editorIdx >= 0 && slot.bindlessIndex != editorIdx)
            {
                slot.bindlessIndex = editorIdx;
                mat->SetDirty();   // BuildRenderScene re-uploads MaterialGPUData this frame
            }
        }
    }
    else if (m_rampOverrideOriginalCaptured
             && m_meshEntity != NullEntity
             && world.IsAlive(m_meshEntity))
    {
        // Override just got turned off — restore the slot to what the
        // texture system originally bound.
        if (auto* mat = world.GetComponent<MaterialComponent>(m_meshEntity))
        {
            mat->textures[MaterialComponent::RAMPMAP].bindlessIndex =
                m_rampOverrideOriginalBindless;
            mat->SetDirty();
        }
        m_rampOverrideOriginalCaptured = false;
        m_rampOverrideOriginalBindless = -1;
    }

    ImGui::End();

    // Render the ramp editor as a separate floating window when toggled on.
    m_rampEditor.OnUIRender(ctx.gfx, m_rampEditorVisible);
}

// ---------------------------------------------------------------------------
void ShaderLabScene::RescanHDRIList()
{
    m_hdriNames.clear();
    std::error_code ec;
    const std::filesystem::path root("asset/IBL");
    if (!std::filesystem::is_directory(root, ec)) return;

    for (auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (!entry.is_directory(ec)) continue;
        // Only surface dirs that look like a complete IBL set — they need the
        // three .itex files SkyboxComponent expects.
        const auto dir = entry.path();
        const auto stem = dir.filename().string();
        const std::filesystem::path irr = dir / (stem + "_Irradiance.itex");
        if (!std::filesystem::exists(irr, ec)) continue;
        m_hdriNames.push_back(stem);
    }
    if (m_hdriIndex >= static_cast<int>(m_hdriNames.size())) m_hdriIndex = 0;
}

void ShaderLabScene::ApplyMeshSwap(World& world)
{
    if (m_meshEntity != NullEntity && world.IsAlive(m_meshEntity))
    {
        // Drop the existing entity wholesale — its MeshHandle/Material/etc.
        // need to be recreated from the new primitive's defaults.
        world.DestroyEntity(m_meshEntity);
        // Remove from the spawned-entity list so Shutdown doesn't double-destroy.
        for (auto it = m_spawnedEntities.begin(); it != m_spawnedEntities.end(); ++it)
            if (*it == m_meshEntity) { m_spawnedEntities.erase(it); break; }
    }

    m_meshEntity = MeshSpawner::Spawn(m_meshType, world);
    if (m_meshEntity != NullEntity)
        m_spawnedEntities.push_back(m_meshEntity);
}

void ShaderLabScene::ApplyLightPreset(World& world)
{
    auto setLight = [&](Entity e, const LightData& src, bool active)
    {
        if (e == NullEntity || !world.IsAlive(e)) return;
        if (auto* ld = world.GetComponent<LightData>(e))
        {
            *ld = src;
            if (!active) ld->intensity = 0.0f;   // keep the entity, just dark
        }
    };

    switch (m_lightPreset)
    {
    case 0: // Three-point
        setLight(m_keyLight,  kKey,  true);
        setLight(m_fillLight, kFill, true);
        setLight(m_rimLight,  kRim,  true);
        break;
    case 1: // Key only
        setLight(m_keyLight,  kKey,  true);
        setLight(m_fillLight, kFill, false);
        setLight(m_rimLight,  kRim,  false);
        break;
    case 2: // Pure black (zero everything; IBL too)
        setLight(m_keyLight,  kKey,  false);
        setLight(m_fillLight, kFill, false);
        setLight(m_rimLight,  kRim,  false);
        if (m_skyboxEntity != NullEntity && world.IsAlive(m_skyboxEntity))
            if (auto* sb = world.GetComponent<SkyboxComponent>(m_skyboxEntity))
                sb->iblStrength = 0.0f;
        return;  // don't restore IBL below
    }

    // For preset 0/1, make sure IBL is on (might have been zeroed by preset 2).
    if (m_skyboxEntity != NullEntity && world.IsAlive(m_skyboxEntity))
        if (auto* sb = world.GetComponent<SkyboxComponent>(m_skyboxEntity))
            sb->iblStrength = 1.0f;
}

void ShaderLabScene::ApplyHDRISwap(World& world)
{
    if (m_hdriIndex < 0 || m_hdriIndex >= static_cast<int>(m_hdriNames.size())) return;
    if (m_skyboxEntity == NullEntity || !world.IsAlive(m_skyboxEntity))         return;

    auto* sb = world.GetComponent<SkyboxComponent>(m_skyboxEntity);
    if (!sb) return;

    const std::string& name = m_hdriNames[m_hdriIndex];
    const std::string  base = "asset/IBL/" + name + "/" + name;
    sb->irradiancePath = base + "_Irradiance.itex";
    sb->radiancePath   = base + "_Radiance.itex";
    sb->skyboxPath     = base + "_skybox.itex";
    LOG_INFO("ShaderLabScene: HDRI swapped to '%s'", name.c_str());
}

std::string ShaderLabScene::MakeCapturePath() const
{
    using clock = std::chrono::system_clock;
    const auto t  = clock::to_time_t(clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "captures/capture_%04d%02d%02d_%02d%02d%02d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void ShaderLabScene::ApplyTurntable(World& world, float dt)
{
    if (m_cameraEntity == NullEntity || !world.IsAlive(m_cameraEntity)) return;
    auto* lt = world.GetComponent<LocalTransform>(m_cameraEntity);
    if (!lt) return;

    m_orbitYaw += m_turntableSpeed * dt;

    // Spherical → Cartesian. Camera orbits the origin at orbitDistance.
    const float cosP = std::cosf(m_orbitPitch);
    const float sinP = std::sinf(m_orbitPitch);
    const float cosY = std::cosf(m_orbitYaw);
    const float sinY = std::sinf(m_orbitYaw);

    lt->translation = { m_orbitDistance * sinY * cosP,
                        m_orbitDistance * sinP,
                        m_orbitDistance * cosY * cosP };

    // Camera looks back at the origin. With the FPS forward convention
    // forward = (sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)),
    // looking from the orbit position toward the origin is exactly
    // yaw = orbitYaw + π, pitch = orbitPitch — no trig inversion needed.
    DirectX::XMStoreFloat4(&lt->rotation,
        DirectX::XMQuaternionRotationRollPitchYaw(
            m_orbitPitch, m_orbitYaw + DirectX::XM_PI, 0.f));
}
