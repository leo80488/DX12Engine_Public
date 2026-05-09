#include "Resource/DecalMaterialSerializer.h"
#include "Resource/DecalMaterialAsset.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace Resource
{
    using json = nlohmann::json;

    // Format version history:
    //   v1: albedoTexPath / normalTexPath / ormTexPath + tint/opacity/flags
    //       (pre-Unreal-schema).
    //   v2: 9-slot "textures" map keyed by SlotKey() + scalar multipliers +
    //       new flag bitmask (WRITE_BASECOLOR/NORMAL/ROUGHNESS/SPECULAR/AO).
    static constexpr int kFormatVersion = 2;

    static const char* BlendModeToString(DecalMaterialAsset::NormalBlendMode m)
    {
        return (m == DecalMaterialAsset::NormalBlendMode::Lerp) ? "Lerp" : "RNM";
    }
    static DecalMaterialAsset::NormalBlendMode BlendModeFromString(const std::string& s)
    {
        return (s == "Lerp") ? DecalMaterialAsset::NormalBlendMode::Lerp
                             : DecalMaterialAsset::NormalBlendMode::RNM;
    }

    // ---------------------------------------------------------------------------
    bool SaveDecalMaterial(const DecalMaterialAsset& a, const std::string& path)
    {
        json j;
        j["version"] = kFormatVersion;
        j["name"]    = a.name;

        // Textures: 9-slot map keyed by canonical name. Only non-empty paths
        // are emitted so the JSON stays lean for "scalar-only" decal materials.
        json textures = json::object();
        for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
        {
            const auto slot = static_cast<DecalMaterialAsset::TextureSlot>(i);
            if (!a.texPaths[i].empty())
                textures[DecalMaterialAsset::SlotKey(slot)] = a.texPaths[i];
        }
        j["textures"] = std::move(textures);

        j["baseColorTint"] = { a.baseColorTint.x, a.baseColorTint.y,
                               a.baseColorTint.z, a.baseColorTint.w };

        j["scalars"] = {
            {"opacity",           a.opacity},
            {"roughness",         a.roughness},
            {"specular",          a.specular},
            {"ao",                a.ao},
            {"normalStrength",    a.normalStrength},
            {"bumpStrength",      a.bumpStrength},
            {"cavityStrength",    a.cavityStrength},
            {"displacementScale", a.displacementScale}
        };

        j["flags"]           = a.flags;
        j["normalBlendMode"] = BlendModeToString(a.normalBlendMode);
        j["angleFadeStart"]  = a.angleFadeStart;
        j["sortLayer"]       = a.sortLayer;

        std::ofstream f(path);
        if (!f)
        {
            LOG_ERROR("DecalMaterialSerializer: cannot open '%s' for writing", path.c_str());
            return false;
        }
        f << j.dump(2);
        if (!f.good())
        {
            LOG_ERROR("DecalMaterialSerializer: write error on '%s'", path.c_str());
            return false;
        }
        LOG_SUCCESS("DecalMaterialSerializer: saved '%s' ('%s')", path.c_str(), a.name.c_str());
        return true;
    }

    // ---------------------------------------------------------------------------
    // Read helpers — v1/v2 share some field shapes (name, version, fade,
    // sortLayer). Splitting into a version-specific path keeps the top-level
    // reader short.

    static void LoadV1Compat(DecalMaterialAsset& out, const json& j)
    {
        // v1 → v2 migration:
        //   albedoTexPath → SLOT_BASECOLOR
        //   normalTexPath → SLOT_NORMAL
        //   ormTexPath    → SLOT_ROUGHNESS (we fold ORM.g into roughness slot;
        //                                   the apply CS will sample .r by
        //                                   default, so users with proper ORM
        //                                   textures need to re-export or
        //                                   relocate to matching slots.)
        // We also rewrite v1 flag bits (WRITE_ALBEDO/NORMAL/ROUGH/METAL ≡
        // bits 0..3) to v2 (WRITE_BASECOLOR/NORMAL/ROUGHNESS/SPECULAR/AO
        // ≡ bits 0..4). v1 bits 0..2 map 1:1; v1 bit 3 (METAL) has no slot
        // in v2's Specular workflow and is dropped.
        if (auto it = j.find("textures"); it != j.end() && it->is_object())
        {
            out.texPaths[DecalMaterialAsset::SLOT_BASECOLOR] = it->value("albedo", std::string{});
            out.texPaths[DecalMaterialAsset::SLOT_NORMAL]    = it->value("normal", std::string{});
            out.texPaths[DecalMaterialAsset::SLOT_ROUGHNESS] = it->value("orm",    std::string{});
        }

        if (auto it = j.find("tint"); it != j.end() && it->is_array() && it->size() == 4)
        {
            out.baseColorTint.x = (*it)[0].get<float>();
            out.baseColorTint.y = (*it)[1].get<float>();
            out.baseColorTint.z = (*it)[2].get<float>();
            out.baseColorTint.w = (*it)[3].get<float>();
        }
        out.opacity        = j.value("opacity", out.opacity);
        out.angleFadeStart = j.value("angleFadeStart", out.angleFadeStart);
        out.sortLayer      = j.value("sortLayer", out.sortLayer);

        // Map v1 flags (bits 0..3) to v2 flags (bits 0..4).
        // v1 bit 0 (albedo)  → v2 bit 0 (basecolor)
        // v1 bit 1 (normal)  → v2 bit 1 (normal)
        // v1 bit 2 (rough)   → v2 bit 2 (roughness)
        // v1 bit 3 (metal)   → dropped (no metallic in Unreal Specular workflow)
        const uint32_t v1Flags = j.value<uint32_t>("flags",
            static_cast<uint32_t>(DecalMaterialAsset::DEFAULT));
        out.flags = (v1Flags & 0x7u);  // mask to bits 0..2 which map 1:1
        // Default scalar roughness/specular/ao are fine; normalStrength = 1.
    }

    static void LoadV2(DecalMaterialAsset& out, const json& j)
    {
        if (auto it = j.find("textures"); it != j.end() && it->is_object())
        {
            for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
            {
                const auto slot = static_cast<DecalMaterialAsset::TextureSlot>(i);
                out.texPaths[i] = it->value(DecalMaterialAsset::SlotKey(slot), std::string{});
            }
        }

        if (auto it = j.find("baseColorTint");
            it != j.end() && it->is_array() && it->size() == 4)
        {
            out.baseColorTint.x = (*it)[0].get<float>();
            out.baseColorTint.y = (*it)[1].get<float>();
            out.baseColorTint.z = (*it)[2].get<float>();
            out.baseColorTint.w = (*it)[3].get<float>();
        }

        if (auto it = j.find("scalars"); it != j.end() && it->is_object())
        {
            out.opacity            = it->value("opacity",           out.opacity);
            out.roughness          = it->value("roughness",         out.roughness);
            out.specular           = it->value("specular",          out.specular);
            out.ao                 = it->value("ao",                out.ao);
            out.normalStrength     = it->value("normalStrength",    out.normalStrength);
            out.bumpStrength       = it->value("bumpStrength",      out.bumpStrength);
            out.cavityStrength     = it->value("cavityStrength",    out.cavityStrength);
            out.displacementScale  = it->value("displacementScale", out.displacementScale);
        }

        out.flags           = j.value("flags",
                                      static_cast<uint32_t>(DecalMaterialAsset::DEFAULT));
        out.normalBlendMode = BlendModeFromString(
            j.value("normalBlendMode", std::string("RNM")));
        out.angleFadeStart  = j.value("angleFadeStart", out.angleFadeStart);
        out.sortLayer       = j.value("sortLayer",      out.sortLayer);
    }

    bool LoadDecalMaterial(DecalMaterialAsset& out, const std::string& path)
    {
        std::ifstream f(path);
        if (!f)
        {
            LOG_ERROR("DecalMaterialSerializer: cannot open '%s' for reading", path.c_str());
            return false;
        }

        json j;
        try { f >> j; }
        catch (const std::exception& e)
        {
            LOG_ERROR("DecalMaterialSerializer: parse error on '%s' — %s", path.c_str(), e.what());
            return false;
        }

        const int version = j.value("version", 0);
        if (version > kFormatVersion)
        {
            LOG_ERROR("DecalMaterialSerializer: '%s' has version %d > supported %d",
                      path.c_str(), version, kFormatVersion);
            return false;
        }

        out.name = j.value("name", out.name);

        if (version <= 1) LoadV1Compat(out, j);
        else              LoadV2     (out, j);

        // Invalidate runtime caches — next Resolve() will re-Acquire.
        for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
        {
            out.texHandles[i]  = kInvalidTextureHandle;
            out.texBindless[i] = -1;
        }

        LOG_SUCCESS("DecalMaterialSerializer: loaded '%s' (v%d, '%s')",
                    path.c_str(), version, out.name.c_str());
        return true;
    }
}
