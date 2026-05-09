#include "Graphics/ShaderLibrary.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/DxcCompiler.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cassert>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace
{
    // Returns the last-write timestamp as a 64-bit value (100-ns ticks since
    // 1601-01-01, matching FILETIME).  Returns 0 if the file does not exist.
    uint64_t GetFileWriteTime(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr))
            return 0;
        ULARGE_INTEGER t;
        t.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
        t.HighPart = attr.ftLastWriteTime.dwHighDateTime;
        return t.QuadPart;
    }

    // Scans hlslSource for `#include "foo.hlsli"` directives and returns a
    // list of resolved full paths. Simple line-based scanner — handles the
    // common HLSL case (quote includes, optional whitespace) and skips
    // comment-guarded lines. Doesn't deal with `#include <...>` (system-
    // style, unused in this codebase) or conditional includes that are
    // gated behind runtime defines.
    std::vector<std::string> ExtractIncludePaths(const std::string& hlslPath,
                                                 const std::string& shaderDir)
    {
        std::vector<std::string> out;
        std::ifstream f(hlslPath);
        if (!f) return out;

        const std::filesystem::path sourceDir =
            std::filesystem::path(hlslPath).parent_path();

        std::string line;
        while (std::getline(f, line))
        {
            // Strip trailing `\r` from CRLF files; skip empty lines.
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // Ignore lines where `//` comes before `#include` (line comment).
            const std::size_t commentPos = line.find("//");
            const std::size_t hashPos    = line.find('#');
            if (hashPos == std::string::npos) continue;
            if (commentPos != std::string::npos && commentPos < hashPos) continue;

            // Match `#[ws]include[ws]"NAME"` starting at hashPos.
            std::size_t i = hashPos + 1;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (line.compare(i, 7, "include") != 0) continue;
            i += 7;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= line.size() || line[i] != '"') continue;
            ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != '"') ++i;
            if (i >= line.size()) continue;
            const std::string name = line.substr(start, i - start);
            if (name.empty()) continue;

            // Resolve: first relative to the including file's directory
            // (matches D3D_COMPILE_STANDARD_FILE_INCLUDE), fall back to
            // shaderDir (for root-level shared headers).
            std::error_code ec;
            std::filesystem::path p = sourceDir / name;
            if (!std::filesystem::exists(p, ec))
                p = std::filesystem::path(shaderDir) / name;
            if (!std::filesystem::exists(p, ec)) continue;

            out.push_back(p.string());
        }
        return out;
    }

    // Returns the maximum last-write time across `hlslPath` and every file
    // it transitively #includes. Cycle-safe via `visited`. Zero for missing
    // files — caller treats "0 transitive time" as "no usable info", not
    // "fresh source", so a missing include never causes a false re-use.
    uint64_t MaxIncludeTime(const std::string& hlslPath,
                            const std::string& shaderDir,
                            std::unordered_set<std::string>& visited)
    {
        if (!visited.insert(hlslPath).second) return 0;

        uint64_t maxTime = GetFileWriteTime(hlslPath);
        for (const auto& inc : ExtractIncludePaths(hlslPath, shaderDir))
        {
            const uint64_t t = MaxIncludeTime(inc, shaderDir, visited);
            if (t > maxTime) maxTime = t;
        }
        return maxTime;
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
void ShaderLibrary::Init(IGraphicsDevice& gfx, const char* shaderDir)
{
    m_gfx       = &gfx;
    m_shaderDir = shaderDir ? shaderDir : "";
    if (!m_shaderDir.empty() && m_shaderDir.back() != '/' && m_shaderDir.back() != '\\')
        m_shaderDir += '/';
}

void ShaderLibrary::Shutdown(IGraphicsDevice& /*gfx*/)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_shaderCache.clear();
    m_reflectionCache.clear();
    m_dynamicRegistry.clear();
    m_dynamicPathToId.clear();
    m_gfx = nullptr;
}

void ShaderLibrary::Register(ShaderID id, RHI::ShaderStage stage,
                              const char* hlslFile, const char* entry)
{
    const uint32_t idx = static_cast<uint32_t>(id);
    assert(idx < static_cast<uint32_t>(ShaderID::Count));
    m_registry[idx].hlslFile   = hlslFile ? hlslFile : "";
    m_registry[idx].entry      = entry    ? entry    : "main";
    m_registry[idx].stage      = stage;
    m_registry[idx].registered = true;
}

