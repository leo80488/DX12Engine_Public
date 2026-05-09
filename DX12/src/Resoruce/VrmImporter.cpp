#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/VrmImporter.h"
#include "Resource/SceneImporter.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace Resource
{
    namespace
    {
        // ---------------------------------------------------------------
        // GLB binary container layout (glTF 2.0 spec):
        //   12 B header  : "glTF" (u32 LE) | version=2 (u32) | length (u32)
        //   N x chunk    : chunkLength (u32) | chunkType (u32) | chunkData
        //   Chunk 0 MUST be JSON ("JSON" = 0x4E4F534A)
        //   Chunk 1 (optional) is BIN ("BIN\0" = 0x004E4942)
        // We only need the JSON chunk; the BIN payload is consumed by Assimp.
        // ---------------------------------------------------------------
        constexpr uint32_t kGlbMagic   = 0x46546C67; // "glTF"
        constexpr uint32_t kChunkJson  = 0x4E4F534A; // "JSON"

        // Returns a string_view-equivalent into `data` for the JSON chunk.
        // On failure, .first == nullptr.
        struct JsonSpan { const char* ptr; size_t len; };
        static JsonSpan ExtractGlbJsonChunk(const uint8_t* data, size_t size)
        {
            if (!data || size < 12 + 8) return { nullptr, 0 };

            uint32_t magic, version, length;
            std::memcpy(&magic,   data + 0, 4);
            std::memcpy(&version, data + 4, 4);
            std::memcpy(&length,  data + 8, 4);

            if (magic != kGlbMagic)
            {
                LOG_ERROR("VrmImporter: not a GLB file (magic mismatch)");
                return { nullptr, 0 };
            }
            if (version != 2)
            {
                LOG_ERROR("VrmImporter: unsupported GLB version %u (expected 2)", version);
                return { nullptr, 0 };
            }
            if (length > size) length = static_cast<uint32_t>(size); // be lenient

            // First chunk header at offset 12
            uint32_t chunkLen, chunkType;
            std::memcpy(&chunkLen,  data + 12, 4);
            std::memcpy(&chunkType, data + 16, 4);

            if (chunkType != kChunkJson)
            {
                LOG_ERROR("VrmImporter: first GLB chunk is not JSON (type=0x%08X)", chunkType);
                return { nullptr, 0 };
            }
            if (20ull + chunkLen > size)
            {
                LOG_ERROR("VrmImporter: GLB JSON chunk overruns file (len=%u, file=%zu)",
                          chunkLen, size);
                return { nullptr, 0 };
            }

            return { reinterpret_cast<const char*>(data + 20), chunkLen };
        }

        // Safe optional-string accessor.
        static std::string JStr(const nlohmann::json& obj, const char* key)
        {
            auto it = obj.find(key);
            if (it == obj.end() || !it->is_string()) return {};
            return it->get<std::string>();
        }

        // Resolve a glTF node index → node name (empty if out of range / unnamed).
        static std::string ResolveNodeName(const nlohmann::json& gltf, int32_t nodeIdx)
        {
            if (nodeIdx < 0) return {};
            auto nodes = gltf.find("nodes");
            if (nodes == gltf.end() || !nodes->is_array()) return {};
            if (static_cast<size_t>(nodeIdx) >= nodes->size()) return {};
            return JStr((*nodes)[nodeIdx], "name");
        }

        // ---------------------------------------------------------------
        // VRM 1.0 ("VRMC_vrm") parser
        // ---------------------------------------------------------------
        static void ParseVrm1(const nlohmann::json& gltf,
                              const nlohmann::json& vrmc,
                              VrmImportResult&      out)
        {
            out.specVersion = 1;

            // ---- meta ----
            if (auto m = vrmc.find("meta"); m != vrmc.end() && m->is_object())
            {
                out.meta.title              = JStr(*m, "name");
                out.meta.version            = JStr(*m, "version");
                out.meta.licenseName        = JStr(*m, "licenseUrl");
                out.meta.contactInformation = JStr(*m, "contactInformation");
                out.meta.reference          = JStr(*m, "references"); // first only — keep simple
                out.meta.licenseTextUrl     = JStr(*m, "thirdPartyLicenses");

                // Authors is an array in 1.0
                if (auto a = m->find("authors"); a != m->end() && a->is_array() && !a->empty())
                {
                    if (a->front().is_string())
                        out.meta.author = a->front().get<std::string>();
                }

                if (auto t = m->find("thumbnailImage"); t != m->end() && t->is_number_integer())
                {
                    // thumbnailImage is a glTF image index; resolve to image name if present.
                    int idx = t->get<int>();
                    auto images = gltf.find("images");
                    if (images != gltf.end() && images->is_array() &&
                        static_cast<size_t>(idx) < images->size())
                        out.meta.thumbnailNodeName = JStr((*images)[idx], "name");
                }
            }

            // ---- humanoid.humanBones is an OBJECT keyed by bone name ----
            if (auto h = vrmc.find("humanoid"); h != vrmc.end() && h->is_object())
            {
                if (auto hb = h->find("humanBones"); hb != h->end() && hb->is_object())
                {
                    out.humanoidBones.reserve(hb->size());
                    for (auto it = hb->begin(); it != hb->end(); ++it)
                    {
                        if (!it.value().is_object()) continue;
                        auto nodeIt = it.value().find("node");
                        if (nodeIt == it.value().end() || !nodeIt->is_number_integer()) continue;

                        VrmHumanoidBone b;
                        b.humanoidName = it.key();
                        b.nodeIndex    = nodeIt->get<int32_t>();
                        b.nodeName     = ResolveNodeName(gltf, b.nodeIndex);
                        out.humanoidBones.push_back(std::move(b));
                    }
                }
            }

            // ---- expressions.preset / expressions.custom ----
            auto parseExpressionMap = [&](const nlohmann::json& mapNode, bool preset)
            {
                if (!mapNode.is_object()) return;
                for (auto it = mapNode.begin(); it != mapNode.end(); ++it)
                {
                    if (!it.value().is_object()) continue;
                    VrmExpression e;
                    e.name     = it.key();
                    e.isPreset = preset;
                    e.isBinary = false;
                    if (auto isb = it.value().find("isBinary");
                        isb != it.value().end() && isb->is_boolean())
                        e.isBinary = isb->get<bool>();

                    if (auto mb = it.value().find("morphTargetBinds");
                        mb != it.value().end() && mb->is_array())
                    {
                        e.morphBindings.reserve(mb->size());
                        for (const auto& b : *mb)
                        {
                            if (!b.is_object()) continue;
                            VrmExpressionBinding bb;
                            if (auto v = b.find("node");   v != b.end() && v->is_number_integer())
                                bb.meshIndex  = v->get<int32_t>(); // VRM 1.0: node index (we record verbatim)
                            if (auto v = b.find("index");  v != b.end() && v->is_number_integer())
                                bb.morphIndex = v->get<int32_t>();
                            if (auto v = b.find("weight"); v != b.end() && v->is_number())
                                bb.weight     = v->get<float>();
                            e.morphBindings.push_back(bb);
                        }
                    }
                    out.expressions.push_back(std::move(e));
                }
            };

            if (auto exprs = vrmc.find("expressions"); exprs != vrmc.end() && exprs->is_object())
            {
                if (auto p = exprs->find("preset"); p != exprs->end()) parseExpressionMap(*p, true);
                if (auto c = exprs->find("custom"); c != exprs->end()) parseExpressionMap(*c, false);
            }
        }

        // ---------------------------------------------------------------
        // VRM 0.x ("VRM") parser
        // ---------------------------------------------------------------
        static void ParseVrm0(const nlohmann::json& gltf,
                              const nlohmann::json& vrm0,
                              VrmImportResult&      out)
        {
            out.specVersion = 0;

            // ---- meta (flat object) ----
            if (auto m = vrm0.find("meta"); m != vrm0.end() && m->is_object())
            {
                out.meta.title              = JStr(*m, "title");
                out.meta.version            = JStr(*m, "version");
                out.meta.author             = JStr(*m, "author");
                out.meta.contactInformation = JStr(*m, "contactInformation");
                out.meta.reference          = JStr(*m, "reference");
                out.meta.licenseName        = JStr(*m, "licenseName");
                out.meta.allowedUserName    = JStr(*m, "allowedUserName");
                out.meta.commercialUseName  = JStr(*m, "commercialUssageName"); // VRM 0.x typo'd key
                if (out.meta.commercialUseName.empty())
                    out.meta.commercialUseName = JStr(*m, "commercialUseName");
                out.meta.licenseTextUrl     = JStr(*m, "otherLicenseUrl");

                if (auto t = m->find("texture"); t != m->end() && t->is_number_integer())
                {
                    int idx = t->get<int>();
                    auto textures = gltf.find("textures");
                    if (textures != gltf.end() && textures->is_array() &&
                        static_cast<size_t>(idx) < textures->size())
                        out.meta.thumbnailNodeName = JStr((*textures)[idx], "name");
                }
            }

            // ---- humanoid.humanBones is an ARRAY of {bone, node, ...} ----
            if (auto h = vrm0.find("humanoid"); h != vrm0.end() && h->is_object())
            {
                if (auto hb = h->find("humanBones"); hb != h->end() && hb->is_array())
                {
                    out.humanoidBones.reserve(hb->size());
                    for (const auto& entry : *hb)
                    {
                        if (!entry.is_object()) continue;
                        auto boneIt = entry.find("bone");
                        auto nodeIt = entry.find("node");
                        if (boneIt == entry.end() || !boneIt->is_string()) continue;
                        if (nodeIt == entry.end() || !nodeIt->is_number_integer()) continue;

                        VrmHumanoidBone b;
                        b.humanoidName = boneIt->get<std::string>();
                        b.nodeIndex    = nodeIt->get<int32_t>();
                        b.nodeName     = ResolveNodeName(gltf, b.nodeIndex);
                        out.humanoidBones.push_back(std::move(b));
                    }
                }
            }

            // ---- blendShapeMaster.blendShapeGroups is an ARRAY ----
            if (auto bsm = vrm0.find("blendShapeMaster"); bsm != vrm0.end() && bsm->is_object())
            {
                if (auto groups = bsm->find("blendShapeGroups");
                    groups != bsm->end() && groups->is_array())
                {
                    out.expressions.reserve(groups->size());
                    for (const auto& g : *groups)
                    {
                        if (!g.is_object()) continue;
                        VrmExpression e;
                        // 0.x uses presetName (e.g. "joy") + name (display name)
                        e.name     = JStr(g, "presetName");
                        if (e.name.empty() || e.name == "unknown")
                            e.name = JStr(g, "name");
                        e.isPreset = (e.name != JStr(g, "name") || JStr(g, "presetName").size() > 0);
                        if (auto isb = g.find("isBinary"); isb != g.end() && isb->is_boolean())
                            e.isBinary = isb->get<bool>();

                        if (auto binds = g.find("binds"); binds != g.end() && binds->is_array())
                        {
                            e.morphBindings.reserve(binds->size());
                            for (const auto& b : *binds)
                            {
                                if (!b.is_object()) continue;
                                VrmExpressionBinding bb;
                                if (auto v = b.find("mesh");   v != b.end() && v->is_number_integer())
                                    bb.meshIndex  = v->get<int32_t>();
                                if (auto v = b.find("index");  v != b.end() && v->is_number_integer())
                                    bb.morphIndex = v->get<int32_t>();
                                // VRM 0.x weight is 0..100, normalize to 0..1
                                if (auto v = b.find("weight"); v != b.end() && v->is_number())
                                    bb.weight = v->get<float>() / 100.f;
                                e.morphBindings.push_back(bb);
                            }
                        }
                        out.expressions.push_back(std::move(e));
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // Serialise VrmImportResult to a stable text format (.ivrm).
        // Layout:
        //   # comment lines start with #
        //   V version=<0|1>
        //   M key=<value>             (one per meta field; empty values omitted)
        //   H name=<humanoid> node=<idx> nodeName=<resolved>
        //   E name=<expr> preset=<0|1> binary=<0|1> binds=<n>
        //     B mesh=<i> morph=<i> weight=<f>      (n lines following an E)
        // ---------------------------------------------------------------
        static std::vector<uint8_t> BuildIvrmBlob(const VrmImportResult& vrm)
        {
            std::ostringstream ss;
            ss << "# .ivrm — VRM extension metadata (humanoid + expressions + meta)\n";
            ss << "V version=" << vrm.specVersion << "\n";

            auto putMeta = [&](const char* key, const std::string& value)
            {
                if (!value.empty()) ss << "M " << key << "=" << value << "\n";
            };
            putMeta("title",              vrm.meta.title);
            putMeta("version",            vrm.meta.version);
            putMeta("author",             vrm.meta.author);
            putMeta("contact",            vrm.meta.contactInformation);
            putMeta("reference",          vrm.meta.reference);
            putMeta("license",            vrm.meta.licenseName);
            putMeta("allowedUserName",    vrm.meta.allowedUserName);
            putMeta("commercialUseName",  vrm.meta.commercialUseName);
            putMeta("licenseUrl",         vrm.meta.licenseTextUrl);
            putMeta("thumbnail",          vrm.meta.thumbnailNodeName);

            for (const auto& b : vrm.humanoidBones)
            {
                ss << "H name=" << b.humanoidName
                   << " node=" << b.nodeIndex
                   << " nodeName=" << b.nodeName
                   << "\n";
            }

            for (const auto& e : vrm.expressions)
            {
                ss << "E name=" << e.name
                   << " preset=" << (e.isPreset ? 1 : 0)
                   << " binary=" << (e.isBinary ? 1 : 0)
                   << " binds=" << e.morphBindings.size()
                   << "\n";
                for (const auto& bb : e.morphBindings)
                {
                    ss << "  B mesh=" << bb.meshIndex
                       << " morph="   << bb.morphIndex
                       << " weight="  << bb.weight
                       << "\n";
                }
            }

            const std::string s = ss.str();
            return std::vector<uint8_t>(s.begin(), s.end());
        }
    } // anonymous namespace

    // =====================================================================
    // VrmImporter::Import
    // =====================================================================
    VrmImportResult VrmImporter::Import(const uint8_t* data, size_t size)
    {
        VrmImportResult result;
        if (!data || size == 0)
        {
            LOG_ERROR("VrmImporter: empty input");
            return result;
        }

        const JsonSpan js = ExtractGlbJsonChunk(data, size);
        if (!js.ptr)
        {
            // Error already logged.
            return result;
        }

        nlohmann::json gltf;
        try
        {
            gltf = nlohmann::json::parse(js.ptr, js.ptr + js.len);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("VrmImporter: JSON parse failed: %s", e.what());
            return result;
        }

        auto extensions = gltf.find("extensions");
        if (extensions == gltf.end() || !extensions->is_object())
        {
            LOG_ERROR("VrmImporter: glTF has no 'extensions' block — not a VRM file");
            return result;
        }

        if (auto v1 = extensions->find("VRMC_vrm"); v1 != extensions->end() && v1->is_object())
        {
            ParseVrm1(gltf, *v1, result);
        }
        else if (auto v0 = extensions->find("VRM"); v0 != extensions->end() && v0->is_object())
        {
            ParseVrm0(gltf, *v0, result);
        }
        else
        {
            LOG_ERROR("VrmImporter: neither VRMC_vrm (1.0) nor VRM (0.x) extension found");
            return result;
        }

        result.success = true;
        LOG_INFO("VrmImporter: parsed VRM %d.x — %zu humanoid bone(s), %zu expression(s)",
                 result.specVersion,
                 result.humanoidBones.size(),
                 result.expressions.size());
        return result;
    }

    // =====================================================================
    // VrmImporter::BuildBlobs
    // =====================================================================
    std::vector<VrmExportedFile> VrmImporter::BuildBlobs(
        const std::string&          sourcePath,
        const std::vector<uint8_t>& sourceData,
        const VrmImportResult&      vrm)
    {
        std::vector<VrmExportedFile> files;

        // 1) Delegate mesh/skel/mat/anim extraction to SceneImporter (Assimp/glb).
        SceneImporter::ImportResult scene = SceneImporter::Import(sourcePath, sourceData);
        if (!scene.success || scene.files.empty())
        {
            LOG_ERROR("VrmImporter: SceneImporter failed for '%s'", sourcePath.c_str());
            return files;
        }

        files.reserve(scene.files.size() + 1);
        for (auto& f : scene.files)
            files.push_back({ std::move(f.relativePath), std::move(f.blob) });

        // 2) Append the .ivrm metadata blob (only if VRM parsing succeeded).
        if (vrm.success)
        {
            const std::string stem = std::filesystem::path(sourcePath).stem().string();
            const std::string ivrmRel = stem + "/" + stem + ".ivrm";
            files.push_back({ ivrmRel, BuildIvrmBlob(vrm) });
            LOG_INFO("VrmImporter:   .ivrm → '%s'", ivrmRel.c_str());
        }
        else
        {
            LOG_WARNING("VrmImporter: VRM extension data missing/invalid — emitting glTF data only");
        }

        return files;
    }
} // namespace Resource
