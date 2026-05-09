#pragma once

// AssetManager — thin facade bundling Mesh/Texture/Resource systems.
//
// Textures are delegated to TextureSystem (path-based dedup + async load).
// Meshes have moved to MeshLibrary (see Resource/MeshLibrary.h): the legacy
// per-.imsh cache + AcquireMeshBatch fast path were retired in the P1-P6
// rewrite. The AssetManager kept for:
//   - one-stop init/shutdown of the underlying systems
//   - texture-side delegation
//   - raw-system accessors so Renderer can pull MeshSystem/TextureSystem
//     from a single handle
//
// Usage:
//   assetMgr.Init(meshSys, texSys, rm, gfx);     // once at startup
//   TextureHandle th = assetMgr.AcquireTexture("stone.itex");
//   assetMgr.Tick();                              // once per frame
//   assetMgr.Shutdown();                          // before device shutdown

#include "Resource/MeshSystem.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/SystemHandles.h"
#include "Graphics/GraphicsStruct.h"

#include <string>

class IGraphicsDevice;

namespace Resource
{
    class AssetManager
    {
    public:
        AssetManager()  = default;
        ~AssetManager() = default;

        AssetManager(const AssetManager&)            = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        // Wire up the underlying systems. None are owned by AssetManager.
        void Init(MeshSystem&      meshSys,
                  TextureSystem&   texSys,
                  ResourceManager& rm,
                  IGraphicsDevice& gfx);

        // ---- Texture -----------------------------------------------------

        // Delegates to TextureSystem::Acquire (already path-deduplicates).
        TextureHandle       AcquireTexture(const std::string& path);
        void                ReleaseTexture(TextureHandle h);
        bool                IsTextureReady(TextureHandle h) const;
        const RHI::Texture* GetTexture(TextureHandle h)     const;

        // ---- Per-frame ---------------------------------------------------

        // Forward to TextureSystem::Tick (GPU promotion + deferred destroy).
        // Call once per frame, after BeginFrame.
        void Tick();

        // Release any per-AssetManager cached state. Call before TextureSystem
        // and MeshSystem shutdown.
        void Shutdown();

        // ---- Raw system access (for Renderer injection) ------------------
        MeshSystem*      GetMeshSystem()      const { return m_meshSys; }
        TextureSystem*   GetTextureSystem()   const { return m_texSys;  }
        ResourceManager* GetResourceManager() const { return m_rm;      }
        IGraphicsDevice* GetGraphicsDevice()  const { return m_gfx;     }

    private:
        MeshSystem*      m_meshSys = nullptr;
        TextureSystem*   m_texSys  = nullptr;
        ResourceManager* m_rm      = nullptr;
        IGraphicsDevice* m_gfx     = nullptr;
    };
}