// ---------------------------------------------------------------------------
const RHI::Shader* ShaderLibrary::GetShader(ShaderID id, PermutationKey perm)
{
    return GetShaderInternal(static_cast<uint32_t>(id), perm);
}

const RHI::Shader* ShaderLibrary::GetDynamicShader(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return nullptr;
    return GetShaderInternal(dynId, perm);
}

const ShaderReflect::Reflection* ShaderLibrary::GetReflection(ShaderID id, PermutationKey perm)
{
    return GetReflectionInternal(static_cast<uint32_t>(id), perm);
}

const ShaderReflect::Reflection* ShaderLibrary::GetDynamicReflection(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return nullptr;
    return GetReflectionInternal(dynId, perm);
}

const RHI::Shader* ShaderLibrary::GetShaderInternal(uint32_t id, PermutationKey perm)
{
    const CacheKey key{ id, perm.bits };

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_shaderCache.find(key);
        if (it != m_shaderCache.end())
            return &it->second;
        // Negative cache: a previous attempt for this (id, perm) failed and
        // there's no point re-running DXC every frame against a broken file.
        // Hot-reload's ClearCaches drops this map so future edits retry.
        if (m_failedShaders.find(key) != m_failedShaders.end())
            return nullptr;
    }

    RHI::Shader shader;
    std::vector<uint8_t> bytecode;
    if (!LoadOrCompile(id, perm, shader, bytecode))
        return nullptr;

    // Reflect off the fresh bytecode. Failure here is non-fatal — shader still
    // runs, we just skip auto-UI / binding discovery for it.
    ShaderReflect::Reflection reflection;
    const bool reflected = ShaderReflect::Reflect(bytecode.data(), bytecode.size(), reflection);

    std::lock_guard<std::mutex> lk(m_mutex);
    auto [it, inserted] = m_shaderCache.emplace(key, shader);
    if (reflected) m_reflectionCache.emplace(key, std::move(reflection));
    return &it->second;
}

const ShaderReflect::Reflection* ShaderLibrary::GetReflectionInternal(uint32_t id, PermutationKey perm)
{
    // Warm the shader (and reflection) cache if needed.
    if (!GetShaderInternal(id, perm)) return nullptr;

    const CacheKey key{ id, perm.bits };
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_reflectionCache.find(key);
    return it != m_reflectionCache.end() ? &it->second : nullptr;
}

const ShaderLibrary::ShaderEntry* ShaderLibrary::GetRegistryEntry(uint32_t id) const
{
    if (id < static_cast<uint32_t>(ShaderID::Count))
    {
        const ShaderEntry& e = m_registry[id];
        return e.registered ? &e : nullptr;
    }
    const uint32_t dynIdx = id - static_cast<uint32_t>(ShaderID::Count);
    if (dynIdx >= m_dynamicRegistry.size()) return nullptr;
    const ShaderEntry& e = m_dynamicRegistry[dynIdx];
    return e.registered ? &e : nullptr;
}

uint32_t ShaderLibrary::RegisterDynamic(const char* hlslFile, RHI::ShaderStage stage,
                                        const char* entry)
{
    if (!hlslFile || !*hlslFile)
    {
        LOG_WARNING("ShaderLibrary::RegisterDynamic: empty path");
        return kInvalidDynShaderID;
    }
    const char* entryName = (entry && *entry) ? entry : "main";

    // Dedup key covers path + entry + stage so the same .hlsl with a
    // different entry point gets its own ID.
    std::string key;
    key.reserve(std::strlen(hlslFile) + 16);
    key.append(hlslFile);
    key.push_back('|');
    key.append(entryName);
    key.push_back('|');
    key.push_back(static_cast<char>('0' + static_cast<int>(stage)));

    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_dynamicPathToId.find(key);
    if (it != m_dynamicPathToId.end())
        return it->second;

    ShaderEntry e;
    e.hlslFile   = hlslFile;
    e.entry      = entryName;
    e.stage      = stage;
    e.registered = true;
    const uint32_t dynIdx = static_cast<uint32_t>(m_dynamicRegistry.size());
    m_dynamicRegistry.push_back(std::move(e));

    const uint32_t id = static_cast<uint32_t>(ShaderID::Count) + dynIdx;
    m_dynamicPathToId.emplace(std::move(key), id);

    LOG_INFO("ShaderLibrary: registered dynamic shader '%s' (entry=%s stage=%d) → id=%u",
             hlslFile, entryName, static_cast<int>(stage), id);
    return id;
}

