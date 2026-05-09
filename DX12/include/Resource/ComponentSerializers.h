#pragma once

// ComponentSerializers — per-type serialize/deserialize functions for ECS components.
//
// Each serializable component type registers:
//   - TypeTag (string, e.g. "LightData")
//   - Serialize(entity, world, ostream)   → writes component data as key=value text
//   - Deserialize(entity, world, kvMap)   → reads key=value pairs, creates component
//
// Components are classified as:
//   INLINE:   data stored directly in scene/prefab (LightData, Billboard, Camera, etc.)
//   REF:      stores a file path; ResourceSystem loads the actual data (Mesh, Material)
//   DERIVED:  never serialized; reconstructed at runtime (GlobalTransform, Parent, etc.)
//
// Usage:
//   RegisterAllSerializers(registry);
//   for each component on entity:
//     if serializer exists → call Serialize()

#include "ECS/ECS.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <typeindex>

class World;

namespace Resource
{
    class AssetManager;
}

// Key-value pairs parsed from one serialized component block.
using KVMap = std::unordered_map<std::string, std::string>;

// Per-component-type serialization entry.
struct ComponentSerializer
{
    std::string tag;  // e.g. "LightData", "Billboard", "Camera"

    // Returns true if the entity has this component.
    std::function<bool(World&, Entity)> has;

    // Write this component's data for entity 'e' to the stream.
    // Format: "  <tag>: key=val key=val ...\n"
    std::function<void(World&, Entity, std::ostringstream&)> serialize;

    // Read key-value pairs and create/update the component on entity 'e'.
    std::function<void(World&, Entity, const KVMap&, Resource::AssetManager*)> deserialize;
};

// Registry of all serializable component types.
class ComponentSerializerRegistry
{
public:
    void Register(std::type_index typeIdx, ComponentSerializer entry)
    {
        m_byType[typeIdx] = std::move(entry);
        m_byTag[m_byType[typeIdx].tag] = &m_byType[typeIdx];
    }

    const std::unordered_map<std::type_index, ComponentSerializer>& All() const { return m_byType; }

    const ComponentSerializer* FindByTag(const std::string& tag) const
    {
        auto it = m_byTag.find(tag);
        return (it != m_byTag.end()) ? it->second : nullptr;
    }

private:
    std::unordered_map<std::type_index, ComponentSerializer> m_byType;
    std::unordered_map<std::string, ComponentSerializer*>    m_byTag;
};

// Register all engine component serializers.
void RegisterAllComponentSerializers(ComponentSerializerRegistry& reg);

// Get the global singleton registry (lazy-initialized on first call).
ComponentSerializerRegistry& GetComponentRegistry();
