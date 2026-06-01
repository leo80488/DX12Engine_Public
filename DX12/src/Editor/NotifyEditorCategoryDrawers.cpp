// NotifyEditorCategoryDrawers — concrete per-NotifyCategory property panels
// registered into NotifyEditorRegistry.
//
// The architecture doc deliberately keeps NotifyTrackEditor unaware of
// per-category fields ("editor framework doesn't know each subsystem's
// details"). This TU is the matching half of that contract: each engine
// subsystem (or this central file as a stand-in until they split out) is
// responsible for telling the registry what to draw.
//
// Today everything lives in one file — easier to ship and small enough
// that no one's bothered yet. When Hitbox / Audio / Camera each grow
// editor-only details, lift the relevant lambda out into that subsystem's
// own .cpp and remove it from here.

#include "Editor/NotifyEditorRegistry.h"
#include "Editor/NotifyEditorCategoryDrawers.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Inline char-buf editor backed by a std::string-typed PropertyBag entry.
    void StringField(PropertyBag& bag, const char* key, const char* label,
                     const char* defaultVal = "")
    {
        auto cur = bag.GetString(key, defaultVal);
        char buf[256];
        const size_t n = std::min(cur.size(), sizeof(buf) - 1);
        std::memcpy(buf, cur.data(), n);
        buf[n] = '\0';
        if (ImGui::InputText(label, buf, sizeof(buf)))
            bag.SetString(key, buf);
    }
    void IntField(PropertyBag& bag, const char* key, const char* label,
                  int defaultVal = 0, int lo = INT_MIN, int hi = INT_MAX)
    {
        int v = bag.GetInt(key, defaultVal);
        if (ImGui::DragInt(label, &v, 1.f, lo, hi)) bag.SetInt(key, v);
    }
    void FloatField(PropertyBag& bag, const char* key, const char* label,
                    float defaultVal = 0.f, float lo = -1e6f, float hi = 1e6f,
                    const char* fmt = "%.3f", float step = 0.01f)
    {
        float v = bag.GetFloat(key, defaultVal);
        if (ImGui::DragFloat(label, &v, step, lo, hi, fmt)) bag.SetFloat(key, v);
    }
    void Vec3Field(PropertyBag& bag, const char* key, const char* label,
                   DirectX::XMFLOAT3 defaultVal = {0,0,0})
    {
        DirectX::XMFLOAT3 v = bag.GetVec3(key, defaultVal);
        if (ImGui::DragFloat3(label, &v.x, 0.01f)) bag.SetVec3(key, v);
    }
    void EnumField(PropertyBag& bag, const char* key, const char* label,
                   const char* const* names, int count, int defaultVal = 0)
    {
        int v = bag.GetInt(key, defaultVal);
        v = std::clamp(v, 0, count - 1);
        if (ImGui::Combo(label, &v, names, count)) bag.SetInt(key, v);
    }
}

