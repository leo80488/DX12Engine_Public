#pragma once

// EditorLayer — ImGui Docking editor UI.
// Layout: MenuBar | top = Hierarchy | Viewport | Inspector | bottom = Resource.

#include "ECS/ECS.h"
#include "Editor/TimelineEditor.h"
#include "Editor/ImGuiManager.h"
#include "Graphics/RenderTypes.h"
#include "Reflection/ReflectionEditor.h"
#include "Resource/SystemHandles.h"
#include "imgui/imgui.h"
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <DirectXMath.h>
#include <functional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;
class Renderer;
class AnimationSystem;
struct GPUProfiler;
namespace Resource { class TextureSystem; class ResourceManager; class AssetManager; class AnimationClipSystem; }

enum class ViewportPlayState { Stopped, Playing, Paused };

class EditorLayer
{
public:
    EditorLayer();
    ~EditorLayer();

    // ===== Lifecycle =====
    void OnAttach();
    void OnDetach() {}
    // Init ImGui DX12+Win32 backends + window message hooks. Call once after SetGraphicsDevice().
    void InitImGuiBackends();
    // Register all built-in component editors + tags. Call once after SetRenderer().
    void RegisterDefaultEditors();

    // ===== Per-frame =====
    void BeginImGuiFrame();
    void EndImGuiFrame(RHI::CommandList cmd);
    // Draws DockSpace + all panels. Call once per frame.
    void OnUIRender();

    // ===== System injection =====
    void SetGraphicsDevice(IGraphicsDevice* gfx)             { m_gfx = gfx; }
    void SetRenderer(Renderer* renderer)                     { m_renderer = renderer; }
    void SetAssetManager(Resource::AssetManager* assetMgr)   { m_assetMgr = assetMgr; }
    void SetGPUProfiler(GPUProfiler* p)                      { m_gpuProfiler = p; }
    void SetCPUFrameTime(float ms)                           { m_cpuFrameMs = ms; }
    void SetAnimationClipSystem(Resource::AnimationClipSystem* acs) { m_animClipSys = acs; }
    void SetAnimationSystem(AnimationSystem* sys)            { m_animSys = sys; }
    void SetResourceSystems(Resource::TextureSystem* texSys, Resource::ResourceManager* rm)
    {
        m_textureSys  = texSys;
        m_resourceMgr = rm;
    }
    // Root directory scanned for converted engine assets (.itex, .ishdr …).
    void SetAssetDirectory(const std::string& dir)
    {
        m_assetDir   = dir;
        m_currentDir = dir;
        m_needsRescan = true;
    }

    // ===== ECS connection =====
    void SetWorld(World* world)                  { m_world = world; }
    void SetRenderView(const RenderView& view)   { m_renderView = view; }

    // ===== Viewport state (driven by App each frame) =====
    void SetSceneTextureId(unsigned long long texId);
    // F11: render viewport over the whole window (hide docking UI).
    void SetViewportFullscreen(bool fullscreen)  { m_viewportFullscreen = fullscreen; }
    // Last Viewport panel content size; (0,0) before first frame.
    void GetViewportSize(unsigned int& outWidth, unsigned int& outHeight) const;
    // Screen-space top-left of Viewport content. Used to translate window-space
    // mouse coords into panel-relative pixels for runtime UI hit-testing.
    void GetViewportMin(float& x, float& y) const { x = m_viewportMin.x; y = m_viewportMin.y; }
    void GetViewportMouseDelta(float& dx, float& dy) const { dx = m_vpMouseDX; dy = m_vpMouseDY; }
    bool IsViewportRightDragging() const         { return m_viewportRightDragging; }
    ViewportPlayState GetPlayState() const       { return m_playState; }
    // True for one frame after user clicks "Update 1 frame"; consume per frame.
    bool ConsumeWantsStepFrame();

    // ===== Window-toggle accessors =====
    void SetShowAnimDebug(bool show) { m_showAnimDebug = show; }
    bool GetShowAnimDebug() const    { return m_showAnimDebug; }

