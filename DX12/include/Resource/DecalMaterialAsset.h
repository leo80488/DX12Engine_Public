#pragma once

// DecalMaterialAsset — shared-template material for the clustered decal
// system. Unreal-style channel layout:
//
//   BaseColor   — albedo / diffuse.  Goes into GBuffer.Albedo (R11G11B10F).
//   Normal      — tangent-space normal map.  Blends into GBuffer.Normal via
//                 Reoriented Normal Mapping (RNM).
//   Opacity     — grayscale mask (.r).  Multiplies the overall decal alpha.
//                 When absent, BaseColor's alpha channel is used as fallback.
//   Roughness   — grayscale (.r) → GBuffer.Surface.r.
//   Specular    — grayscale (.r) dielectric reflectance → GBuffer.Surface.a.
//   AO          — grayscale (.r) ambient occlusion → GBuffer.Surface.b.
//   Bump        — grayscale (.r) small-scale height.  Derivative → normal
//                 perturbation, then RNM-combined with Normal.
//   Cavity      — grayscale (.r) local occlusion darkening the BaseColor.
//                 sampled value 1 → no change; 0 → fully dark.
//   Displacement — grayscale (.r) height offset used as parallax UV shift
//                 before all other samples.  scalar displacementScale
//                 controls magnitude (0 disables).
//
// Every slot has a matching scalar multiplier exposed in the editor so users
// can tune a single texture without re-exporting (e.g. roughnessScalar = 0.8
// multiplies the sampled roughness).  Scalars also act as raw constants when
// the texture slot is empty — e.g. no roughness texture + roughness=0.2 → the
// decal writes a flat 0.2 roughness anywhere it touches.
//
// Ownership model (unchanged from previous version): DecalMaterialLibrary
// holds shared_ptr<DecalMaterialAsset>; DecalComponent also holds
// shared_ptr.  Textures are acquired per-asset (not per-component) so many
// DecalComponent instances sharing one asset cost one set of texture refs.

#include "Resource/SystemHandles.h"
#include "Resource/MaterialDomain.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>

class IGraphicsDevice;

namespace Resource
{
    class TextureSystem;
    class ResourceManager;

    class DecalMaterialAsset
    {
    public:
        // ---- Texture slots (Unreal-aligned order) ----------------------------
        enum TextureSlot : uint32_t
        {
            SLOT_BASECOLOR    = 0,
            SLOT_NORMAL       = 1,
            SLOT_OPACITY      = 2,
            SLOT_ROUGHNESS    = 3,
            SLOT_SPECULAR     = 4,
            SLOT_AO           = 5,
            SLOT_BUMP         = 6,
            SLOT_CAVITY       = 7,
            SLOT_DISPLACEMENT = 8,
            SLOT_COUNT        = 9,
        };

        // ---- Write-channel flags (which GBuffer slots this decal overwrites) -
        // Minimum-viable decal = BaseColor only. Users opt into other channels
        // by enabling the matching WRITE_* flag in the inspector. This avoids
        // the "I only assigned BaseColor but roughness got clobbered with the
        // scalar default 0.5" trap: a channel only writes when its flag is on,
        // and its flag is only on when the user explicitly asked for it.
        enum FLAGS : uint32_t
        {
            WRITE_BASECOLOR = 1 << 0,
            WRITE_NORMAL    = 1 << 1,
            WRITE_ROUGHNESS = 1 << 2,
            WRITE_SPECULAR  = 1 << 3,
            WRITE_AO        = 1 << 4,
            DEFAULT         = WRITE_BASECOLOR,
            // Convenience bundle — every PBR channel enabled. Useful when a
            // material is authored with the full 9-slot set and you want all
            // of them writing from day one (e.g. via code-created material).
            ALL_PBR         = WRITE_BASECOLOR | WRITE_NORMAL | WRITE_ROUGHNESS
                            | WRITE_SPECULAR  | WRITE_AO,
        };

        enum class NormalBlendMode : uint32_t
        {
            RNM  = 0,   // Reoriented Normal Mapping (default; recommended)
            Lerp = 1,   // Plain linear blend (flat on sloped surfaces)
        };

        // ---- Identity (serialization-facing) ---------------------------------
        std::string name;

        // ---- Editor parameters ------------------------------------------------
        std::string texPaths[SLOT_COUNT];

        // Base colour tint — multiplies the sampled BaseColor RGB. `.w` is an
        // extra opacity multiplier (historical; combined with opacity scalar +
        // opacity texture).
        DirectX::XMFLOAT4 baseColorTint = { 1.f, 1.f, 1.f, 1.f };

        // Scalars — each multiplies the matching texture when present; used
        // as a raw constant when the texture is absent.
        float opacity          = 1.0f;   // overall alpha gain
        float roughness        = 0.5f;   // flat-value / multiplier
        float specular         = 0.5f;   // dielectric F0 factor
        float ao               = 1.0f;
        float normalStrength   = 1.0f;   // scales normal map XY before reconstruction
        // Bump slope multiplier (per-UV-unit). 0 = flat, 0.1 = subtle,
        // 1.0 = physical 1:1 height→normal mapping, 2+ = exaggerated.
        float bumpStrength     = 0.1f;
        float cavityStrength   = 1.0f;   // 0 = ignore cavity, 1 = full darkening
        float displacementScale = 0.0f;  // parallax UV offset magnitude; 0 = off

        uint32_t        flags           = DEFAULT;
        NormalBlendMode normalBlendMode = NormalBlendMode::RNM;
        float           angleFadeStart  = 0.3f;
        int32_t         sortLayer       = 0;

        MaterialDomain GetDomain() const { return MaterialDomain::Decal; }

        // ---- Runtime caches (populated by Resolve + TryPromote) ---------------
        mutable TextureHandle texHandles[SLOT_COUNT];
        mutable int32_t       texBindless[SLOT_COUNT];

        DecalMaterialAsset()
        {
            for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            {
                texHandles[i]  = kInvalidTextureHandle;
                texBindless[i] = -1;
            }
        }
        ~DecalMaterialAsset() = default;
        DecalMaterialAsset(const DecalMaterialAsset&)            = delete;
        DecalMaterialAsset& operator=(const DecalMaterialAsset&) = delete;

        void Resolve(TextureSystem&   texSys,
                     ResourceManager& resMgr,
                     IGraphicsDevice& gfx);
        bool TryPromote(TextureSystem& texSys) const;
        void Release(TextureSystem& texSys, IGraphicsDevice& gfx);
        bool IsFullyResolved() const;
        void SetTexturePath(TextureSlot       slot,
                            const std::string& newPath,
                            TextureSystem&    texSys,
                            IGraphicsDevice&  gfx);

        // Human-readable slot label (for editor UI + serializer keys).
        static const char* SlotLabel(TextureSlot slot);
        // Canonical JSON key (lowercase, stable across UI label changes).
        static const char* SlotKey(TextureSlot slot);
    };
}
