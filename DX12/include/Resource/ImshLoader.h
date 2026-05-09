#pragma once

// ImshLoader — loads a .imsh binary mesh blob from disk into a MeshComponent.
// Shared by AssetManager, PrefabSerializer, and SceneInstanceLoader.
// Returns false if the file is missing or has an invalid header.

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
struct MeshComponent;

namespace Resource
{
    // Legacy path: reads the file, de-interleaves into MeshComponent's three
    // separate arrays (positions/normals/uvs). Used by prefab / small meshes.
    bool LoadImsh(const std::string& path, MeshComponent& out);

    // Bulk-load view over a raw .imsh blob (either mmapped out of an
    // ImshPack archive or freshly read from a single file). Holds pointers
    // straight into the source bytes — no de-interleave copy. Callers must
    // ensure the source data outlives the view.
    //
    // Layout of a .imsh payload (see ImshLoader.cpp::PackedVertex):
    //   [ vertexCount × PackedVertex(32 B) ][ indexCount × uint32 ]
    struct ImshSubMesh
    {
        uint32_t indexStart;      // first index into the shared IB
        uint32_t indexCount;      // length of this submesh's draw range
        uint32_t materialIndex;   // scene-F-line index of this submesh's material
        // Per-submesh world-pose AABB, computed from the index range that this
        // submesh draws. Without this every submesh entity inherits the merged
        // mesh's full-scene AABB, which collapses BVH leaves on top of each
        // other and breaks frustum culling.
        DirectX::XMFLOAT3 aabbMin = {  0,  0,  0 };
        DirectX::XMFLOAT3 aabbMax = {  0,  0,  0 };
    };

    struct ImshView
    {
        const uint8_t*    vertexBlob   = nullptr;   // vertexCount × 32 B
        const uint32_t*   indexBlob    = nullptr;   // indexCount × uint32
        uint32_t          vertexCount  = 0;
        uint32_t          indexCount   = 0;
        uint32_t          vertexStride = 32;        // 32 B: float3 pos + float3 nrm + float2 uv
        DirectX::XMFLOAT3 aabbMin      = {  0,  0,  0 };
        DirectX::XMFLOAT3 aabbMax      = {  0,  0,  0 };

        // Submesh table — populated from the .imsh tail if present.
        // When the source is a legacy single-submesh .imsh (subMeshCount == 0
        // in the metadata), MakeImshView synthesises one entry spanning the
        // full index range so downstream code always sees at least one.
        std::vector<ImshSubMesh> subMeshes;
    };

    // Validate blob + populate the view. `sourceBytes`/`sourceSize` must cover
    // the entire .imsh file (header + metadata + payload). AABB is computed
    // on the caller's thread — cheap enough to parallelise.
    bool MakeImshView(const uint8_t* sourceBytes, size_t sourceSize,
                      ImshView& out);
}