    // ===== Callbacks =====
    // meshType: 0=Cube, 1=Sphere, 2=Cone (called when user picks from Create menu).
    void SetCreateMeshCallback(std::function<void(int meshType)> cb)
    {
        m_createMeshCallback = std::move(cb);
    }
    // Left-click in viewport → callback receives viewport-relative pixel coords.
    void SetPickCallback(std::function<void(float px, float py)> cb)
    {
        m_pickCallback = std::move(cb);
    }
    // Called by App after Renderer::ResolvePick (1-frame delay).
    void SetPickResult(Entity entity);

    // ===== Component editor registration =====
    // drawFn receives (live component ptr, world, selected entity).
    template<typename T>
    void RegisterComponentEditor(const char* label,
                                  std::function<void(void*, World*, Entity)> drawFn,
                                  int priority = 100,
                                  std::function<bool(World&, Entity)> requiresFn = nullptr)
    {
        ComponentEditorEntry entry;
        entry.label    = label;
        entry.draw     = std::move(drawFn);
        entry.priority  = priority;
        entry.condition = std::move(requiresFn);
        entry.fetch = [](World& world, Entity e) -> void* {
            return world.GetComponent<T>(e);
        };
        entry.add = [](World& world, Entity e) {
            if (!world.HasComponent<T>(e))
                world.AddComponent<T>(e, T{});
        };
        entry.remove = [](World& world, Entity e) {
            world.RemoveComponent<T>(e);
        };
        m_componentEditors[std::type_index(typeid(T))] = std::move(entry);
        m_componentLabels [std::type_index(typeid(T))] = label;
    }

    // Bucket a registered component into an Add Component submenu (default "Misc").
    template<typename T>
    void SetComponentCategory(const char* category)
    {
        auto it = m_componentEditors.find(std::type_index(typeid(T)));
        if (it != m_componentEditors.end())
            it->second.category = category;
    }

    // Auto-generated inspector via Reflect::Descriptor<T>. Pass postDraw to append
    // hand-written widgets (buttons, conditional UI) after the field widgets.
    // Requires REFLECT_BEGIN(T) + REFLECT_END() visible at the call site.
    template<typename T>
    void RegisterReflectedComponent(const char* label,
                                    int priority = 100,
                                    std::function<void(T&, World*, Entity)> postDraw = nullptr,
                                    std::function<bool(World&, Entity)> requires_ = nullptr)
    {
        auto draw = [postDraw = std::move(postDraw)]
                    (void* data, World* w, Entity e)
        {
            T* obj = static_cast<T*>(data);
            Reflect::DrawObject(obj);
            if (postDraw) postDraw(*obj, w, e);
        };
        RegisterComponentEditor<T>(label, std::move(draw), priority, std::move(requires_));
    }

    // Display-only component (named row, no edit UI).
    template<typename T>
    void RegisterComponentTag(const char* label)
    {
        m_componentLabels[std::type_index(typeid(T))] = label;
    }

private:
    // ===== Panel render methods =====
    void SetupDockSpace();
    void RenderMenuBar();
    void RenderHierarchyPanel();
    void RenderViewportPanel();
    void RenderFullscreenViewport();
    void RenderViewportToolbar();
    void RenderInspectorPanel();
    void RenderResourcePanel();
    void RenderMeshLibraryTab();         // tab inside Resource panel
    void RenderPostProcessPanel();       // color grading / post-process settings
    void RenderTimelinePanel();          // toggled by m_showTimeline
    void RenderProfilerPanel();          // toggled by m_showProfiler
    void RenderAnimationDebugWindow();   // toggled by m_showAnimDebug
    void RenderDecalMaterialsWindow();   // toggled from Tools menu
    void RenderSSRDebugWindow();         // toggled by m_showSSRDebug
    void RenderFontEditorWindow();       // toggled by m_showFontEditor

