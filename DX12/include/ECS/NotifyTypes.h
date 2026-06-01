#pragma once

// NotifyTypes — Timeline / AnimNotify data layer.
//
// Pure data: no system, no editor, no DX12. Lives in EngineCore so both the
// runtime (TimelineSystem + consumer systems) and the editor (NotifyTrack-
// Editor) can depend on it.
//
// Concept (see timeline_system_architecture.md):
//   AnimationComponent provides "the body is moving" — currentTime advances
//   on a clip. TimelineComponent layers "AT what time does the world react?"
//   on top of that clip: a list of NotifyTracks, each holding zero-or-more
//   point Notifies (single-instant events). TimelineSystem watches the
//   clip's prev→curr time window each frame; any Notify whose `time` lies in
//   that window gets routed to the matching Pending* mailbox component on
//   the same entity. Consumer systems read those mailboxes the next phase
//   and act (spawn VFX, play audio, toggle hitbox, etc.).
//
// Hot-path uses Component-as-Mailbox (this file) for strict ordering. The
// existing EventBus is reserved for cross-system reactive broadcasts to Lua.

#include "ECS/ECS.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// NotifyCategory — top-level routing key. Tier 1 categories map to one
// dedicated Pending* mailbox each; StateToggle and Custom share generic
// mailboxes (Tier 2/3 from the architecture doc).
// ---------------------------------------------------------------------------
enum class NotifyCategory : uint8_t
{
    Hitbox      = 0,   // → PendingHitboxCommands
    VFX         = 1,   // → PendingVFXSpawns
    Camera      = 2,   // → PendingCameraEffects
    Audio       = 3,   // → PendingAudioPlays
    StateToggle = 4,   // → PendingStateToggles (generic, tag-driven)
    Custom      = 5,   // → PendingGenericNotifies (designer / Lua)

    COUNT
};

// ---------------------------------------------------------------------------
// StringID — interned-style hash wrapper around std::string for designer-
// authored type IDs ("OnFootstep", "ComboWindowEnd"). The hash is used for
// fast comparison in hot paths; the source string is kept for debugging and
// serialization. Identity is hash-based, so two StringIDs constructed from
// the same source compare equal.
// ---------------------------------------------------------------------------
struct StringID
{
    uint64_t    hash = 0;
    std::string str;

    StringID() = default;
    explicit StringID(std::string s) : hash(Fnv1a(s)), str(std::move(s)) {}

    bool IsValid() const { return hash != 0; }

    bool operator==(const StringID& o) const { return hash == o.hash; }
    bool operator!=(const StringID& o) const { return hash != o.hash; }

    static uint64_t Fnv1a(const std::string& s)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        return h ? h : 1ull;
    }
};

namespace std {
    template <> struct hash<StringID> {
        size_t operator()(const StringID& s) const noexcept { return static_cast<size_t>(s.hash); }
    };
}

// ---------------------------------------------------------------------------
// PropertyBag — generic key→value store for Notify params. Covers the small
// set of primitive types a designer needs to author without dragging in a
// reflection system.
// ---------------------------------------------------------------------------
struct PropertyBag
{
    using Value = std::variant<int, float, std::string, DirectX::XMFLOAT3, DirectX::XMFLOAT4>;
    std::unordered_map<std::string, Value> values;

    // ---- typed accessors ----
    int GetInt(const std::string& k, int def = 0) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<int>(&it->second)) return *v;
        return def;
    }
    void SetInt(const std::string& k, int v) { values[k] = v; }

    float GetFloat(const std::string& k, float def = 0.f) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<float>(&it->second)) return *v;
        return def;
    }
    void SetFloat(const std::string& k, float v) { values[k] = v; }

    std::string GetString(const std::string& k, const std::string& def = {}) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
        return def;
    }
    void SetString(const std::string& k, std::string v) { values[k] = std::move(v); }

    DirectX::XMFLOAT3 GetVec3(const std::string& k, DirectX::XMFLOAT3 def = {0,0,0}) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<DirectX::XMFLOAT3>(&it->second)) return *v;
        return def;
    }
    void SetVec3(const std::string& k, DirectX::XMFLOAT3 v) { values[k] = v; }
};

// ---------------------------------------------------------------------------
// Notify — a single point event on a NotifyTrack. `time` is in clip-local
// seconds. `params` carries any category-specific payload (damage, slot,
// asset path, ...). `id` is a per-clip-stable identifier so the editor can
// keep track of a notify across drags and undo.
// ---------------------------------------------------------------------------
struct Notify
{
    uint32_t       id       = 0;          // assigned by editor; 0 = unset
    NotifyCategory category = NotifyCategory::Custom;
    float          time     = 0.f;        // clip-local seconds
    PropertyBag    params;

    // editor-only display fields (still serialized — cheap and useful)
    std::string    displayName;
    uint32_t       color    = 0xFF80C8FFu; // RRGGBBAA little-endian (ImU32 layout)
};

// ---------------------------------------------------------------------------
// NotifyState — an interval event with Begin / Tick / End semantics.
// Unlike point Notify, the consumer system observes three distinct phases:
//   - Begin: time crossed `startTime` going forward
//   - Tick : current frame's prev/curr both lie inside (startTime, endTime)
//   - End  : time crossed `endTime` (going forward, or wrap)
// TimelineSystem encodes the phase in the dispatched mailbox payload — the
// existing PendingHitboxCommands::Cmd::type field carries Begin / End for
// the hitbox case, etc.
// ---------------------------------------------------------------------------
struct NotifyState
{
    uint32_t       id       = 0;
    NotifyCategory category = NotifyCategory::Custom;
    float          startTime = 0.f;
    float          endTime   = 0.1f;
    PropertyBag    params;

