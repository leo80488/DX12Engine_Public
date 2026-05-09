#pragma once

// VrmImporter — VRM (0.x and 1.0) model importer.
//
// VRM is a GLB (binary glTF 2.0) container with a JSON `extensions` block
// describing humanoid bone mapping, blend-shape expression presets, meta
// information, and (optional) spring-bone/first-person/MToon material data.
//
// This importer takes a hybrid approach:
//   1. The mesh / skeleton / materials / animations are produced by reusing
//      SceneImporter — Assimp already parses glTF/GLB cleanly. The output
//      file set (`.meshlib`, `.iscn`, `.iskel`, `.imat`) is identical to a
//      regular GLB import, so SceneInstanceLoader picks it up unchanged.
//   2. The VRM-specific extension JSON is parsed separately and emitted as
//      an additional `<stem>/<stem>.ivrm` text file containing:
//        - meta block:        title / author / version / license
//        - humanoid bones:    {humanoid_name → glTF node index → node name}
//        - expressions:       preset name → list of (mesh, morphIndex, weight)
//      so downstream systems (humanoid retargeting, expression playback) can
//      consume it without re-parsing the source.
//
// Out of scope (intentionally — each deserves its own subsystem):
//   - SpringBone secondary motion physics
//   - MToon material / outline shader
//   - First-person rendering masks

#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    // -----------------------------------------------------------------------
    // Sub-records of VrmImportResult
    // -----------------------------------------------------------------------
    struct VrmMeta
    {
        std::string title;
        std::string version;
        std::string author;
        std::string contactInformation;
        std::string reference;
        std::string licenseName;       // VRM 1.0 free-form license URL/name
        std::string allowedUserName;   // VRM 0.x consent fields (may be empty for 1.0)
        std::string commercialUseName;
        std::string licenseTextUrl;    // VRM 1.0 licenseUrl / VRM 0.x otherLicenseUrl
        std::string thumbnailNodeName; // resolved glTF node name for the thumbnail (may be empty)
    };

    struct VrmHumanoidBone
    {
        std::string humanoidName;  // canonical VRM name: "hips", "spine", "leftUpperArm", ...
        int32_t     nodeIndex = -1; // glTF node index (raw)
        std::string nodeName;       // glTF node name resolved from nodeIndex
    };

    struct VrmExpressionBinding
    {
        // Identifies a single morph-target on a mesh:
        //   - meshIndex: glTF mesh index (NOT the merged .meshlib entry index)
        //   - morphIndex: index into that mesh's morphTargets array
        //   - weight: 0..1 multiplier applied when the expression is at full strength
        int32_t meshIndex  = -1;
        int32_t morphIndex = -1;
        float   weight     = 1.f;
    };

    struct VrmExpression
    {
        std::string name;                              // preset name: "happy", "blink", ...
        bool        isPreset = true;                   // false → custom expression (VRM 1.0 'custom')
        bool        isBinary = false;                  // true → snap to 0/1 instead of interpolating
        std::vector<VrmExpressionBinding> morphBindings;
    };

    // -----------------------------------------------------------------------
    // VrmImportResult — output of VrmImporter::Import().
    // -----------------------------------------------------------------------
    struct VrmImportResult
    {
        // Detected spec version: 0 → VRM 0.x ("VRM" extension), 1 → VRM 1.0 ("VRMC_vrm")
        int specVersion = -1;

        VrmMeta                       meta;
        std::vector<VrmHumanoidBone>  humanoidBones;
        std::vector<VrmExpression>    expressions;

        bool success = false;
    };

    // -----------------------------------------------------------------------
    // ExportedFile — same layout used by SceneImporter / PmxImporter.
    // -----------------------------------------------------------------------
    struct VrmExportedFile
    {
        std::string          relativePath;
        std::vector<uint8_t> blob;
    };

    class VrmImporter
    {
    public:
        // Parse VRM extension data from the GLB JSON chunk.
        // Does NOT decode geometry — that work is delegated to SceneImporter.
        static VrmImportResult Import(const uint8_t* data, size_t size);

        // Build the full output file set: forwards to SceneImporter for
        // .meshlib / .iscn / .iskel / .imat, then appends a `.ivrm` text
        // file capturing humanoid + expression + meta data.
        // `sourcePath` is used so SceneImporter can derive the file stem and
        // extension hint; `sourceData` is the raw GLB bytes.
        static std::vector<VrmExportedFile> BuildBlobs(const std::string&          sourcePath,
                                                       const std::vector<uint8_t>& sourceData,
                                                       const VrmImportResult&      vrm);

        static std::vector<const char*> GetSourceExtensions() { return { ".vrm" }; }
    };
}