bool ShaderLibrary::PreloadShader(ShaderID id, PermutationKey perm)
{
    return GetShader(id, perm) != nullptr;
}

void ShaderLibrary::ClearCaches()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_shaderCache.clear();
    m_reflectionCache.clear();
    m_failedShaders.clear();   // hot-reload retries previously-broken shaders
}

std::string ShaderLibrary::GetCompileError(ShaderID id, PermutationKey perm)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_failedShaders.find({ static_cast<uint32_t>(id), perm.bits });
    return it != m_failedShaders.end() ? it->second : std::string{};
}

std::string ShaderLibrary::GetDynamicCompileError(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return {};
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_failedShaders.find({ dynId, perm.bits });
    return it != m_failedShaders.end() ? it->second : std::string{};
}

// ---------------------------------------------------------------------------
bool ShaderLibrary::LoadOrCompile(uint32_t id, PermutationKey perm,
                                  RHI::Shader& outShader,
                                  std::vector<uint8_t>& outBytecode)
{
    outBytecode.clear();
    const ShaderEntry* entryPtr = GetRegistryEntry(id);
    if (!entryPtr)
    {
        LOG_ERROR("ShaderLibrary: shader id %u not registered", id);
        return false;
    }
    if (!m_gfx)
    {
        LOG_ERROR("ShaderLibrary: not initialised (call Init first)");
        return false;
    }

    const ShaderEntry& entry     = *entryPtr;
    // Resolve the path. The fixed-enum Register() path passes bare filenames
    // like "GBuffer.vs.hlsl" that need shaderDir prepended ("shaders/..."),
    // while the Resource Panel's drag-drop emits project-root-relative paths
    // like "asset/Shaders/my_custom.ps.hlsl" that must NOT be prefixed.
    //
    // Distinguisher: a path that already contains a directory separator
    // (`/` or `\`) was produced by someone who knows where the file lives;
    // leave it alone. Absolute paths (drive letter, UNC, rooted slash) also
    // bypass the prefix.
    const bool isAbsolute =
        (entry.hlslFile.size() >= 2 && entry.hlslFile[1] == ':') ||
        (entry.hlslFile.size() >= 2 && entry.hlslFile[0] == '\\' && entry.hlslFile[1] == '\\') ||
        (!entry.hlslFile.empty() && entry.hlslFile[0] == '/');
    const bool hasDirComponent =
        entry.hlslFile.find('/')  != std::string::npos ||
        entry.hlslFile.find('\\') != std::string::npos;
    const std::string  hlslPath = (isAbsolute || hasDirComponent)
        ? entry.hlslFile
        : (m_shaderDir + entry.hlslFile);
    const std::string  cachePath = CachePath(hlslPath, perm, entry.entry);

    // --- Try disk cache first ---
    // Skip the cache if the HLSL source OR any transitive `#include "X.hlsli"`
    // is newer than the cached .ishdr. The old check only looked at the root
    // .hlsl file, which missed edits to shared headers like material.hlsli —
    // those would silently ship a stale-compiled binary with the new struct
    // layout (see memory: project_shader_cache_include_trap.md).
    std::unordered_set<std::string> visited;
    const uint64_t hlslTime  = MaxIncludeTime(hlslPath, m_shaderDir, visited);
    const uint64_t cacheTime = GetFileWriteTime(cachePath);
    const bool     cacheStale = (hlslTime != 0 && hlslTime > cacheTime);

    if (cacheStale)
        LOG_INFO("ShaderLibrary: source (or #include) newer than cache, recompiling '%s' (perm=0x%08X)",
                 entry.hlslFile.c_str(), perm.bits);

    std::vector<uint8_t> sc;
    if (!cacheStale)
        sc = ReadFile(cachePath);

    if (!sc.empty() &&
        Resource::ValidateHeader(sc.data(), sc.size(), Resource::MAGIC_SHADER))
    {
        const Resource::ShaderMetadata* meta =
            Resource::GetMetadata<Resource::ShaderMetadata>(sc.data());
        const uint8_t* dxbc     = Resource::GetPayload(sc.data());
        const size_t   dxbcSize = Resource::GetHeader(sc.data())->dataSize;
        const auto     stage    = static_cast<RHI::ShaderStage>(meta->stage);

        if (m_gfx->CreateShader(stage, dxbc, dxbcSize, outShader))
        {
            outBytecode.assign(dxbc, dxbc + dxbcSize);
            LOG_INFO("ShaderLibrary: loaded cached '%s' (perm=0x%08X)",
                     entry.hlslFile.c_str(), perm.bits);
            return true;
        }
        LOG_WARNING("ShaderLibrary: cached shader invalid, recompiling '%s'",
                    entry.hlslFile.c_str());
    }

    // --- Compile from HLSL via DXC ---
    std::vector<uint8_t> src = ReadFile(hlslPath);
    if (src.empty())
    {
        LOG_ERROR("ShaderLibrary: cannot read '%s'", hlslPath.c_str());
        return false;
    }

    DxcCompiler::CompileOptions opts;
    opts.sourceName = hlslPath;
    opts.entry      = entry.entry;
    opts.stage      = entry.stage;
    opts.includeDir = m_shaderDir;
#ifdef _DEBUG
    opts.debug      = true;
#else
    opts.debug      = false;
#endif
    for (auto& [name, val] : MakeDefines(perm))
        opts.defines.push_back({ name, val });

    const DxcCompiler::CompileResult cr = DxcCompiler::Compile(src.data(), src.size(), opts);
    if (!cr.ok)
    {
        LOG_ERROR("ShaderLibrary: DXC compile failed for '%s' (perm=0x%08X)\n%s",
                  entry.hlslFile.c_str(), perm.bits, cr.errorMsg.c_str());
        // Negative-cache the failure so per-frame draw loops don't re-run DXC
        // and spam the log. ClearCaches (called by hot-reload on file change)
        // drops the entry — that's the natural retry trigger.
        std::lock_guard<std::mutex> lk(m_mutex);
        m_failedShaders[{ id, perm.bits }] =
            cr.errorMsg.empty() ? std::string{ "DXC compile failed (no diagnostics)" }
                                : cr.errorMsg;
        return false;
    }
    if (!cr.errorMsg.empty())
    {
        // Warnings — keep going.
        LOG_WARNING("ShaderLibrary: DXC warnings for '%s' (perm=0x%08X)\n%s",
                    entry.hlslFile.c_str(), perm.bits, cr.errorMsg.c_str());
    }

    const void*  dxil     = cr.dxil.data();
    const size_t dxilSize = cr.dxil.size();

    if (!m_gfx->CreateShader(entry.stage, dxil, dxilSize, outShader))
    {
        LOG_ERROR("ShaderLibrary: CreateShader failed for '%s'", entry.hlslFile.c_str());
        return false;
    }

    outBytecode = cr.dxil;

    // --- Write cache ---
    {
        Resource::ShaderMetadata meta{};
        meta.bytecodeSize = static_cast<uint32_t>(dxilSize);
        meta.stage        = static_cast<uint8_t>(entry.stage);
        meta.shaderModel  = static_cast<uint8_t>(RHI::ShaderModel::SM_6_6);
        strncpy_s(meta.entryPoint, sizeof(meta.entryPoint),
                  entry.entry.c_str(), entry.entry.size());

        Resource::AssetHeader hdr{};
        hdr.magic        = Resource::MAGIC_SHADER;
        hdr.version      = Resource::ASSET_VERSION;
        hdr.resourceType = 0;
        hdr.metadataSize = sizeof(Resource::ShaderMetadata);
        hdr.dataSize     = static_cast<uint32_t>(dxilSize);
        hdr.flags        = 0;
        hdr.reserved     = 0;

        const size_t total =
            sizeof(Resource::AssetHeader) + sizeof(Resource::ShaderMetadata) + dxilSize;
        std::vector<uint8_t> blob(total);
        uint8_t* dst = blob.data();
        std::memcpy(dst, &hdr,  sizeof(Resource::AssetHeader));    dst += sizeof(Resource::AssetHeader);
        std::memcpy(dst, &meta, sizeof(Resource::ShaderMetadata)); dst += sizeof(Resource::ShaderMetadata);
        std::memcpy(dst, dxil,  dxilSize);

        if (WriteFile(cachePath, blob))
            LOG_INFO("ShaderLibrary: compiled + cached '%s' (perm=0x%08X, %zu bytes)",
                     entry.hlslFile.c_str(), perm.bits, total);
        else
            LOG_WARNING("ShaderLibrary: compiled '%s' but failed to write cache",
                        entry.hlslFile.c_str());
    }

    return true;
}