    std::string    displayName;
    uint32_t       color = 0xFF80C8FFu;
};

// ---------------------------------------------------------------------------
// NotifyTrack — one named row in the timeline editor. Constrains the
// category of every notify it holds, so a "Hitbox" track only contains
// Hitbox notifies. `muted` lets the designer disable a whole row at runtime
// without deleting it.
// ---------------------------------------------------------------------------
struct NotifyTrack
{
    std::string              name;
    NotifyCategory           category = NotifyCategory::Custom;
    bool                     muted    = false;
    std::vector<Notify>      notifies;
    std::vector<NotifyState> states;
};

// ---------------------------------------------------------------------------
// TimelineComponent — per-entity collection of NotifyTracks bound to the
// entity's AnimationComponent. `clipDuration` mirrors the active clip's
// length so the editor and the cross-time guard can wrap correctly on
// looping clips. `lastObservedTime` is runtime CPU state: TimelineSystem
// initializes it from AnimationComponent::primaryTime on first sight and
// updates it every frame.
// ---------------------------------------------------------------------------
struct TimelineComponent
{
    std::vector<NotifyTrack> tracks;
    float                    clipDuration     = 1.f;
    float                    lastObservedTime = -1.f;  // <0 = uninitialized; first frame just snaps
    uint32_t                 nextNotifyId     = 1u;    // monotonic editor allocator
};

// ---------------------------------------------------------------------------
// Pending* mailbox components.
//
// Written by TimelineSystem (or other producers); consumed and cleared by
// the matching consumer system the following phase. One component per
// engine subsystem — Hitbox commands collapse Begin/End/Change into one
// stream; Camera effects collapse Shake/Zoom/HitStop; etc.
//
// All mailboxes follow the same lifecycle: producer pushes back, consumer
// pops front-to-back and clears the vector. They are NOT auto-removed
// between frames if the consumer system is missing — that would mask a
// scheduler ordering bug.
// ---------------------------------------------------------------------------

// ---- Tier 1: Hitbox ----

enum class HitboxCmdType : uint8_t { Begin, End, ChangeShape, ChangeDamage };

struct HitboxParams
{
    DirectX::XMFLOAT3 localOffset = { 0, 0, 0 };
    float             radius      = 0.5f;          // sphere for now; future: shape variant
    float             damage      = 10.f;
    uint32_t          hitGroup    = 0;             // dedupe key — same group = same swing
};

struct PendingHitboxCommands
{
    struct Cmd {
        HitboxCmdType type    = HitboxCmdType::Begin;
        uint32_t      slot    = 0;                 // multiple hitboxes per entity
        HitboxParams  params;
    };
    std::vector<Cmd> commands;
};

// ---- Tier 1: VFX ----

enum class VFXAttachMode : uint8_t { Detached, AttachToBone, FollowEntity };

struct PendingVFXSpawns
{
    struct Spawn {
        std::string       assetPath;               // e.g. "FX/sword_trail.iparticle"
        VFXAttachMode     mode      = VFXAttachMode::Detached;
        int32_t           boneIndex = -1;          // valid when mode == AttachToBone
        DirectX::XMFLOAT3 localPos  = { 0, 0, 0 };
        float             duration  = 1.0f;        // <=0 = use asset default
    };
    std::vector<Spawn> spawns;
};

// ---- Tier 1: Camera ----

enum class CameraEffectType : uint8_t { Shake, Zoom, FOVPunch, HitStop };

struct CameraEffectParams
{
    // Shake
    float shakeAmplitude = 0.2f;
    float shakeFrequency = 20.f;
    // Zoom / FOVPunch
    float fovDelta       = 0.f;       // degrees
    // HitStop
    float duration       = 0.05f;
    float timeScale      = 0.05f;     // 0 = freeze
};

struct PendingCameraEffects
{
    struct Effect {
        CameraEffectType   type = CameraEffectType::Shake;
        CameraEffectParams params;
    };
    std::vector<Effect> effects;
};

// ---- Tier 1: Audio ----

enum class AudioCategoryTag : uint8_t { SFX, Voice, Foley, Music };

struct PendingAudioPlays
{
    struct Play {
        std::string      clipPath;                 // resolved by AudioSystem
        AudioCategoryTag category   = AudioCategoryTag::SFX;
        float            volume     = 1.f;
        float            pitch      = 1.f;
        int32_t          attachBone = -1;          // <0 = play 2D
    };
    std::vector<Play> plays;
};

// ---- Tier 2: Generic state toggles ----

enum class StateTag : uint32_t
{
    IFrame             = 1,
    SuperArmor         = 2,
    InputBufferOpen    = 3,
    RootMotionEnabled  = 4,
    Invincible         = 5,
    Vulnerable         = 6,
    GuardBreakable     = 7,
    HyperArmor         = 8,
    // designer-extensible; values 1024+ reserved for game-specific tags
};

struct PendingStateToggles
{
    struct Toggle {
        StateTag tag      = StateTag::IFrame;
        bool     enable   = true;
        float    duration = 0.f;   // 0 = persist until next toggle; >0 = auto-clear timer
    };
    std::vector<Toggle> toggles;
};

// ---- Tier 3: Custom designer / Lua notifies ----

struct PendingGenericNotifies
{
    struct Notify {
        StringID    typeId;
        PropertyBag params;
    };
    std::vector<Notify> notifies;
};
