#pragma once

// Reflect — minimal compile-time reflection for engine components.
//
// Goal: let editor / serializer code walk a component's fields by name,
// type, and metadata (min/max range, color hint, enum options) without
// each component repeating the same property list in 3 places (struct
// declaration, ImGui editor block, key=value serializer).
//
// Usage — declare a descriptor for a component AFTER its struct definition:
//
//     struct BillboardComponent { BillboardMode mode; float worldSize; bool fixedSize; };
//
//     constexpr Reflect::EnumOption kBillboardModeOptions[] = {
//         { (int)BillboardMode::Opaque,      "Opaque" },
//         { (int)BillboardMode::Transparent, "Transparent" },
//     };
//
//     REFLECT_BEGIN(BillboardComponent)
//         REFLECT_ENUM (mode,        "Mode",       kBillboardModeOptions)
//         REFLECT_FLOAT(worldSize,   "World Size", 0.01f, 100.f)
//         REFLECT_BOOL (fixedSize,   "Fixed Size")
//     REFLECT_END()
//
// Then `Reflect::Describe<BillboardComponent>()` returns a TypeDescriptor
// that the editor's generic drawer (ReflectionEditor.h) walks to produce
// the inspector UI.
//
// Header-only on purpose: zero runtime dependencies. The drawer that
// consumes descriptors lives in Reflection/ReflectionEditor.h.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace Reflect
{
struct FieldDescriptor;
struct TypeDescriptor;

// -----------------------------------------------------------------------
// FieldKind — primitive data type stored in the field. Drives how the
// drawer reads/writes raw memory at the offset; widget hint then chooses
// which ImGui control to use for that data.
// -----------------------------------------------------------------------
enum class FieldKind : uint8_t
{
    F32,            // float
    F32x2,          // DirectX::XMFLOAT2
    F32x3,          // DirectX::XMFLOAT3
    F32x4,          // DirectX::XMFLOAT4
    I32,            // int32_t
    U32,            // uint32_t
    Bool,           // bool
    Enum,           // backed by 1/2/4-byte integer; options describe values
    String,         // std::string (optionally accepts drag-drop payload)
    Header,         // not a field — visual section divider with a label
    InfoText,       // not a field — TextDisabled line with literal label
    InfoTextFn,     // not a field — TextDisabled line formatted by infoFn(obj)
    Vector,         // std::vector<T> — elementDesc + vec*() ops type-erase T
    GroupBegin,     // hide subsequent fields until matching GroupEnd if visible() is false
    GroupEnd,
    CollapseBegin,  // wrap subsequent fields in an ImGui::CollapsingHeader
    CollapseEnd,
    Custom,         // delegates entirely to customDraw
};

// -----------------------------------------------------------------------
// Widget — display hint chosen at descriptor time. Several widgets can
// apply to the same FieldKind (e.g. F32 → Drag / Slider / Angle).
// -----------------------------------------------------------------------
enum class Widget : uint8_t
{
    Default,    // Drag for numeric, Checkbox for bool, Combo for enum, InputText for string
    Drag,
    Slider,
    Color,      // F32x3 / F32x4 only — uses ImGui::ColorEdit
    Angle,      // F32 only — stored radians, edited in degrees
    InputText,  // String only
    Combo,      // Enum (default)
    Checkbox,   // Bool (default)
};

// -----------------------------------------------------------------------
// EnumOption — one entry in an enum option array. value is widened to int
// so a single struct works for enums backed by uint8_t / uint32_t / int.
// Always declare option arrays as constexpr at namespace or file scope.
// -----------------------------------------------------------------------
struct EnumOption
{
    int         value;
    const char* label;
};

// -----------------------------------------------------------------------
// FieldDescriptor — fully describes one editable property of a component.
// Stored by value inside TypeDescriptor::fields. All pointer members must
// reference data with static storage duration (string literals, constexpr
// arrays) — descriptors live in function-local statics for the lifetime
// of the program.
// -----------------------------------------------------------------------
struct FieldDescriptor
{
    const char*       name      = "";        // C++ member name (used for serialization keys)
    const char*       label     = "";        // ImGui display label
    std::size_t       offset    = 0;         // byte offset from object base
    FieldKind         kind      = FieldKind::F32;
    Widget            widget    = Widget::Default;
    float             minVal    = 0.f;       // slider/drag clamp; ignored for color/bool
    float             maxVal    = 0.f;       // 0,0 ⇒ no clamp (drag widgets)
    float             speed     = 0.01f;     // drag step
    const char*       fmt       = nullptr;   // printf format for numeric (nullptr = ImGui default)
    const char*       tooltip   = nullptr;   // optional hover tooltip
    std::size_t       enumSize  = 0;         // sizeof(EnumType) — 1/2/4 bytes
    const EnumOption* enumOpts  = nullptr;   // null when kind != Enum
    std::size_t       enumCount = 0;

    // Drag-drop payload type accepted by string fields. nullptr disables drop.
    const char*       dropPayload = nullptr;

    // Conditional rendering — when set, returns false to hide this field (or
    // gate the subsequent block until matching GroupEnd, for kind=GroupBegin).
    bool (*visible)(const void* obj) = nullptr;

    // Bypass kind-based drawing entirely. Receives a pointer to the field
    // (obj + offset) and the field descriptor. Returns true if changed.
    bool (*customDraw)(void* fieldPtr, const FieldDescriptor& f) = nullptr;

    // Format an info line each frame from the host object (kind == InfoTextFn).
    void (*infoFn)(const void* obj, char* outBuf, std::size_t outCap) = nullptr;

    // Vector-of-struct support (kind == Vector). All four pointers are bound
    // by MakeVector<T> at descriptor build time so the drawer can iterate
    // std::vector<T> without knowing T at the call site.
    const TypeDescriptor* elementDesc = nullptr;
    std::size_t (*vecSize  )(const void* vecPtr) = nullptr;
    void*       (*vecData  )(void* vecPtr, std::size_t i) = nullptr;
    void        (*vecAdd   )(void* vecPtr) = nullptr;
    void        (*vecRemove)(void* vecPtr, std::size_t i) = nullptr;

    // Fixed-size array support (kind == Vector with elementDesc != null and
    // vecSize == nullptr). countOffset points at the sibling uint/int that
    // tracks the live element count; capacity is the static array length.
    std::size_t       countOffset = 0;
    std::size_t       arrayCapacity = 0;
    bool              countIsInt   = false;  // true for `int count`, false for uint32
};

// -----------------------------------------------------------------------
// TypeDescriptor — a full reflection record for one component type.
// Built once via Descriptor<T>::Make() and cached in Describe<T>().
// -----------------------------------------------------------------------
struct TypeDescriptor
{
    const char*                  name = "";  // unqualified C++ type name
    std::size_t                  size = 0;   // sizeof(T) — sanity check
    std::vector<FieldDescriptor> fields;
};

// -----------------------------------------------------------------------
// Descriptor<T> — user specializes per component type via REFLECT_BEGIN.
// Default (unspecialized) primary template intentionally has no Make()
// so referencing Describe<T>() for a non-reflected type fails to compile
// with a clear "no member named 'Make'" error.
// -----------------------------------------------------------------------
template <typename T>
struct Descriptor;

template <typename T>
inline const TypeDescriptor& Describe()
{
    static const TypeDescriptor d = Descriptor<T>::Make();
    return d;
}

// -----------------------------------------------------------------------
// Field-builder helpers — invoked by the REFLECT_* macros below. Kept as
// free functions (not constructors) so each macro reads as one call and
// initializer-list aggregation stays simple.
// -----------------------------------------------------------------------
inline FieldDescriptor MakeFloat(const char* name, const char* label, std::size_t off,
                                 float mn, float mx,
                                 Widget w = Widget::Drag,
                                 const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32; f.widget = w;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat2(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x2; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat3(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x3; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeFloat4(const char* name, const char* label, std::size_t off,
                                  float mn, float mx, const char* fmt = nullptr, float speed = 0.01f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x4; f.widget = Widget::Drag;
    f.minVal = mn; f.maxVal = mx; f.fmt = fmt; f.speed = speed;
    return f;
}

inline FieldDescriptor MakeColor3(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x3; f.widget = Widget::Color;
    return f;
}

inline FieldDescriptor MakeColor4(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32x4; f.widget = Widget::Color;
    return f;
}

inline FieldDescriptor MakeInt(const char* name, const char* label, std::size_t off,
                               int mn, int mx, Widget w = Widget::Drag, float speed = 1.f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::I32; f.widget = w;
    f.minVal = static_cast<float>(mn); f.maxVal = static_cast<float>(mx); f.speed = speed;
    return f;
}

inline FieldDescriptor MakeUInt(const char* name, const char* label, std::size_t off,
                                unsigned mn, unsigned mx, Widget w = Widget::Drag, float speed = 1.f)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::U32; f.widget = w;
    f.minVal = static_cast<float>(mn); f.maxVal = static_cast<float>(mx); f.speed = speed;
    return f;
}

inline FieldDescriptor MakeBool(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Bool; f.widget = Widget::Checkbox;
    return f;
}

inline FieldDescriptor MakeEnum(const char* name, const char* label, std::size_t off,
                                std::size_t enumByteSize,
                                const EnumOption* opts, std::size_t optCount)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Enum; f.widget = Widget::Combo;
    f.enumSize = enumByteSize; f.enumOpts = opts; f.enumCount = optCount;
    return f;
}

// Float field stored in radians but edited as degrees with a Slider.
// Range is in degrees.
inline FieldDescriptor MakeAngle(const char* name, const char* label, std::size_t off,
                                 float minDeg, float maxDeg)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::F32; f.widget = Widget::Angle;
    f.minVal = minDeg; f.maxVal = maxDeg;
    return f;
}

inline FieldDescriptor MakeString(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::String; f.widget = Widget::InputText;
    return f;
}

// std::string field that also accepts an ImGui drag-drop payload of the given
// type (e.g. "ITEX_PATH", "ILUA_PATH"). The drop replaces the string contents.
inline FieldDescriptor MakeStringDrop(const char* name, const char* label,
                                       std::size_t off, const char* payloadType)
{
    FieldDescriptor f = MakeString(name, label, off);
    f.dropPayload = payloadType;
    return f;
}

// Free-form widget — drawer skips its switch and calls customDraw(fieldPtr, f).
// Use for fields whose UI doesn't fit the kind-based grid (e.g. quaternion as
// Euler with persistent cache, direction as azimuth/elevation).
inline FieldDescriptor MakeCustom(const char* name, const char* label, std::size_t off,
                                   bool (*draw)(void*, const FieldDescriptor&))
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Custom; f.customDraw = draw;
    return f;
}