// ---------------------------------------------------------------------------
// CachePath — maps an .hlsl source path + (perm, entry) to an .ishdr cache
// path under m_cacheDir. Strips m_shaderDir from the source's parent so the
// engine's own shaders go directly under shader_cache/ (not
// shader_cache/shaders/…). Project-relative custom shaders (e.g. from
// asset/Shaders/) preserve their subdirectory structure so they don't
// collide. Absolute drag-dropped sources flatten to just the filename.
std::string ShaderLibrary::CachePath(const std::string& hlslPath,
                                      PermutationKey perm,
                                      const std::string& entryPoint) const
{
    std::filesystem::path src(hlslPath);
    const std::string stem = src.stem().string();        // "GBuffer.ps"

    char suffix[256];
    snprintf(suffix, sizeof(suffix), "_%s_P%08X.ishdr",
             entryPoint.c_str(), perm.bits);

    std::filesystem::path root(m_cacheDir);
    std::filesystem::path parent;
    if (!src.is_absolute())
    {
        parent = src.parent_path();

        // Strip the m_shaderDir prefix so shaders/GBuffer.ps.hlsl →
        // shader_cache/GBuffer... rather than shader_cache/shaders/GBuffer…
        if (!m_shaderDir.empty())
        {
            std::string sdir = m_shaderDir;
            while (!sdir.empty() && (sdir.back() == '/' || sdir.back() == '\\'))
                sdir.pop_back();
            const std::string pStr = parent.string();
            if (!sdir.empty())
            {
                if (pStr == sdir)
                {
                    parent.clear();
                }
                else if (pStr.size() > sdir.size() &&
                         (pStr[sdir.size()] == '/' || pStr[sdir.size()] == '\\') &&
                         pStr.compare(0, sdir.size(), sdir) == 0)
                {
                    parent = pStr.substr(sdir.size() + 1);
                }
            }
        }
    }

    std::filesystem::path out = root / parent / (stem + suffix);
    return out.string();
}

