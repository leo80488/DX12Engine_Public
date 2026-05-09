#pragma once

#include "Resource/Resource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/IPendingGPUUpload.h"
#include <memory>
#include <string>
#include <vector>

// Loader plugin interface: register by file extension; manager dispatches to the matching loader.
namespace Resource
{
    struct LoadResult
    {
        std::unique_ptr<Resource>          resource;      // ownership transferred to ResourceManager
        std::unique_ptr<IPendingGPUUpload> pendingUpload; // optional deferred GPU upload
    };

    class IResourceLoader
    {
    public:
        virtual ~IResourceLoader() = default;

        /** Parse from already-loaded memory and create a Resource; called on a background thread. Optionally return pendingUpload for main-thread GPU upload. */
        virtual LoadResult Load(
            const std::string& path,
            const std::vector<uint8_t>& data) = 0;

        /** File extension handled (lowercase, with dot), e.g. ".dds", ".wav" */
        virtual const char* GetExtension() const = 0;
    };
}

