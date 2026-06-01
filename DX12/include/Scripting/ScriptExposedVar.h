#pragma once

// ScriptExposedVar — editor-exposed Lua script variables (Unity/Unreal-style).
//
// A Logic script declares a static `exposed` table on its returned template:
//
//     local Rotate = {}
//     Rotate.exposed = {
//         amplitude = { type='float', default=0.5, min=0.0, max=5.0, tooltip='Bob height' },
//         frequency = { type='float', default=2.0, min=0.1, max=10.0 },
//         tint      = { type='color', default={0.35, 0.85, 2.80}, hdr=true },
//         clip      = { type='asset', default='', ext='.ianim' },
//         speed     = 1.0,        -- shorthand: a bare literal infers its type
//     }
//
// The engine parses that table ONCE into a vector<ScriptVarDesc> (the SCHEMA —
// types + defaults + UI metadata). The editor renders one ImGui widget per
// entry and stores per-entity edits as ScriptVarValue OVERRIDES on the
// ScriptComponent (which serialize into the scene). At spawn time the engine
// writes each value onto the per-entity Lua instance table so the script reads
// it as `self.<name>`.
//
// This header is intentionally sol-free and ImGui-free so ScriptComponent.h
// (widely included) can pull it in cheaply. The Lua parsing lives in
// ScriptSystem.cpp; the widget drawing lives in EditorLayer.cpp.

#include <cstdint>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// ScriptVarType — the editable kinds an exposed variable may take. The byte
// values are NOT serialized directly; the serializer uses a stable type CHAR
// (see ScriptVarTypeToChar) so reordering this enum stays safe.
// ---------------------------------------------------------------------------
enum class ScriptVarType : uint8_t
{
    Float,      // f — number, optional min/max → slider, else drag
    Int,        // i — integer number
    Bool,       // b — checkbox
    Float3,     // v — xyz vector (injected as Vec3 usertype: self.v.x/.y/.z)
    Color,      // c — rgb color (injected as Vec3: .x=r .y=g .z=b), optional HDR
    String,     // s — free text
    Entity,     // e — entity id (uint32)
    Asset,      // a — asset path string (with drag-drop + ext filter)
};

// ---------------------------------------------------------------------------
// ScriptVarValue — a concrete value for one exposed variable. Stored per-entity
// as an override on ScriptComponent and serialized inline into the scene.
// Mirrors the MaterialOverride::Value variant shape (POD union + side string).
// ---------------------------------------------------------------------------
struct ScriptVarValue
{
    ScriptVarType type = ScriptVarType::Float;
    union
    {
        float    f;
        int32_t  i;
        bool     b;
        float    v3[3];     // Float3 / Color (rgb)
        uint32_t entity;    // Entity id
    } data = {};
    std::string str;        // String / Asset path (empty otherwise)

    static ScriptVarValue MakeFloat (float v)              { ScriptVarValue r; r.type = ScriptVarType::Float;  r.data.f = v;  return r; }
    static ScriptVarValue MakeInt   (int32_t v)            { ScriptVarValue r; r.type = ScriptVarType::Int;    r.data.i = v;  return r; }
    static ScriptVarValue MakeBool  (bool v)               { ScriptVarValue r; r.type = ScriptVarType::Bool;   r.data.b = v;  return r; }
    static ScriptVarValue MakeFloat3(float x, float y, float z)
    { ScriptVarValue r; r.type = ScriptVarType::Float3; r.data.v3[0]=x; r.data.v3[1]=y; r.data.v3[2]=z; return r; }
    static ScriptVarValue MakeColor (float x, float y, float z)
    { ScriptVarValue r; r.type = ScriptVarType::Color;  r.data.v3[0]=x; r.data.v3[1]=y; r.data.v3[2]=z; return r; }
    static ScriptVarValue MakeString(std::string s)        { ScriptVarValue r; r.type = ScriptVarType::String; r.str = std::move(s); return r; }
    static ScriptVarValue MakeEntity(uint32_t e)           { ScriptVarValue r; r.type = ScriptVarType::Entity; r.data.entity = e; return r; }
    static ScriptVarValue MakeAsset (std::string s)        { ScriptVarValue r; r.type = ScriptVarType::Asset;  r.str = std::move(s); return r; }
};

