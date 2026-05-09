#include "Resource/DecalMaterialLibrary.h"
#include "Resource/TextureSystem.h"
#include "Graphics/IGraphicsDevice.h"

namespace Resource
{
    std::shared_ptr<DecalMaterialAsset> DecalMaterialLibrary::GetOrCreate(const std::string& name)
    {
        auto it = m_assets.find(name);
        if (it != m_assets.end()) return it->second;

        auto asset  = std::make_shared<DecalMaterialAsset>();
        asset->name = name;
        m_assets.emplace(name, asset);
        return asset;
    }

    std::shared_ptr<DecalMaterialAsset> DecalMaterialLibrary::Find(const std::string& name) const
    {
        auto it = m_assets.find(name);
        return (it == m_assets.end()) ? nullptr : it->second;
    }

    void DecalMaterialLibrary::Destroy(const std::string& name,
                                       TextureSystem& texSys,
                                       IGraphicsDevice& gfx)
    {
        auto it = m_assets.find(name);
        if (it == m_assets.end()) return;

        // Release texture handles on the library's own copy. Any DecalComponent
        // still holding a shared_ptr keeps the asset alive but with no textures
        // — its bindless indices fall to -1 and the decal stops rendering.
        if (it->second)
            it->second->Release(texSys, gfx);
        m_assets.erase(it);
    }

    void DecalMaterialLibrary::Shutdown(TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        for (auto& [_, asset] : m_assets)
            if (asset) asset->Release(texSys, gfx);
        m_assets.clear();
    }
}