// std::vector<T> support. T must have a Reflect::Descriptor<T> specialization.
// Element ops are type-erased through small lambdas baked into function pointers
// so the drawer can iterate without seeing T at the call site.
template <typename T>
inline FieldDescriptor MakeVector(const char* name, const char* label, std::size_t off)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Vector;
    f.elementDesc = &Describe<T>();
    f.vecSize = [](const void* v) -> std::size_t {
        return static_cast<const std::vector<T>*>(v)->size();
    };
    f.vecData = [](void* v, std::size_t i) -> void* {
        return &(*static_cast<std::vector<T>*>(v))[i];
    };
    f.vecAdd = [](void* v) {
        static_cast<std::vector<T>*>(v)->emplace_back();
    };
    f.vecRemove = [](void* v, std::size_t i) {
        auto& vec = *static_cast<std::vector<T>*>(v);
        if (i < vec.size()) vec.erase(vec.begin() + i);
    };
    return f;
}

// Fixed-size array `T arr[CAP]` paired with a sibling `count` field. Reuses
// the Vector kind path; vec*() pointers stay null so the drawer falls back
// to the count/capacity-based iteration.
template <typename T>
inline FieldDescriptor MakeArray(const char* name, const char* label,
                                  std::size_t off, std::size_t capacity,
                                  std::size_t countOff, bool countIsInt)
{
    FieldDescriptor f;
    f.name = name; f.label = label; f.offset = off;
    f.kind = FieldKind::Vector;
    f.elementDesc   = &Describe<T>();
    f.arrayCapacity = capacity;
    f.countOffset   = countOff;
    f.countIsInt    = countIsInt;
    // Lambdas perform pointer arithmetic on the raw element block instead
    // of going through std::vector — same drawer code path.
    f.vecData = [](void* arrayBase, std::size_t i) -> void* {
        return static_cast<T*>(arrayBase) + i;
    };
    return f;
}

