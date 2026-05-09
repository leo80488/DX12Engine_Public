#pragma once

// DecalMaterialLibrary — name-keyed pool of DecalMaterialAsset instances.
//
// Why a library (instead of scattering asset ownership across DecalComponents):
//   - Many decal instances (1000s of blood splats) share one material. The
//     library enforces that "same name → same asset" without leaking memory.
//   - Texture handles are Acquired once per asset, not once per DecalComponent.
//     Releasing the asset (at scene teardown) Releases textures exactly once.
//   - Editor UI iterates GetAll() to present "all decal materials" regardless
//     of which scene entity currently references them.
//
// Threading: main-thread only. Assets are shared_ptr so callers can keep
// weak references; the Library retains strong ownership.

#include "Resource/DecalMaterialAsset.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;

namespace Resource
{
    class TextureSystem;

    class DecalMaterialLibrary
    {
    public:
        DecalMaterialLibrary()  = default;
        ~DecalMaterialLibrary() = default;

        DecalMaterialLibrary(const DecalMaterialLibrary&)            = delete;
        DecalMaterialLibrary& operator=(const DecalMaterialLibrary&) = delete;

        // Returns the asset for @p name — creates a new empty asset if absent.
        // The asset's texture paths are left unset; callers populate them and
        // then call asset->Resolve() (the Renderer does this implicitly each
        // frame for referenced assets).
        std::shared_ptr<DecalMaterialAsset> GetOrCreate(const std::string& name);

        // Returns the asset for @p name, or nullptr if absent. Non-mutating.
        std::shared_ptr<DecalMaterialAsset> Find(const std::string& name) const;

        // Drop an asset by name; releases its texture handles. Pre-existing
        // shared_ptr copies held by DecalComponents keep the asset alive
        // until the last copy drops (standard shared_ptr semantics).
        void Destroy(const std::string& name, TextureSystem& texSys, IGraphicsDevice& gfx);

        // Release every asset's texture handles and clear the pool. Call
        // before device shutdown or on full scene teardown so refcounts
        // balance against the TextureSystem.
        void Shutdown(TextureSystem& texSys, IGraphicsDevice& gfx);

        // Editor hook — returns every asset for UI enumeration.
        const std::unordered_map<std::string, std::shared_ptr<DecalMaterialAsset>>&
            GetAll() const { return m_assets; }

    private:
        std::unordered_map<std::string, std::shared_ptr<DecalMaterialAsset>> m_assets;
    };
}
