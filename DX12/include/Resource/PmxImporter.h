#pragma once

// PmxImporter — custom binary parser for PMX 2.0/2.1 model files.
// Replaces Assimp for PMX, giving direct access to grant (付与) and IK data.
//
// Coordinate convention: identity (no axis negation).
// MMD and DX12 are both left-handed Y-up. Z direction (toward-viewer vs
// into-screen) is a camera convention, not a model-space difference.
//   Position/Normal: (x,y,z) → (x,y,z)   unchanged
//   Quaternion:      (qx,qy,qz,qw) → (qx,qy,qz,qw)  unchanged
//   Triangle winding: unchanged

#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{
    // -----------------------------------------------------------------------
    // PmxMaterial — parsed material properties (one per PMX material entry).
    // -----------------------------------------------------------------------
    struct PmxMaterial
    {
        std::string        nameJP;                   // material name (Japanese)
        std::string        nameEN;                   // material name (English, may be empty)
        DirectX::XMFLOAT4 diffuse   = { 1,1,1,1 };
        DirectX::XMFLOAT3 specular  = { 0,0,0 };
        float              shininess = 0.f;
        DirectX::XMFLOAT3 ambient   = { 0,0,0 };
        int32_t            textureIndex = -1;      // index into textureFiles[]
        int32_t            sphereTextureIndex = -1;
        uint8_t            sphereMode = 0;         // 0=disabled, 1=mul, 2=add, 3=sub
        int32_t            toonTextureIndex = -1;
        uint32_t           indexStart = 0;
        uint32_t           indexCount = 0;
    };

    // -----------------------------------------------------------------------
    // PmxImportResult — output of PmxImporter::Import().
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // PmxMorphTarget — vertex morph (position delta per vertex).
    // -----------------------------------------------------------------------
    struct PmxMorphTarget
    {
        char                            name[64]{};
        // Sparse: only modified vertices have deltas.
        std::vector<uint32_t>           vertexIndices;  // global PMX vertex indices
        std::vector<DirectX::XMFLOAT3>  positionDeltas; // corresponding position offsets
    };

    struct PmxImportResult
    {
        SkeletonAsset skeleton;

        // Mesh data (all submeshes concatenated; split by materialRanges)
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        std::vector<uint32_t>           indices;
        std::vector<BlendVertex>        blendData;

        // Per-material face ranges and properties
        std::vector<PmxMaterial>        materials;

        // Texture filenames referenced by materials
        std::vector<std::string>        textureFiles;

        // Vertex morphs (blend shapes / facial expressions)
        std::vector<PmxMorphTarget>     morphTargets;

        bool success = false;
    };

    // -----------------------------------------------------------------------
    // PmxImporter — stateless, all methods are static.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // ExportedFile — same layout used by SceneImporter for compatibility.
    // -----------------------------------------------------------------------
    struct PmxExportedFile
    {
        std::string           relativePath;
        std::vector<uint8_t>  blob;
    };

    class PmxImporter
    {
    public:
        // Parse a PMX file from an in-memory buffer.
        static PmxImportResult Import(const uint8_t* data, size_t size);

        // Convenience: load from file path.
        static PmxImportResult ImportFromFile(const std::string& path);

        // Build .imsh / .iskel / .iscn blobs from a successful PmxImportResult.
        // `stem` is used for naming (e.g. "ModelName" → "ModelName/ModelName.iscn").
        // Returns a list of relative-path blobs ready to be written to disk.
        static std::vector<PmxExportedFile> BuildBlobs(const PmxImportResult& pmx,
                                                        const std::string& stem);
    };
}
