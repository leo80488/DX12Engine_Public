#pragma once

// SceneGraph → ECS hierarchy components.
// These are written by SceneLoader and propagated by TransformSystem.
// Keep this header free of GPU / DX12 types — pure data only.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // Resource::MeshHandle

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <vector>

// ---------------------------------------------------------------------------
// LocalTransform — TRS relative to parent node.
// Uses quaternion rotation (xyzw) for singularity-free composition.
// Written by user code / animation; TransformSystem reads it.
// ---------------------------------------------------------------------------
struct LocalTransform
{
    DirectX::XMFLOAT3 translation = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 rotation    = { 0.f, 0.f, 0.f, 1.f };  // quaternion xyzw
    DirectX::XMFLOAT3 scale       = { 1.f, 1.f, 1.f };

    // Returns the local SRT matrix in DirectXMath (row-vector) convention.
    DirectX::XMMATRIX ToMatrix() const
    {
        using namespace DirectX;
        const XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scale));
        const XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rotation));
        const XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&translation));
        return S * R * T;
    }
};

// ---------------------------------------------------------------------------
// GlobalTransform — world-space 4×4 matrix, computed by TransformSystem.
// Do NOT write this manually; it is overwritten every Propagate() call.
// ---------------------------------------------------------------------------
struct GlobalTransform
{
    DirectX::XMFLOAT4X4 matrix;
    GlobalTransform() { DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixIdentity()); }
};

// ---------------------------------------------------------------------------
// Parent — reference to parent entity.  NullEntity = root (no parent).
// ---------------------------------------------------------------------------
struct Parent
{
    Entity entity = NullEntity;
};

// ---------------------------------------------------------------------------
// Children — ordered list of immediate child entities.
// Maintained by SceneLoader; update on reparent / destroy.
// ---------------------------------------------------------------------------
struct Children
{
    std::vector<Entity> entities;
};

// ---------------------------------------------------------------------------
// Visibility — per-entity rendering flag.
// is_visible       : user-settable toggle (culling system may also clear it)
// inherited_hidden : set by TransformSystem when any ancestor is invisible
// ---------------------------------------------------------------------------
struct Visibility
{
    bool is_visible       = true;
    bool inherited_hidden = false;

    bool IsEffectivelyVisible() const { return is_visible && !inherited_hidden; }
};

// ---------------------------------------------------------------------------
// RenderLayer — bitmask for camera layer filtering.
// Bit 0 = default scene layer.  A camera renders only entities whose mask
// overlaps the camera's own layer mask.
// ---------------------------------------------------------------------------
struct RenderLayer
{
    uint32_t mask = 1u;
};

// ---------------------------------------------------------------------------
// LocalAabb — model-space axis-aligned bounding box (immutable, set at load time).
// TransformSystem reads this + GlobalTransform → writes WorldAabb each frame.
// ---------------------------------------------------------------------------
struct LocalAabb
{
    DirectX::XMFLOAT3 min = { -0.5f, -0.5f, -0.5f };
    DirectX::XMFLOAT3 max = {  0.5f,  0.5f,  0.5f };
};

// ---------------------------------------------------------------------------
// WorldAabb — world-space axis-aligned bounding box (recomputed each frame from LocalAabb).
// Used by FrustumCullSystem / BVH for per-entity visibility tests.
// ---------------------------------------------------------------------------
struct WorldAabb
{
    DirectX::XMFLOAT3 min = { -0.5f, -0.5f, -0.5f };
    DirectX::XMFLOAT3 max = {  0.5f,  0.5f,  0.5f };
};

