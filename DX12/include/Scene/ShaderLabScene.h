#pragma once

// ShaderLabScene — minimal IScene for the ShaderLab.exe build target.
//
// Per the original ShaderLab design doc (section 6.1), the tool runs against
// the same ECS / Renderer / RenderGraph the game uses — just with a tiny
// 5-10 entity scene rather than a full level. That guarantees "what you see
// in the tool == what you see in the game" without forking the renderer.
//
// Spawned content:
//   - 1 procedural sphere (default material, drag custom shader onto it
//     to exercise the reflection-driven inspector)
//   - 1 key directional light + 2 fill lights for a 3-point setup
//   - 1 skybox + IBL environment
//   - 1 camera positioned to frame the sphere
//
// Update is empty — ECS systems in App handle everything. Future work
// (turntable spin, HDRI swap UI, ramp editor) hangs off this skeleton.

#include "Scene/IScene.h"
#include "Scene/NPRRampEditor.h"

#include <vector>

class ShaderLabScene : public IScene
{
public:
    const char* GetName() const override { return "ShaderLabScene"; }

    void Init      (SceneContext* ctx)  override;
    void Update    (float dt)           override;
    void OnUIRender(SceneContext& ctx)  override;
    void Shutdown  ()                   override;

private:
    // Track entities we spawned so Shutdown can clean up without touching
    // anything user code has added since.
    std::vector<Entity> m_spawnedEntities;

    // Tracked separately so the panel can swap them without losing the
    // identity of "the test mesh" / "the camera" / each light.
    Entity m_meshEntity   = NullEntity;
    Entity m_cameraEntity = NullEntity;
    Entity m_keyLight     = NullEntity;
    Entity m_fillLight    = NullEntity;
    Entity m_rimLight     = NullEntity;
    Entity m_skyboxEntity = NullEntity;

    // ---- Panel state ----
    int   m_meshType        = 1;       // 0=Cube, 1=Sphere, 2=Cone (matches MeshSpawner)
    int   m_lightPreset     = 0;       // 0=Three-point, 1=Key only, 2=Pure black
    int   m_hdriIndex       = 0;       // index into m_hdriPaths
    bool  m_turntableEnabled = true;
    float m_turntableSpeed   = 0.4f;   // radians/sec
    float m_orbitYaw         = 0.0f;
    float m_orbitPitch       = 0.25f;  // ~14° elevation
    float m_orbitDistance    = 3.0f;

    // Discovered IBL set names (from asset/IBL/<name>/...). Filled in Init.
    std::vector<std::string> m_hdriNames;

    // Recent capture filenames (relative paths like "captures/...png").
    // Bounded to kMaxCaptureHistory to keep the panel tidy.
    static constexpr std::size_t kMaxCaptureHistory = 8;
    std::vector<std::string> m_captureHistory;

    // NPR ramp editor (design doc §5.3). Lifetime tied to scene Init/Shutdown.
    ShaderLab::NPRRampEditor m_rampEditor;
    bool                     m_rampEditorVisible = false;
    // Live-binding state — when true, ShaderLabScene writes the editor's
    // bindless index into the sphere material's RAMPMAP slot every UI tick.
    // Original value is captured on enable + restored on disable so the
    // material returns to whatever the texture system originally bound.
    bool    m_rampOverrideEnabled         = false;
    bool    m_rampOverrideOriginalCaptured = false;
    int32_t m_rampOverrideOriginalBindless = -1;

    // Helpers
    void RescanHDRIList();
    void ApplyMeshSwap(World& world);
    void ApplyLightPreset(World& world);
    void ApplyHDRISwap(World& world);
    void ApplyTurntable(World& world, float dt);
    // Build "captures/capture_YYYYMMDD_HHMMSS.png" + push to history.
    std::string MakeCapturePath() const;
};