    // Asset browser internals.
    enum class AssetType { Texture, Shader, Mesh, Material, Scene, Prefab, Skeleton, Animation, Audio, Script, Folder, Unknown };
    void ScanAssets();
    void RenderAssetGrid();
    void RunImport(AssetType type);

    // Per-frame helpers.
    void RenderMaterialInspector(struct MaterialComponent& mat);
    // Left-click pick + left-hold drag inside the viewport.
    void HandleViewportPicking(float imageMinX, float imageMinY,
                               float contentW,  float contentH,
                               bool  hovered);
    // ImGuizmo transform gizmo for the selected entity.
    void RenderViewportGizmo(ImVec2 vpMin, ImVec2 vpMax);

    // ===== ImGui backend ownership =====
    ImGuiManager m_imguiMgr;
    bool         m_imguiBackendsReady = false;

    // ===== System refs (injected, not owned) =====
    IGraphicsDevice*               m_gfx          = nullptr;
    Renderer*                      m_renderer     = nullptr;
    Resource::TextureSystem*       m_textureSys   = nullptr;
    Resource::ResourceManager*     m_resourceMgr  = nullptr;
    Resource::AssetManager*        m_assetMgr     = nullptr;
    Resource::AnimationClipSystem* m_animClipSys  = nullptr;
    AnimationSystem*               m_animSys      = nullptr;
    GPUProfiler*                   m_gpuProfiler  = nullptr;

    // ===== ECS state =====
    struct ComponentEditorEntry
    {
        std::string                                label;
        std::string                                category = "Misc"; // Add Component submenu
        std::function<void(void*, World*, Entity)> draw;     // live ptr + context
        std::function<void*(World&, Entity)>       fetch;
        std::function<void(World&, Entity)>        add;      // default-construct
        std::function<void(World&, Entity)>        remove;
        int                                        priority = 100;   // lower = first
        std::function<bool(World&, Entity)>        condition;        // null = always show
    };
    World*  m_world          = nullptr;
    Entity  m_selectedEntity = NullEntity;
    std::unordered_map<std::type_index, ComponentEditorEntry> m_componentEditors;
    std::unordered_map<std::type_index, std::string>          m_componentLabels;

    // ===== Render view (for picking ray construction) =====
    RenderView m_renderView{};

    // ===== Viewport state =====
    bool               m_layoutInitialized   = false;
    bool               m_viewportFullscreen  = false;
    bool               m_hasFontAwesomeIcons = false;
    unsigned long long m_sceneTextureId      = 0;
    ImVec2             m_viewportMin{};                       // screen-space top-left of viewport content
    unsigned int       m_viewportSizeX       = 0;
    unsigned int       m_viewportSizeY       = 0;
    ViewportPlayState  m_playState           = ViewportPlayState::Stopped;
    bool               m_wantsStepFrame      = false;
    bool               m_viewportRightDragging = false;
    float              m_vpMouseDX           = 0.f;
    float              m_vpMouseDY           = 0.f;

    // ===== Picking / drag =====
    std::function<void(int)>          m_createMeshCallback;
    std::function<void(float, float)> m_pickCallback;
    bool                              m_hasPendingDrag = false;  // click → pick result interim
    std::string                       m_pendingMatDropPath;      // .imat dropped on viewport
    bool                              m_isDragging      = false;
    Entity                            m_dragEntity      = NullEntity;
    DirectX::XMFLOAT3                 m_dragPlaneNormal{};        // camera-facing plane normal
    DirectX::XMFLOAT3                 m_dragPlanePoint {};        // a point on the drag plane
    DirectX::XMFLOAT3                 m_dragEntityOffset{};       // entity origin - hit point

    // Pending animation assignment (deferred until clip finishes async load).
    struct PendingAnimDrop
    {
        Resource::AnimHandle handle;
        Entity               target;
        std::string          filePath;  // .ianim source for prefab serialization
    };
    std::vector<PendingAnimDrop> m_pendingAnimDrops;

