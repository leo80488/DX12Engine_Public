#pragma once

// ECS components for the Behavior Tree AI system.
//
//   AIComponent          — entity opts in to BT execution. Holds the shared
//                          tree handle, the per-entity instance state, and
//                          tick frequency control.
//   BlackboardComponent  — string-keyed working memory shared between BT
//                          nodes and other systems (Perception, Movement,
//                          Damage). Held as its own component so non-AI
//                          systems can read/write without touching BT
//                          internals.
//
// Tree assets are SHARED across many entities (same model as SkeletonAsset,
// MeshLibRef): one parsed BT, many BTInstances. Per-entity state — running
// node, cooldown timers, repeater counts — lives in BTInstance, never on
// BTNode. See "Common Pitfalls" §9 in BT_AI_System_Architecture.md.

#include <DirectXMath.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ECS/ECS.h"
#include "AI/BTNode.h"   // BTInstance complete type — required for unique_ptr<BTInstance>

namespace AI
{
    class BTAsset;

    // ---- Blackboard value types --------------------------------------------
    // Variant covers every type a BT node, gameplay system, or Lua action
    // is reasonably going to want to write into the blackboard. Vector3 is
    // XMFLOAT3 to match the rest of the engine. Entity is the raw uint32 —
    // callers store EntityHandle values themselves (as a separate key) when
    // they need lifetime safety.
    using BBValue = std::variant<
        bool,
        int,
        float,
        std::string,
        Entity,
        DirectX::XMFLOAT3,
        // Array variant — BT params like PatrolPoints' waypoint list need a
        // nested structure that doesn't collapse into a single XMFLOAT3.
        // Round-trip back to Lua as `{ {x,y,z}, {x,y,z}, ... }`.
        std::vector<DirectX::XMFLOAT3>,
        // String-list variant — used by params like SetRandomState's
        // `candidates = { "WALK_1", "WALK_2" }`. Without this entry the
        // Lua→BBValue parser falls into the XMFLOAT3 branch and silently
        // collapses the list to a zero vector.
        std::vector<std::string>>;
}

struct BlackboardComponent
{
    std::unordered_map<std::string, AI::BBValue> values;

    // Typed setters / getters. The getter returns nullptr when the key is
    // missing or stored under a different type — let the caller decide
    // whether that's a soft failure or an error.
    template <typename T>
    void Set(const std::string& key, T value)
    {
        values[key] = std::move(value);
    }

    template <typename T>
    const T* Get(const std::string& key) const
    {
        auto it = values.find(key);
        if (it == values.end()) return nullptr;
        return std::get_if<T>(&it->second);
    }

    template <typename T>
    T* GetMut(const std::string& key)
    {
        auto it = values.find(key);
        if (it == values.end()) return nullptr;
        return std::get_if<T>(&it->second);
    }

    bool Has(const std::string& key) const
    {
        return values.find(key) != values.end();
    }

    void Erase(const std::string& key) { values.erase(key); }
};

struct AIComponent
{
    // Shared tree resource. Many AIComponents pointing at the same asset
    // share the same parsed node graph — they only differ by their per-
    // entity BTInstance.
    std::shared_ptr<AI::BTAsset> tree;

    // Per-entity execution state (running node id, cooldown timers,
    // repeater counts, debug trace). unique_ptr so AIComponent can be
    // moved cheaply but the instance data is heap-stable across vector
    // resizes inside ComponentPool.
    std::unique_ptr<AI::BTInstance> instance;

    // Seconds between ticks. 0 = every frame. AILODSystem rewrites this
    // field per-frame based on distance / visibility tier.
    float tickInterval     = 0.1f;
    float timeSinceLastTick = 0.f;

    bool  enabled = true;

    // Optional source path — kept for hot reload, debug labels, and the
    // "show which BT this entity uses" inspector row. Empty when the tree
    // was set programmatically rather than loaded from disk.
    std::string treePath;
};