// ---------------------------------------------------------------------------
// ScriptVarDesc — one entry of a script's SCHEMA, parsed from `exposed`.
// Holds the type + default + UI hints. The default doubles as the value the
// engine injects when an entity has no override for this variable.
// ---------------------------------------------------------------------------
struct ScriptVarDesc
{
    std::string    name;                 // lua field name == self.<name>
    std::string    label;                // inspector label (empty → use name)
    ScriptVarType  type   = ScriptVarType::Float;
    ScriptVarValue defVal;               // default value (type matches `type`)
    bool           hasMin = false;
    bool           hasMax = false;
    float          minVal = 0.f;
    float          maxVal = 0.f;
    float          speed  = 0.f;         // drag step (0 → auto)
    bool           hdr    = false;       // Color only — allow values > 1
    std::string    tooltip;
    std::string    assetExt;             // Asset only — e.g. ".ianim" (chooses drop payload)
};

// ---------------------------------------------------------------------------
// Stable serialization CHAR for a type (independent of enum byte values).
// ---------------------------------------------------------------------------
inline char ScriptVarTypeToChar(ScriptVarType t)
{
    switch (t)
    {
        case ScriptVarType::Float:  return 'f';
        case ScriptVarType::Int:    return 'i';
        case ScriptVarType::Bool:   return 'b';
        case ScriptVarType::Float3: return 'v';
        case ScriptVarType::Color:  return 'c';
        case ScriptVarType::String: return 's';
        case ScriptVarType::Entity: return 'e';
        case ScriptVarType::Asset:  return 'a';
    }
    return 'f';
}

inline bool ScriptVarTypeFromChar(char c, ScriptVarType& out)
{
    switch (c)
    {
        case 'f': out = ScriptVarType::Float;  return true;
        case 'i': out = ScriptVarType::Int;    return true;
        case 'b': out = ScriptVarType::Bool;   return true;
        case 'v': out = ScriptVarType::Float3; return true;
        case 'c': out = ScriptVarType::Color;  return true;
        case 's': out = ScriptVarType::String; return true;
        case 'e': out = ScriptVarType::Entity; return true;
        case 'a': out = ScriptVarType::Asset;  return true;
    }
    return false;
}

// Map an `exposed` descriptor's `type='...'` string to a ScriptVarType.
inline bool ScriptVarTypeFromString(const std::string& s, ScriptVarType& out)
{
    if (s == "float"  || s == "number")             { out = ScriptVarType::Float;  return true; }
    if (s == "int"    || s == "integer")            { out = ScriptVarType::Int;    return true; }
    if (s == "bool"   || s == "boolean")            { out = ScriptVarType::Bool;   return true; }
    if (s == "vec3"   || s == "float3")             { out = ScriptVarType::Float3; return true; }
    if (s == "color"  || s == "colour")             { out = ScriptVarType::Color;  return true; }
    if (s == "string" || s == "text")               { out = ScriptVarType::String; return true; }
    if (s == "entity")                              { out = ScriptVarType::Entity; return true; }
    if (s == "asset"  || s == "path")               { out = ScriptVarType::Asset;  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Resolve — the effective value for a variable: the per-entity override if one
// exists AND matches the schema type, otherwise the schema default. Used by
// both the editor (to seed widgets) and the engine (to inject onto the
// instance). A type-mismatched override is ignored (script changed the type).
// ---------------------------------------------------------------------------
inline ScriptVarValue ResolveScriptVar(const ScriptVarDesc& d,
                                       const std::unordered_map<std::string, ScriptVarValue>& overrides)
{
    auto it = overrides.find(d.name);
    if (it != overrides.end() && it->second.type == d.type)
        return it->second;
    return d.defVal;
}