    // ===== Gizmo state =====
    ImGuizmo::OPERATION   m_gizmoOperation    = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE        m_gizmoMode         = ImGuizmo::LOCAL;
    // Cached world matrix fed to ImGuizmo — frozen while hovered/dragged so
    // hierarchy float drift doesn't flicker the gizmo.
    DirectX::XMFLOAT4X4   m_gizmoWorldMatrix  {};
    Entity                m_gizmoCachedEntity = NullEntity;

    // ===== Asset browser =====
    struct AssetEntry
    {
        std::string             path;       // full filesystem path
        std::string             stem;       // filename without extension
        AssetType               type        = AssetType::Unknown;
        Resource::TextureHandle thumbnail   = Resource::kInvalidTextureHandle;
        float                   lastVisibleTime = -1.f;        // ImGui time last in-view; <0 = never
    };
    std::string             m_assetDir;        // root (set once)
    std::string             m_currentDir;      // current browsing dir
    std::vector<AssetEntry> m_assets;
    bool                    m_needsRescan   = true;
    double                  m_lastScanTime  = -999.0;          // ImGui time of last scan
    int                     m_selectedAsset = -1;
    char                    m_assetFilter[128] {0};            // case-insensitive substring filter

    // Thumbnail streaming budget.
    int   m_previewLoadBudgetPerFrame = 4;
    int   m_previewLoadsThisFrame     = 0;
    float m_previewEvictDelay         = 8.f;                   // seconds off-screen before Release()

    // ===== Window-toggle flags =====
    bool m_showAnimDebug         = false;
    bool m_showSSRDebug          = false;
    bool m_showFontEditor        = false;
    bool m_showProbeVizSpheres   = true;       // walks "ReflectionProbe" Visibility each frame
    bool m_showTimeline          = false;
    bool m_showProfiler          = false;
    bool m_showPostProcess       = false;
    bool m_showDecalMaterials    = false;
    bool m_tracerTestModeEnabled = false;      // Debug menu; T fires a tracer along view ray

    // ===== Timeline editor =====
    Timeline::TimelineEditor m_timelineEditor;

    // ===== Profiler smoothing & history =====
    static constexpr int   kProfilerHistoryLen = 240;          // ~4 seconds at 60fps
    static constexpr float kProfilerEmaAlpha   = 0.05f;        // EMA smoothing factor
    static constexpr int   kMaxProfilerPasses  = 32;
    struct PassStats { const char* name = nullptr; float smoothedMs = 0.f; };
    float     m_cpuFrameMs   = 0.f;
    float     m_cpuSmoothed  = 0.f;
    float     m_gpuSmoothed  = 0.f;
    float     m_cpuHistory[kProfilerHistoryLen]{};
    float     m_gpuHistory[kProfilerHistoryLen]{};
    int       m_historyOffset = 0;                              // ring buffer write position
    PassStats m_passStats[kMaxProfilerPasses];
    int       m_passStatsCount = 0;
    float     m_cpuAvg = 0.f, m_cpuMin = 0.f, m_cpuMax = 0.f;
    float     m_gpuAvg = 0.f, m_gpuMin = 0.f, m_gpuMax = 0.f;
    int       m_statFrameCount = 0;                             // up to kProfilerHistoryLen

    // ===== Post-process =====
    // Path of the currently-bound post-process config (.ippc). Updated by
    // Save/Load buttons + LoadWorld; passed back into SaveWorld.
    std::string m_postProcessConfigPath;

    // ===== Log panel =====
    bool m_logShowSuccess = true;
    bool m_logShowInfo    = true;
    bool m_logShowWarning = true;
    bool m_logShowError   = true;
    // Auto-scroll: follows new lines while user is pinned to bottom; cancels
    // when user scrolls up, resumes when they scroll back down.
    bool m_logAutoScroll  = true;
    char m_logFilter[128] {0};

    // ===== Decal Materials window =====
    std::string m_selectedDecalMatName;        // "" = no selection
    char        m_newDecalMatNameBuf[64] {0};  // "Create" row text buffer
};
