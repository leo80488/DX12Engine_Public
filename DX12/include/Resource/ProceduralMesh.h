#pragma once
// ProceduralMesh — CPU-side procedural mesh generation utilities.
// Returns plain float arrays so the caller decides how to upload to GPU.

#include <vector>
#include <array>
#include <cstdint>

namespace ProceduralMesh
{

// All vertex data in separate parallel arrays (matches PVF ByteAddressBuffer layout).
struct MeshData
{
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 4>> tangents;  // xyz = tangent, w = bitangent handedness sign (+1 or -1)
    std::vector<std::array<float, 2>> uvs;        // texture coordinates (uv0 slot)
    std::vector<std::array<float, 3>> colors;     // per-vertex color (uv1 slot)
    std::vector<uint16_t>             indices;
};

// Unit cube centred at origin. Each face has 4 unique vertices so normals are correct.
MeshData Cube(float halfSize = 0.5f,
              std::array<float, 3> color = { 0.80f, 0.80f, 0.80f });

// UV sphere centred at origin.
// rings   = latitude bands  (default 12)
// slices  = longitude bands (default 24)
MeshData Sphere(float radius = 0.5f,
                uint32_t rings  = 32,
                uint32_t slices = 48,
                std::array<float, 3> color = { 0.40f, 0.65f, 0.90f });

// Cone with apex at +Y, base disc at -Y.
// slices = number of base vertices (default 24)
MeshData Cone(float radius = 0.5f,
              float height = 1.0f,
              uint32_t slices = 24,
              std::array<float, 3> color = { 0.90f, 0.55f, 0.15f });

} // namespace ProceduralMesh

// Primitive mesh type index — matches the pre-built GPU mesh slots.
// Shared between MeshSpawner (entity creation) and Renderer (GPU indexing).
enum class PrimitiveMeshType : int
{
    Cube   = 0,
    Sphere = 1,
    Cone   = 2,
    Count  = 3
};
