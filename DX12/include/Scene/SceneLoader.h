#pragma once

// SceneLoader — thin namespace that survives the P1-P6 rewrite only because
// it still owns the `SkinnedMeshPending` struct shared between
// SceneInstanceLoader (skinned asset deserialisation) and Renderer
// (Renderer::RegisterSkinnedMeshFull). The runtime Assimp-based Load() path
// is gone — scenes are imported offline via SceneImporter.

#include "ECS/ECS.h"
#include "Resource/SkeletonAsset.h"  // BlendVertex, kInvalidSkeletonIndex

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

class SceneLoader
{
public:
    // Per-skinned-mesh CPU data, ready to be passed to
    // Renderer::RegisterSkinnedMeshFull(). Populated by SceneInstanceLoader
    // after deserialising a .iskel blob.
    struct SkinnedMeshPending
    {
        Entity   entity         = NullEntity;
        Entity   rootEntity     = NullEntity;
        uint32_t skeletonIndex  = kInvalidSkeletonIndex;

        std::vector<DirectX::XMFLOAT3> restPositions;
        std::vector<DirectX::XMFLOAT3> restNormals;
        std::vector<DirectX::XMFLOAT2> uvs;
        std::vector<DirectX::XMFLOAT4> tangents;     // optional
        std::vector<uint32_t>           indices;
        std::vector<BlendVertex>        blendData;

        // Morph targets.
        std::vector<DirectX::XMFLOAT3> morphDeltas; // dense: morphCount * vertexCount
        uint32_t                        morphCount = 0;
        std::vector<std::string>        morphNames;
    };
};