std::vector<uint8_t> ShaderLibrary::ReadFile(const std::string& path)
{
    // Route through AssetFS so shader_cache/*.ishdr reads go through the pak
    // when mounted (game build) or fall back to disk (editor/dev).
    std::vector<uint8_t> buf;
    ::Resource::AssetFS::Get().ReadFile(path, buf);
    return buf;
}

bool ShaderLibrary::WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
    // Cache writes target shader_cache/ which probably doesn't exist on a
    // fresh checkout. create_directories is idempotent — cheap noop once the
    // directory is there, safe to call every write.
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return f.good();
}

std::vector<std::pair<std::string,std::string>>
ShaderLibrary::MakeDefines(PermutationKey perm)
{
    std::vector<std::pair<std::string,std::string>> defs;
    auto add = [&](const char* name, bool active) {
        defs.push_back({ name, active ? "1" : "0" });
    };
    add("HAS_NORMALMAP",       perm.Has(PermutationKey::HAS_NORMALMAP));
    add("HAS_EMISSIVE",        perm.Has(PermutationKey::HAS_EMISSIVE));
    add("ALPHA_TEST",          perm.Has(PermutationKey::ALPHA_TEST));
    add("DOUBLE_SIDED",        perm.Has(PermutationKey::DOUBLE_SIDED));
    add("SKINNED",             perm.Has(PermutationKey::SKINNED));
    add("ALPHA_BLEND",         perm.Has(PermutationKey::ALPHA_BLEND));
    add("ADDITIVE_BLEND",      perm.Has(PermutationKey::ADDITIVE_BLEND));
    add("PREMULTIPLIED_BLEND", perm.Has(PermutationKey::PREMULTIPLIED_BLEND));
    add("MULTIPLY_BLEND",      perm.Has(PermutationKey::MULTIPLY_BLEND));
    add("BILLBOARD",           perm.Has(PermutationKey::BILLBOARD));
    add("UNLIT",               perm.Has(PermutationKey::UNLIT));
    add("NPR_PASS",            perm.Has(PermutationKey::NPR_STENCIL));
    add("OCCLUSION_CULL",      perm.Has(PermutationKey::OCCLUSION_CULL));
    return defs;
}