// Pseudo-fields — render UI without reading object memory.

inline FieldDescriptor MakeInfoFn(const char* label,
                                   void (*fn)(const void*, char*, std::size_t))
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::InfoTextFn;
    f.infoFn = fn;
    return f;
}

inline FieldDescriptor MakeGroupBegin(bool (*pred)(const void* obj))
{
    FieldDescriptor f;
    f.kind = FieldKind::GroupBegin;
    f.visible = pred;
    return f;
}

inline FieldDescriptor MakeGroupEnd()
{
    FieldDescriptor f;
    f.kind = FieldKind::GroupEnd;
    return f;
}

// Wraps subsequent fields in an ImGui::CollapsingHeader. defaultOpen controls
// the first-frame state. Closes at MakeCollapseEnd.
inline FieldDescriptor MakeCollapseBegin(const char* label, bool defaultOpen = false)
{
    FieldDescriptor f;
    f.kind  = FieldKind::CollapseBegin;
    f.label = label;
    // Reuse minVal as a 1.0 marker for "default open" so we don't need yet
    // another field on FieldDescriptor.
    f.minVal = defaultOpen ? 1.f : 0.f;
    return f;
}

inline FieldDescriptor MakeCollapseEnd()
{
    FieldDescriptor f;
    f.kind = FieldKind::CollapseEnd;
    return f;
}

