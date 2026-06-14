#include "Resource/MaterialSerializer.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "ECS/Components.h"
#include "ECS/MaterialSchema.h"
#include "System/Log.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace Resource
{

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

static std::string BuildText(const MaterialComponent& mat)
{
    std::ostringstream s;
    char buf[128];

    s << "# DX12 Engine Material\n";

    // ---- Meta / mode (not in MaterialSchema — these aren't editable params) --
    s << "shaderType = "     << static_cast<uint32_t>(mat.shaderType)     << "\n";
    s << "blendMode = "      << static_cast<uint32_t>(mat.userBlendMode)  << "\n";
    s << "shadowCullMode = " << static_cast<uint32_t>(mat.shadowCullMode) << "\n";

    s << "castShadow = "    << (mat.IsCastingShadow()  ? "1" : "0") << "\n";
    s << "receiveShadow = " << (mat.IsReceiveShadow()  ? "1" : "0") << "\n";
    s << "doubleSided = "   << (mat.IsDoubleSided()    ? "1" : "0") << "\n";
    s << "outline = "       << (mat.IsOutlineEnabled() ? "1" : "0") << "\n";
    s << "excludeFromSsao = "
        << ((mat._flags & MaterialComponent::EXCLUDE_FROM_SSAO) ? "1" : "0") << "\n";
    s << "useVertexColor = " << (mat.IsUsingVertexColors() ? "1" : "0") << "\n";

    // outlinePixels lives outside the param schema (it's tied to the OUTLINE
    // flag UI, not surfaced as a generic field).
    snprintf(buf, sizeof(buf), "%.6f", mat.outlinePixels);
    s << "outlinePixels = " << buf << "\n";

    // ---- Schema-driven parameter dump ----------------------------------------
    // Walks kMaterialPBRSchema in declaration order; each row contributes
    // exactly one `name = value` line. FloatComponent fields (e.g. an explicit
    // "emissiveStrength" alias for emissiveColor.w) are intentionally skipped
    // in serialization — they're UI-level conveniences, the parent Color3/4
    // already covers their storage.
    //
    // Adding a new editable field is now a single-row append in
    // include/ECS/MaterialSchema.h, no edit to this file required.
    for (const auto& f : MaterialSchema::kMaterialPBRSchema)
    {
        const float* base = MaterialSchema::FieldPtr(mat, f);
        switch (f.kind)
        {
        case MaterialSchema::Kind::Float:
            snprintf(buf, sizeof(buf), "%.6f", base[0]);
            s << f.name << " = " << buf << "\n";
            break;
        case MaterialSchema::Kind::Color3:
        case MaterialSchema::Kind::Color4:
            // Always write 4 floats — Color3's .w may carry semantic data
            // (emissiveColor.w == strength) so we must round-trip it even
            // though the widget doesn't expose .w directly.
            snprintf(buf, sizeof(buf), "%.6f %.6f %.6f %.6f",
                     base[0], base[1], base[2], base[3]);
            s << f.name << " = " << buf << "\n";
            break;
        case MaterialSchema::Kind::FloatComponent:
            // UI alias only — its parent (Color3/4) already serialised the
            // underlying float4.
            break;
        }
    }

    // ---- UV tiling / offset --------------------------------------------------
    snprintf(buf, sizeof(buf), "%.6f %.6f", mat.texMulAdd.x, mat.texMulAdd.y);
    s << "texTiling = " << buf << "\n";
    snprintf(buf, sizeof(buf), "%.6f %.6f", mat.texMulAdd.z, mat.texMulAdd.w);
    s << "texOffset = " << buf << "\n";

    // ---- Texture slots -------------------------------------------------------
#define WRITE_TEX_(e, label, defpath) \
    s << "tex_" #e " = " << mat.textures[MaterialComponent::e].name << "\n";
    MATERIAL_TEXTURE_SLOTS(WRITE_TEX_)
#undef WRITE_TEX_

    return s.str();
}

bool SaveMaterial(const MaterialComponent& mat, const std::string& path)
{
    const std::string text        = BuildText(mat);
    const uint32_t    textLength  = static_cast<uint32_t>(text.size());
    const uint32_t    payloadSize = textLength + 1; // +1 null terminator

    MaterialMetadata meta{};
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MATERIAL;
    hdr.version      = ASSET_VERSION;
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Material);
    hdr.metadataSize = sizeof(MaterialMetadata);
    hdr.dataSize     = payloadSize;

    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("MaterialSerializer: cannot open '%s' for writing", path.c_str());
        return false;
    }

    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLength);
    const char nul = '\0';
    f.write(&nul, 1);

    if (!f.good())
    {
        LOG_ERROR("MaterialSerializer: write error for '%s'", path.c_str());
        return false;
    }

    LOG_INFO("MaterialSerializer: saved '%s' (%u bytes text)", path.c_str(), textLength);
    return true;
}