// ---------------------------------------------------------------------------
// MeshLibRef — first-class reference to one mesh inside a MeshLibrary
// (.meshlib file). One per mesh entity. Replaces SceneMeshHandle for the new
// scene-loader path: `libHandle` identifies which library, `meshId` is the
// index into that library's MeshLibraryEntry[] table. Scene geometry range,
// AABB, and default material are all fetched by dereferencing the entry at
// draw time — no more `indexStart` arithmetic on the ECS side.
// ---------------------------------------------------------------------------
struct MeshLibRef
{
    Resource::Handle libHandle = {};   // ResourceType::MeshLibrary
    uint32_t         meshId    = 0;    // index into library's entries[]

    // Renderer-owned O(1) cache of the resolved mesh-descriptor slot.
    // Valid only while cachedGeneration == Renderer::m_meshLibDescGeneration;
    // any global invalidation (OnWorldClear) bumps the generation, forcing a
    // re-lookup without needing to walk the ECS and patch every ref.
    mutable uint32_t cachedDescSlot   = 0xFFFFFFFFu;
    mutable uint32_t cachedGeneration = 0;

    bool IsValid() const { return libHandle.IsValid(); }
};

// ---------------------------------------------------------------------------
// SceneNodeTag — marker distinguishing a structural Node Entity (no geometry)
// from a Mesh Entity (has SceneMeshHandle).
// ---------------------------------------------------------------------------
struct SceneNodeTag {};

// ---------------------------------------------------------------------------
// MeshSourcePath — stores the absolute .imsh path that a mesh entity was
// loaded from.  Written by SceneInstanceLoader; read by PrefabSerializer.
// ---------------------------------------------------------------------------
struct MeshSourcePath
{
    std::string path;
};

// Source path for the .iscn scene that created this skinned character.
// Set during SceneInstanceLoader::Load; used by PrefabSerializer to reconstruct.
struct SceneSourcePath
{
    std::string path;  // absolute path to .iscn file
};

// Source path for the bound animation (.ianim).
// Set when EditorLayer binds an animation clip; used by PrefabSerializer.
struct AnimationSourcePath
{
    std::string path;  // absolute path to .ianim file
};

// Source path for the material (.imat) that this entity's MaterialComponent was loaded from.
// When present, prefab/world serialization saves a reference instead of inline data.
struct MaterialSourcePath
{
    std::string path;  // path to .imat file
};

// MaterialOverride — property bag storing per-entity material parameter overrides.
//
// Design follows the ECS Scene Serialization Architecture doc:
//   - Only stores properties that DIFFER from the base .imat
//   - Uses string-keyed map for forward/backward compatibility
//   - Shader adds/removes params → unknown keys silently skipped
//   - Zero overhead on entities without overrides (no component)
//
// Serialization: scene stores key=value pairs inline.
// Runtime: resolve() maps string keys to index-based slots once at load time.
struct MaterialOverride
{
    // Typed variant for material property values
    struct Value
    {
        enum Type : uint8_t { Float, Vec4, Int, Bool, TexRef };
        Type type = Float;
        union {
            float f;
            float v4[4];
            int   i;
            bool  b;
        } data = {};
        std::string texPath; // only if type == TexRef

        static Value MakeFloat(float v)        { Value r; r.type = Float; r.data.f = v; return r; }
        static Value MakeVec4(float x, float y, float z, float w)
        { Value r; r.type = Vec4; r.data.v4[0]=x; r.data.v4[1]=y; r.data.v4[2]=z; r.data.v4[3]=w; return r; }
        static Value MakeInt(int v)            { Value r; r.type = Int; r.data.i = v; return r; }
        static Value MakeBool(bool v)          { Value r; r.type = Bool; r.data.b = v; return r; }
        static Value MakeTex(const std::string& p) { Value r; r.type = TexRef; r.texPath = p; return r; }
    };

    // Persistent property bag: key = parameter name, value = override
    std::unordered_map<std::string, Value> props;

    bool Has(const std::string& key) const { return props.count(key) > 0; }
    void Set(const std::string& key, Value v) { props[key] = std::move(v); }
    void Remove(const std::string& key) { props.erase(key); }
    bool IsEmpty() const { return props.empty(); }
};