inline FieldDescriptor MakeHeader(const char* label)
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::Header;
    return f;
}

inline FieldDescriptor MakeInfoText(const char* label)
{
    FieldDescriptor f;
    f.label = label;
    f.kind  = FieldKind::InfoText;
    return f;
}

// Attach a hover tooltip to the most-recently-added field. Returns the
// same FieldDescriptor by value so it slots into the initializer-list
// flow without needing a separate statement.
inline FieldDescriptor WithTooltip(FieldDescriptor f, const char* tip)
{
    f.tooltip = tip;
    return f;
}

} // namespace Reflect

// ---------------------------------------------------------------------------
// REFLECT_BEGIN / REFLECT_END — define a Descriptor<T> specialization. The
// body is the C++ initializer list for the fields vector; one REFLECT_*
// macro per field. Place at namespace scope (the macros open the Reflect
// namespace internally).
//
// Example:
//   REFLECT_BEGIN(BillboardComponent)
//       REFLECT_FLOAT(worldSize, "World Size", 0.01f, 100.f)
//       REFLECT_BOOL (fixedSize, "Fixed Screen Size")
//   REFLECT_END()
// ---------------------------------------------------------------------------

#define REFLECT_BEGIN(TYPE)                                              \
    namespace Reflect {                                                  \
    template <> struct Descriptor<TYPE> {                                \
        using ThisType = TYPE;                                           \
        static TypeDescriptor Make() {                                   \
            TypeDescriptor d;                                            \
            d.name = #TYPE;                                              \
            d.size = sizeof(TYPE);                                       \
            d.fields = {

#define REFLECT_END()                                                    \
            };                                                           \
            return d;                                                    \
        }                                                                \
    };                                                                   \
    }

// Per-field macros — each emits one FieldDescriptor entry into the list.
// Use offsetof() so the descriptor records the raw byte offset; the drawer
// reads/writes the field via reinterpret_cast at runtime.

#define REFLECT_FLOAT(NAME, LABEL, MIN, MAX) \
    Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX)),

#define REFLECT_FLOAT_FMT(NAME, LABEL, MIN, MAX, FMT, SPEED) \
    Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX), \
                       Reflect::Widget::Drag, FMT, (float)(SPEED)),

#define REFLECT_SLIDER(NAME, LABEL, MIN, MAX) \
    Reflect::MakeFloat(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX), \
                       Reflect::Widget::Slider),

#define REFLECT_FLOAT2(NAME, LABEL, MIN, MAX) \
    Reflect::MakeFloat2(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX)),

#define REFLECT_FLOAT3(NAME, LABEL, MIN, MAX) \
    Reflect::MakeFloat3(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX)),

#define REFLECT_FLOAT4(NAME, LABEL, MIN, MAX) \
    Reflect::MakeFloat4(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN), (float)(MAX)),

