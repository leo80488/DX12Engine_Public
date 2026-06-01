#pragma once

// NotifyTrackEditor — ImGui multi-track editor for TimelineComponent.
//
// Distinct from include/Editor/TimelineEditor.h (which edits bone keyframe
// curves). This editor operates on a TimelineComponent: a list of named
// tracks, each holding zero-or-more point Notifies AND zero-or-more
// NotifyStates (interval events).
//
// Usage from EditorLayer (or any other ImGui host):
//     Editor::NotifyTrackEditor m_notifyEditor;
//     // per frame:
//     m_notifyEditor.SetTarget(world->GetComponent<TimelineComponent>(e));
//     m_notifyEditor.SetEditingEntity(e, world);
//     m_notifyEditor.RenderInWindow("Timeline Editor", &m_showTimeline);
//
// Features (Stages 1–6):
//   - Multi-track display, per-track header (name + category + mute)
//   - Time ruler at top, zoom + scroll
//   - Point notifies as triangles; NotifyStates as rectangles with two
//     resize handles
//   - Click-to-select, drag-to-move (Shift = 60 Hz frame snap)
//   - Right-click context menu: Add Notify / Add NotifyState / Duplicate /
//     Delete
//   - Property panel calls NotifyEditorRegistry per category
//   - Preview mode: editor scrubber writes AnimationComponent::primaryTime
//     on the editing entity, so notifies fire through the real
//     TimelineSystem mailbox path. "Editor sees == game sees."
//   - Undo / Redo via command pattern (Ctrl+Z / Ctrl+Y). Drag pushes a
//     single command on release, not per-frame.

#include "ECS/ECS.h"
#include "ECS/NotifyTypes.h"

#include <memory>
#include <vector>

namespace Editor
{

// ---- Command pattern for undo/redo --------------------------------------
struct INotifyCommand
{
    virtual ~INotifyCommand() = default;
    virtual void Apply  (TimelineComponent& tl) = 0;
    virtual void Revert (TimelineComponent& tl) = 0;
    virtual const char* Name() const = 0;
};

class NotifyTrackEditor
{
public:
    // Target: the TimelineComponent we edit. Pass nullptr to show the
    // "select an entity" empty state.
    void SetTarget(TimelineComponent* tl);

    // Editing context: used by Preview mode to write back into the
    // selected entity's AnimationComponent. Optional — without it, Preview
    // mode degrades to "editor-only scrubber" without firing real notifies.
    void SetEditingEntity(Entity e, World* world) { m_editingEntity = e; m_world = world; }

    TimelineComponent* GetTarget() const { return m_target; }

    // Current editor preview/scrubber time in clip-local seconds. Lets a host
    // panel align other views (e.g. a bone-curve overlay) to the same cursor.
    float GetPreviewTime() const { return m_previewTime; }

    // Convenience wrapper that issues ImGui::Begin/End around Render().
    void RenderInWindow(const char* title, bool* pOpen);

    // Body-only render (caller already inside an ImGui::Begin). Useful if
    // you want to dock this into a sub-region of another window.
    void Render();

private:
    // ---- pixel <-> time mapping ----
    float TimeToPixel(float t) const { return t * m_pixelsPerSecond - m_scrollX; }
    float PixelToTime(float px) const { return (px + m_scrollX) / m_pixelsPerSecond; }

    // ---- sub-draws ----
    void DrawToolbar();
    void DrawRuler(float canvasX, float canvasY, float width);
    void DrawTrackHeader(int trackIdx, float canvasX, float rowY, float headerWidth);
    void DrawTrackContent(int trackIdx, float canvasX, float rowY, float width);
    void DrawCurrentTimeCursor(float canvasX, float topY, float bottomY);
    void DrawPropertyPanel();
    void HandleContextMenu();

    // ---- preview integration ----
    // Pushes m_previewTime into AnimationComponent::primaryTime on the
    // editing entity so the runtime path (AnimationSystem → TimelineSystem
    // → mailboxes → consumer systems) sees the same time we draw.
    void ApplyPreviewTimeToEntity();

