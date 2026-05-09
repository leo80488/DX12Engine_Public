#pragma once

// SkeletonImporter — Assimp-based extractor for skeleton, animation clips,
// and per-mesh blend weights from an aiScene.
//
// Usage pattern (called from SceneLoader when a mesh has bones):
//
//   SkeletonImportResult result;
//   if (SkeletonImporter::Import(scene, result))
//   {
//       uint32_t skelIdx = registry.Register(std::move(result.skeleton));
//       uint32_t firstClipIdx = clipLib.Count();
//       for (auto& clip : result.clips)
//           clipLib.Register(std::move(clip));
//       // result.perMeshBlendData[aiMeshIndex] -> vector<BlendVertex>
//   }

#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

struct aiScene;
struct aiMesh;
struct aiNode;
struct aiAnimation;

namespace Resource
{
    // -----------------------------------------------------------------------
    // SkeletonImportResult — output of a single SkeletonImporter::Import call.
    // -----------------------------------------------------------------------
    struct SkeletonImportResult
    {
        SkeletonAsset             skeleton;
        std::vector<ClipAsset>    clips;           // one per aiAnimation in the scene

        // Per aiMesh index: blend weights packed as BlendVertex[vertexCount].
        // Index matches scene->mMeshes[i].  Empty vector = mesh has no bones.
        std::vector<std::vector<BlendVertex>> perMeshBlendData;

        // Map from aiMesh index to the 0-based skeleton bone index of its first
        // bone influence (used to verify mesh-skeleton association).
        bool hasBones = false;
    };

    // -----------------------------------------------------------------------
    // SkeletonImporter — stateless, all methods are static.
    // -----------------------------------------------------------------------
    class SkeletonImporter
    {
    public:
        // Parse bones and animations from an already-loaded aiScene.
        // Returns false if the scene contains no bones.
        // On success, fills result.skeleton, result.clips, result.perMeshBlendData.
        static bool Import(const aiScene* scene, SkeletonImportResult& result);

        // Build grant (付与) parent table by name-matching heuristics.
        // Detects D-bones ("左足D" → "左足") and EX bones.
        static void BuildGrantTable(SkeletonAsset& skel);

    private:
        // Collect all bone nodes referenced across all meshes and sort them into
        // root-first topological order (parentIndex[i] < i invariant).
        // Returns false if no bones are found.
        static bool CollectBones(const aiScene*                        scene,
                                  SkeletonAsset&                        out,
                                  std::unordered_map<std::string, uint32_t>& nameToSlot);

        // Walk the aiNode hierarchy recursively to find bone nodes and record
        // their parent relationships.
        static void WalkBoneHierarchy(const aiNode*                          node,
                                       int32_t                                parentSlot,
                                       const std::unordered_map<std::string, bool>& boneNodeSet,
                                       SkeletonAsset&                         out,
                                       std::unordered_map<std::string, uint32_t>& nameToSlot);

        // Extract per-mesh BlendVertex arrays (top-4 influences per vertex,
        // weights normalised to [0,255]).
        static void BuildBlendData(const aiScene*                                scene,
                                    const std::unordered_map<std::string, uint32_t>& nameToSlot,
                                    std::vector<std::vector<BlendVertex>>&            outPerMesh);

        // Convert one aiAnimation to a ClipAsset SOA.
        // skeleton is used to initialise un-animated bones to their rest-pose TRS.
        static ClipAsset BuildClip(const aiAnimation*              anim,
                                    const std::unordered_map<std::string, uint32_t>& nameToSlot,
                                    uint32_t                               boneCount,
                                    const SkeletonAsset&                   skeleton);

        // FNV-32 hash for bone names.
        static uint32_t Fnv32(const char* s);
    };
}
