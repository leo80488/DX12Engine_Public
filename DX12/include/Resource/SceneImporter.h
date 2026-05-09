#pragma once

// SceneImporter — offline cook for multi-mesh 3D model files.
//
// Takes a source model file (glTF / FBX / OBJ / ...) and produces a set of
// internal-format blobs ready to write to disk:
//
//   <stem>_<NodeName>_m<idx>.imsh  — one per mesh, identical format to
//                                    MeshImporter output (no dependency on it)
//   <stem>.iscn                    — scene hierarchy descriptor
//
// .iscn text payload format (one record per line):
//   # comment
//   S nodes=<N> meshfiles=<M> source=<original_filename>
//   N idx=<i> parent=<j|-1> tx=<f> ty=<f> tz=<f> qx=<f> qy=<f> qz=<f> qw=<f> sx=<f> sy=<f> sz=<f> mc=<k> [mr0=<fi> mr1=<fi> ...] name=<sanitized>
//   F idx=<i> file=<relativePath>
//
// Fields:
//   S  — one-per-file header   N  — node record (one per aiNode in the scene graph)
//   F  — mesh file reference   name — spaces replaced with '_', no embedded spaces
//   mr0..mrN — indices into F records (which .imsh files this node references)
//
// SceneLoader reads .iscn to reconstruct the ECS hierarchy without Assimp.
// Individual .imsh files can be used standalone via MeshSystem::Acquire().

#include <string>
#include <vector>

namespace Resource
{
    class SceneImporter
    {
    public:
        struct ExportedFile
        {
            std::string          relativePath;  // filename (no directory), e.g. "char_Body_m0.imsh"
            std::vector<uint8_t> blob;
        };

        struct ImportResult
        {
            std::vector<ExportedFile> files;    // mesh .imsh files first, then .iscn last
            bool                      success = false;
        };

        // Import a model from raw source bytes.
        // sourcePath : full filesystem path (used for extension hint + output naming)
        // sourceData : raw file bytes
        // Returns all output blobs; caller writes them to the asset directory.
        static ImportResult Import(const std::string&          sourcePath,
                                   const std::vector<uint8_t>& sourceData);

        // Source extensions this importer handles.
        static std::vector<const char*> GetSourceExtensions()
        {
            return { ".obj", ".gltf", ".glb", ".fbx", ".dae", ".3ds", ".ply", ".stl", ".vrm" };
        }
    };
}