    // ---- helpers ----
    enum class SelKind : uint8_t { None, Notify, NotifyState };
    struct Selection {
        SelKind  kind = SelKind::None;
        uint32_t id   = 0u;
        bool operator==(const Selection& o) const { return kind == o.kind && id == o.id; }
    };

    Notify*      FindNotifyById     (uint32_t id, int* outTrackIdx = nullptr);
    NotifyState* FindNotifyStateById(uint32_t id, int* outTrackIdx = nullptr);

    // Selection is the "primary" pick (used by property panel). m_multiSel
    // holds additional picks for batch operations (delete, copy/paste).
    bool IsInMultiSel(const Selection& s) const;
    void AddToMultiSel(const Selection& s);
    void ClearMultiSel();
    void SelectExclusive(const Selection& s);

    void DeleteSelection();
    void DuplicateSelection();
    void CopySelectionToClipboard();
    void PasteClipboardAtTime(float t);

    // Pull AnimationComponent::primaryClip's duration into the timeline.
    // Requires SetEditingEntity to have been called; no-op otherwise.
    void SyncDurationFromActiveClip();

    // Auto-scroll the canvas when a drag is near a horizontal edge.
    void TickAutoScrollOnDrag(float canvasX, float canvasWidth);

    // Clamp m_scrollX to [0, contentEnd] for the given lane view width so
    // zooming / panning can't push all the tracks off-screen.
    void ClampScroll(float viewWidth);

    // ---- undo / redo ----
    void PushCommand(std::unique_ptr<INotifyCommand> cmd, bool runApply = true);
    void Undo();
    void Redo();

    // ---- target ----
    TimelineComponent* m_target        = nullptr;
    Entity             m_editingEntity = 0;            // NullEntity
    World*             m_world         = nullptr;

    // ---- view state ----
    float m_pixelsPerSecond = 200.f;
    float m_scrollX         = 0.f;
    float m_previewTime     = 0.f;
    bool  m_previewPlaying  = false;
    float m_previewSpeed    = 1.f;

    // ---- selection / drag ----
    Selection              m_selection;       // primary pick (drives property panel)
    std::vector<Selection> m_multiSel;        // additional picks; never contains m_selection
    enum class DragMode : uint8_t { None, Move, ResizeStart, ResizeEnd, BoxSelect };
    DragMode m_dragMode        = DragMode::None;
    float    m_dragStartT0     = 0.f;        // initial time / startTime
    float    m_dragStartT1     = 0.f;        // initial endTime (only for state)
    float    m_dragStartMouseX = 0.f;

    // Box-select rectangle screen coords (only valid while m_dragMode == BoxSelect).
    float m_boxStartX = 0.f, m_boxStartY = 0.f;
    float m_boxEndX   = 0.f, m_boxEndY   = 0.f;

    // ---- clipboard ----
    // Stores deep copies so paste survives the source being deleted /
    // edited. Layout: per-item, the original time is stored relative to
    // the bounding-box left edge so paste re-anchors at the cursor.
    struct ClipboardItem {
        SelKind     kind;
        Notify      n;
        NotifyState s;
        float       relTime;        // for Notify: time - boxLeft
        float       relTime2;       // for NotifyState: endTime - boxLeft
        int         trackOffset;    // delta from the topmost copied track
    };
    std::vector<ClipboardItem> m_clipboard;

    // ---- context menu state ----
    bool  m_openContextMenu  = false;
    int   m_pendingAddTrack  = -1;
    float m_pendingAddTime   = 0.f;

    // ---- undo / redo history ----
    std::vector<std::unique_ptr<INotifyCommand>> m_undoStack;
    std::vector<std::unique_ptr<INotifyCommand>> m_redoStack;
    static constexpr size_t kMaxHistory = 128;

    // ---- cached layout ----
    static constexpr float kHeaderColumnWidth = 240.f;  // roomier two-row header
    static constexpr float kTrackHeight       = 48.f;   // fits name row + controls row
    static constexpr float kRulerHeight       = 24.f;
};

} // namespace Editor