void RegisterDefaultNotifyEditors()
{
    // ---- Hitbox ---------------------------------------------------------
    // `cmdType` is only meaningful for point Notifies — for NotifyState
    // intervals, TimelineSystem injects Begin / End itself. We still expose
    // the combo so a point notify can author a one-shot End on its own.
    NotifyEditorRegistry::Register(NotifyCategory::Hitbox, [](Notify& n) {
        static const char* kCmdNames[] = { "Begin", "End", "ChangeShape", "ChangeDamage" };
        EnumField(n.params, "cmdType",  "Cmd",          kCmdNames, IM_ARRAYSIZE(kCmdNames), 0);
        IntField (n.params, "slot",     "Slot",         0, 0, 16);
        IntField (n.params, "hitGroup", "Hit Group",    0, 0, 1024);
        FloatField(n.params, "radius",  "Radius (m)",   0.5f, 0.05f, 10.f, "%.2f", 0.01f);
        FloatField(n.params, "damage",  "Damage",      10.f,  0.f,   9999.f, "%.1f", 0.5f);
        Vec3Field (n.params, "offset",  "Local Offset");
    });

    // ---- VFX ------------------------------------------------------------
    NotifyEditorRegistry::Register(NotifyCategory::VFX, [](Notify& n) {
        StringField(n.params, "asset",   "Asset Path");
        static const char* kAttach[] = { "Detached", "AttachToBone", "FollowEntity" };
        EnumField (n.params, "attach",  "Attach Mode", kAttach, IM_ARRAYSIZE(kAttach), 0);
        IntField  (n.params, "bone",    "Bone Index", -1, -1, 256);
        Vec3Field (n.params, "offset",  "Local Offset");
        FloatField(n.params, "duration","Duration (s)", 1.f, 0.f, 60.f, "%.2f", 0.05f);
    });

    // ---- Camera ---------------------------------------------------------
    NotifyEditorRegistry::Register(NotifyCategory::Camera, [](Notify& n) {
        static const char* kKind[] = { "Shake", "Zoom", "FOVPunch", "HitStop" };
        EnumField (n.params, "kind",     "Effect",       kKind, IM_ARRAYSIZE(kKind), 0);

        const int kind = n.params.GetInt("kind", 0);
        if (kind == 0) {  // Shake
            // "amp" is consumed as camera TRAUMA (0..1); CameraShakeTickSystem
            // uses trauma² for the noise strength, so values below ~0.4 are
            // barely visible. Default 0.6 gives a clearly-felt shake.
            FloatField(n.params, "amp",   "Trauma (0..1)", 0.6f, 0.f, 1.f, "%.2f", 0.01f);
            FloatField(n.params, "freq",  "Frequency", 20.f, 0.f, 100.f);
            FloatField(n.params, "duration", "Duration (s)", 0.2f, 0.f, 5.f);
        } else if (kind == 1 || kind == 2) {  // Zoom / FOVPunch
            FloatField(n.params, "fovDelta", "FOV Delta (deg)", -5.f, -45.f, 45.f);
            FloatField(n.params, "duration", "Duration (s)", 0.2f, 0.f, 5.f);
        } else if (kind == 3) {  // HitStop
            FloatField(n.params, "duration", "Duration (s)", 0.05f, 0.f, 1.f);
            FloatField(n.params, "scale",    "Time Scale",   0.05f, 0.f, 1.f);
        }
    });

    // ---- Audio ----------------------------------------------------------
    NotifyEditorRegistry::Register(NotifyCategory::Audio, [](Notify& n) {
        StringField(n.params, "clip",   "Clip Path");
        static const char* kBus[] = { "SFX", "Voice", "Foley", "Music" };
        EnumField (n.params, "cat",    "Category", kBus, IM_ARRAYSIZE(kBus), 0);
        FloatField(n.params, "volume", "Volume",   1.f, 0.f, 4.f, "%.2f", 0.02f);
        FloatField(n.params, "pitch",  "Pitch",    1.f, 0.25f, 4.f, "%.2f", 0.01f);
        IntField  (n.params, "bone",   "Attach Bone (-1 = 2D)", -1, -1, 256);
    });

    // ---- StateToggle ----------------------------------------------------
    NotifyEditorRegistry::Register(NotifyCategory::StateToggle, [](Notify& n) {
        static const char* kTags[] = {
            "(unset)", "IFrame", "SuperArmor", "InputBufferOpen", "RootMotionEnabled",
            "Invincible", "Vulnerable", "GuardBreakable", "HyperArmor",
        };
        EnumField (n.params, "tag",      "Tag", kTags, IM_ARRAYSIZE(kTags), 1);
        IntField  (n.params, "enable",   "Enable (0/1)", 1, 0, 1);
        FloatField(n.params, "duration", "Auto-clear (s, 0=persist)", 0.f, 0.f, 30.f, "%.2f", 0.05f);
    });

    // ---- Custom (Tier 3) — keep the generic PropertyBag fallback so
    //      designers can author arbitrary key/value pairs that Lua picks up
    //      via the `CustomNotifyEvent` broadcast. No specific drawer here.
}