#define REFLECT_COLOR3(NAME, LABEL) \
    Reflect::MakeColor3(#NAME, LABEL, offsetof(ThisType, NAME)),

#define REFLECT_COLOR4(NAME, LABEL) \
    Reflect::MakeColor4(#NAME, LABEL, offsetof(ThisType, NAME)),

#define REFLECT_INT(NAME, LABEL, MIN, MAX) \
    Reflect::MakeInt(#NAME, LABEL, offsetof(ThisType, NAME), (int)(MIN), (int)(MAX)),

#define REFLECT_SLIDER_INT(NAME, LABEL, MIN, MAX) \
    Reflect::MakeInt(#NAME, LABEL, offsetof(ThisType, NAME), (int)(MIN), (int)(MAX), Reflect::Widget::Slider),

#define REFLECT_UINT(NAME, LABEL, MIN, MAX) \
    Reflect::MakeUInt(#NAME, LABEL, offsetof(ThisType, NAME), (unsigned)(MIN), (unsigned)(MAX)),

#define REFLECT_BOOL(NAME, LABEL) \
    Reflect::MakeBool(#NAME, LABEL, offsetof(ThisType, NAME)),

// OPTIONS must be a constexpr Reflect::EnumOption array visible at the
// REFLECT_END point. sizeof(((T*)0)->NAME) gives the underlying byte size
// (works for enum class with explicit underlying type and plain enums).
#define REFLECT_ENUM(NAME, LABEL, OPTIONS) \
    Reflect::MakeEnum(#NAME, LABEL, offsetof(ThisType, NAME), \
                      sizeof(((ThisType*)0)->NAME), \
                      (OPTIONS), sizeof(OPTIONS) / sizeof((OPTIONS)[0])),

// Float field stored in radians; UI shows degrees in MIN..MAX range.
#define REFLECT_ANGLE(NAME, LABEL, MIN_DEG, MAX_DEG) \
    Reflect::MakeAngle(#NAME, LABEL, offsetof(ThisType, NAME), (float)(MIN_DEG), (float)(MAX_DEG)),

#define REFLECT_STRING(NAME, LABEL) \
    Reflect::MakeString(#NAME, LABEL, offsetof(ThisType, NAME)),

// String field that also accepts the named ImGui drag-drop payload.
#define REFLECT_STRING_DROP(NAME, LABEL, PAYLOAD) \
    Reflect::MakeStringDrop(#NAME, LABEL, offsetof(ThisType, NAME), PAYLOAD),

// std::vector<T> — T must have its own REFLECT_BEGIN/END descriptor.
#define REFLECT_VECTOR(NAME, LABEL, ELEMENT_T) \
    Reflect::MakeVector<ELEMENT_T>(#NAME, LABEL, offsetof(ThisType, NAME)),

// Fixed C-array `T arr[CAP]` paired with sibling `count` field. CAP is the
// static capacity, COUNT_FIELD is the unsigned/int count member name.
#define REFLECT_ARRAY(NAME, LABEL, ELEMENT_T, CAP, COUNT_FIELD) \
    Reflect::MakeArray<ELEMENT_T>(#NAME, LABEL, offsetof(ThisType, NAME), \
        (std::size_t)(CAP), offsetof(ThisType, COUNT_FIELD), \
        std::is_signed_v<decltype(ThisType::COUNT_FIELD)>),

// Field whose UI is entirely hand-written. DRAW_FN must be a free function
// `bool(void* fieldPtr, const Reflect::FieldDescriptor&)`.
#define REFLECT_CUSTOM(NAME, LABEL, DRAW_FN) \
    Reflect::MakeCustom(#NAME, LABEL, offsetof(ThisType, NAME), DRAW_FN),

// Visual-only entries — render a Separator+label or a TextDisabled line.
#define REFLECT_HEADER(LABEL) \
    Reflect::MakeHeader(LABEL),

#define REFLECT_INFO(TEXT) \
    Reflect::MakeInfoText(TEXT),

// Dynamic info line. FN is `void(const void* obj, char* buf, std::size_t cap)`.
#define REFLECT_INFO_FN(LABEL, FN) \
    Reflect::MakeInfoFn(LABEL, FN),

// Conditional block — fields between IF/ENDIF render only when PRED(obj) is
// true. PRED is `bool(const void* obj)`. Captureless lambdas convert.
#define REFLECT_IF(PRED) \
    Reflect::MakeGroupBegin(PRED),

#define REFLECT_ENDIF() \
    Reflect::MakeGroupEnd(),

// CollapsingHeader wrapper. Fields between BEGIN/END render only when the
// header is open. Pass true for default-open headers.
#define REFLECT_COLLAPSE(LABEL) \
    Reflect::MakeCollapseBegin(LABEL, false),

#define REFLECT_COLLAPSE_OPEN(LABEL) \
    Reflect::MakeCollapseBegin(LABEL, true),

#define REFLECT_COLLAPSE_END() \
    Reflect::MakeCollapseEnd(),

// Attach a hover tooltip to the previous field by re-wrapping the trailing
// entry. Use as a separate macro on its own line BEFORE the field whose
// tooltip you want to set is final — actually easier: pass tooltip via a
// dedicated TIP suffix variant. Keep the WithTooltip helper exposed for
// hand-written descriptors that need it.
