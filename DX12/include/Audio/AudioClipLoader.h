#pragma once

// AudioClipLoader — turns an .aclip blob into an AudioClipResource on a
// background worker thread. CPU-only (no GPU upload step), so LoadResult
// returns no IPendingGPUUpload. Symmetric with AnimationLoader.

#include "Resource/IResourceLoader.h"

namespace Audio
{
    class AudioClipLoader : public Resource::IResourceLoader
    {
    public:
        Resource::LoadResult Load(const std::string& path,
                                  const std::vector<uint8_t>& data) override;

        const char* GetExtension() const override { return ".aclip"; }
    };
}
