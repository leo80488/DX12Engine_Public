#include "Resource/DecalMaterialAsset.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Graphics/IGraphicsDevice.h"

namespace Resource
{
    namespace
    {
        bool PromoteSlot(TextureHandle        handle,
                         int32_t&             bindless,
                         const TextureSystem& texSys)
        {
            if (handle == kInvalidTextureHandle) return true;   // empty slots always "resolved"
            if (bindless >= 0)                  return true;   // already promoted

            if (!texSys.IsReady(handle)) return false;
            if (const RHI::Texture* tex = texSys.GetTexture(handle))
            {
                bindless = static_cast<int32_t>(tex->handle_id);
                return true;
            }
            return false;
        }
    }

    // ---------------------------------------------------------------------------
    void DecalMaterialAsset::Resolve(TextureSystem&   texSys,
                                     ResourceManager& resMgr,
                                     IGraphicsDevice& gfx)
    {
        // Acquire-when-missing / Release-when-cleared per slot. Editor path
        // changes go through SetTexturePath which Releases the previous handle
        // first, so this path is purely "ensure handle matches path".
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
        {
            const std::string& path = texPaths[i];
            const bool wantTexture = !path.empty();
            const bool haveTexture = (texHandles[i] != kInvalidTextureHandle);

            if (wantTexture && !haveTexture)
            {
                texHandles[i]  = texSys.Acquire(path, resMgr, gfx);
                texBindless[i] = -1;
            }
            else if (!wantTexture && haveTexture)
            {
                texSys.Release(texHandles[i], gfx);
                texHandles[i]  = kInvalidTextureHandle;
                texBindless[i] = -1;
            }
        }
    }

    // ---------------------------------------------------------------------------
    bool DecalMaterialAsset::TryPromote(TextureSystem& texSys) const
    {
        bool allReady = true;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            allReady &= PromoteSlot(texHandles[i], texBindless[i], texSys);
        return allReady;
    }

    // ---------------------------------------------------------------------------
    void DecalMaterialAsset::Release(TextureSystem& texSys, IGraphicsDevice& gfx)
    {
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (texHandles[i] != kInvalidTextureHandle)
            {
                texSys.Release(texHandles[i], gfx);
                texHandles[i] = kInvalidTextureHandle;
            }
            texBindless[i] = -1;
        }
    }

    // ---------------------------------------------------------------------------
    bool DecalMaterialAsset::IsFullyResolved() const
    {
        for (uint32_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (!texPaths[i].empty() && texBindless[i] < 0)
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    void DecalMaterialAsset::SetTexturePath(TextureSlot       slot,
                                            const std::string& newPath,
                                            TextureSystem&    texSys,
                                            IGraphicsDevice&  gfx)
    {
        if (slot >= SLOT_COUNT) return;
        if (texPaths[slot] == newPath) return;

        if (texHandles[slot] != kInvalidTextureHandle)
        {
            texSys.Release(texHandles[slot], gfx);
            texHandles[slot] = kInvalidTextureHandle;
        }
        texPaths[slot]    = newPath;
        texBindless[slot] = -1;
        // Next Resolve() call will Acquire for the new path (if non-empty).
    }

    // ---------------------------------------------------------------------------
    const char* DecalMaterialAsset::SlotLabel(TextureSlot slot)
    {
        switch (slot)
        {
        case SLOT_BASECOLOR:    return "Base Color";
        case SLOT_NORMAL:       return "Normal";
        case SLOT_OPACITY:      return "Opacity";
        case SLOT_ROUGHNESS:    return "Roughness";
        case SLOT_SPECULAR:     return "Specular";
        case SLOT_AO:           return "AO";
        case SLOT_BUMP:         return "Bump";
        case SLOT_CAVITY:       return "Cavity";
        case SLOT_DISPLACEMENT: return "Displacement";
        default:                return "?";
        }
    }

    const char* DecalMaterialAsset::SlotKey(TextureSlot slot)
    {
        switch (slot)
        {
        case SLOT_BASECOLOR:    return "baseColor";
        case SLOT_NORMAL:       return "normal";
        case SLOT_OPACITY:      return "opacity";
        case SLOT_ROUGHNESS:    return "roughness";
        case SLOT_SPECULAR:     return "specular";
        case SLOT_AO:           return "ao";
        case SLOT_BUMP:         return "bump";
        case SLOT_CAVITY:       return "cavity";
        case SLOT_DISPLACEMENT: return "displacement";
        default:                return "unknown";
        }
    }
}
