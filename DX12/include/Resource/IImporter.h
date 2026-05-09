#pragma once

#include <string>
#include <vector>

// Importer plugin interface: one-time CPU-side conversion from an external
// source format to the engine's internal binary format.
//
// Responsibility split:
//   IImporter  — reads raw source bytes, produces an internal blob
//                (AssetHeader + Metadata + Payload).  No GPU calls.
//   IResourceLoader — reads an internal blob and creates a Resource object.
//                     May enqueue a GPU upload via IPendingGPUUpload.
//
// ResourceManager drives the pipeline:
//   1. Detects external extension → finds IImporter.
//   2. Derives cache path (same dir, internal extension).
//   3. If cache exists on disk: loads cache directly (skip importer).
//   4. Otherwise: reads source file → Import() → writes cache → loads cache.
namespace Resource
{
    class IImporter
    {
    public:
        virtual ~IImporter() = default;

        // Convert raw source bytes to an internal-format blob.
        // Returns an empty vector on failure; ResourceManager will mark the entry as Failed.
        virtual std::vector<uint8_t> Import(const std::string& sourcePath,
                                             const std::vector<uint8_t>& sourceData) = 0;

        // Source extensions this importer handles (lowercase with dot).
        // Multiple extensions may share one importer instance (e.g. ".jpg", ".jpeg").
        virtual std::vector<const char*> GetSourceExtensions() const = 0;

        // The single internal extension this importer produces (".itex", ".ishdr", etc.).
        virtual const char* GetInternalExtension() const = 0;
    };
}