// ---------------------------------------------------------------------------
// BuildMaterialBlob — in-memory .imat blob for BuildBlobs pipelines
// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildMaterialBlob(const MaterialComponent& mat)
{
    const std::string text = BuildText(mat);
    const uint32_t textLength = static_cast<uint32_t>(text.size());
    const uint32_t payloadSize = textLength + 1;

    MaterialMetadata meta{};
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MATERIAL;
    hdr.version      = ASSET_VERSION;
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Material);
    hdr.metadataSize = sizeof(MaterialMetadata);
    hdr.dataSize     = payloadSize;

    std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(MaterialMetadata) + payloadSize);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr,  sizeof(hdr));  dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta)); dst += sizeof(meta);
    std::memcpy(dst, text.data(), textLength);
    dst[textLength] = '\0';

    return blob;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool LoadMaterial(const std::string& path, MaterialComponent& mat)
{
    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data))
    {
        LOG_ERROR("MaterialSerializer: cannot open '%s'", path.c_str());
        return false;
    }

    if (!ValidateHeader(data.data(), data.size(), MAGIC_MATERIAL))
    {
        LOG_ERROR("MaterialSerializer: invalid .imat header in '%s'", path.c_str());
        return false;
    }

    const char*  text    = reinterpret_cast<const char*>(GetPayload(data.data()));
    const size_t textLen = GetHeader(data.data())->dataSize;

    // Build texture-slot name → index lookup table.
    struct TexEntry { const char* key; MaterialComponent::TEXTURESLOT slot; };
    static const TexEntry kTexSlots[] = {
#define TEX_ENTRY_(e, label, defpath) { "tex_" #e, MaterialComponent::e },
        MATERIAL_TEXTURE_SLOTS(TEX_ENTRY_)
#undef TEX_ENTRY_
    };
    constexpr int kTexCount = static_cast<int>(sizeof(kTexSlots) / sizeof(kTexSlots[0]));

    // Helper — schema lookup by key. Linear scan is fine, the schema is ~25
    // entries. Returns nullptr for keys that aren't in the param schema (e.g.
    // shaderType, flags, texture slots — those are handled by the manual
    // dispatch below).
    auto findSchemaField = [](const std::string& key) -> const MaterialSchema::FieldDesc*
    {
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
            if (key == f.name) return &f;
        return nullptr;
    };

    std::istringstream ss(std::string(text, textLen));
    std::string line;

    while (std::getline(ss, line))
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;

        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(t.substr(0, eq));
        const std::string val = Trim(t.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        try
        {
            // ---- Schema-driven parameter dispatch ----------------------------
            if (const auto* f = findSchemaField(key))
            {
                float* base = MaterialSchema::FieldPtr(mat, *f);
                switch (f->kind)
                {
                case MaterialSchema::Kind::Float:
                    base[0] = std::stof(val);
                    break;
                case MaterialSchema::Kind::Color3:
                case MaterialSchema::Kind::Color4:
                {
                    float r = 0, g = 0, b = 0, a = 0;
                    if (sscanf_s(val.c_str(), "%f %f %f %f", &r, &g, &b, &a) == 4)
                    {
                        base[0] = r; base[1] = g; base[2] = b; base[3] = a;
                    }
                    break;
                }
                case MaterialSchema::Kind::FloatComponent:
                    // UI alias — written by its parent Color3/4. Ignore on load.
                    break;
                }
                continue;
            }

            // ---- Meta / mode (not in schema) ---------------------------------
            if      (key == "shaderType")    mat.shaderType    = static_cast<MaterialComponent::SHADERTYPE>(std::stoi(val));
            else if (key == "blendMode")     mat.userBlendMode = static_cast<BlendMode>(std::stoi(val));
            else if (key == "shadowCullMode")mat.shadowCullMode = static_cast<ShadowCullMode>(std::stoi(val));
            else if (key == "castShadow")    mat.SetCastShadow(val == "1");
            else if (key == "receiveShadow") mat.SetReceiveShadow(val == "1");
            else if (key == "doubleSided")
            {
                if (val == "1") mat._flags |= MaterialComponent::DOUBLE_SIDED;
                else            mat._flags &= ~MaterialComponent::DOUBLE_SIDED;
            }
            else if (key == "outline")
            {
                if (val == "1") mat._flags |= MaterialComponent::OUTLINE;
                else            mat._flags &= ~MaterialComponent::OUTLINE;
            }
            else if (key == "excludeFromSsao")
            {
                if (val == "1") mat._flags |=  MaterialComponent::EXCLUDE_FROM_SSAO;
                else            mat._flags &= ~MaterialComponent::EXCLUDE_FROM_SSAO;
            }
            else if (key == "useVertexColor")
            {
                if (val == "1") mat._flags |=  MaterialComponent::USE_VERTEXCOLORS;
                else            mat._flags &= ~MaterialComponent::USE_VERTEXCOLORS;
            }
            else if (key == "outlinePixels") mat.outlinePixels = std::stof(val);
            // ---- UV tiling / offset ------------------------------------------
            else if (key == "texTiling")
            {
                float u = 1, v = 1;
                if (sscanf_s(val.c_str(), "%f %f", &u, &v) == 2) { mat.texMulAdd.x = u; mat.texMulAdd.y = v; }
            }
            else if (key == "texOffset")
            {
                float u = 0, v = 0;
                if (sscanf_s(val.c_str(), "%f %f", &u, &v) == 2) { mat.texMulAdd.z = u; mat.texMulAdd.w = v; }
            }
            else
            {
                // ---- Texture slots ---------------------------------------------
                for (int i = 0; i < kTexCount; ++i)
                {
                    if (key == kTexSlots[i].key)
                    {
                        // Normalize separators so TextureSystem hashes match
                        // regardless of whether the .imat was saved on Windows (backslash).
                        std::string normalized = val;
                        for (char& c : normalized) if (c == '\\') c = '/';
                        mat.textures[kTexSlots[i].slot].name = std::move(normalized);
                        break;
                    }
                }
            }
        }
        catch (...) { /* skip malformed values */ }
    }

    mat.SetDirty();
    LOG_INFO("MaterialSerializer: loaded '%s'", path.c_str());
    return true;
}

} // namespace Resource
