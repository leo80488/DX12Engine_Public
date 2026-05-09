#include "SceneSerialization.h"
#include "System/Log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <algorithm>

namespace
{
    constexpr uint32_t kBinaryMagic = 0x4E435353u; // "SSCN"
    constexpr uint32_t kBinaryVersion = 2u;  // v2: added outlinePixels

    template<typename T>
    void Write(std::ostream& out, const T& v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }

    template<typename T>
    bool Read(std::istream& in, T& v)
    {
        return in.read(reinterpret_cast<char*>(&v), sizeof(T)).good();
    }

    void WriteString(std::ostream& out, const std::string& s)
    {
        uint32_t len = static_cast<uint32_t>(s.size());
        Write(out, len);
        if (len)
            out.write(s.data(), len);
    }

    bool ReadString(std::istream& in, std::string& s)
    {
        uint32_t len = 0;
        if (!Read(in, len))
            return false;
        s.resize(len);
        if (len && !in.read(&s[0], len).good())
            return false;
        return true;
    }

    void WriteFloat3(std::ostream& out, const DirectX::XMFLOAT3& v)
    {
        Write(out, v.x);
        Write(out, v.y);
        Write(out, v.z);
    }

    bool ReadFloat3(std::istream& in, DirectX::XMFLOAT3& v)
    {
        return Read(in, v.x) && Read(in, v.y) && Read(in, v.z);
    }

    void WriteFloat4(std::ostream& out, const DirectX::XMFLOAT4& v)
    {
        Write(out, v.x);
        Write(out, v.y);
        Write(out, v.z);
        Write(out, v.w);
    }

    bool ReadFloat4(std::istream& in, DirectX::XMFLOAT4& v)
    {
        return Read(in, v.x) && Read(in, v.y) && Read(in, v.z) && Read(in, v.w);
    }

    void WriteUint4(std::ostream& out, const DirectX::XMUINT4& v)
    {
        Write(out, v.x);
        Write(out, v.y);
        Write(out, v.z);
        Write(out, v.w);
    }

    bool ReadUint4(std::istream& in, DirectX::XMUINT4& v)
    {
        return Read(in, v.x) && Read(in, v.y) && Read(in, v.z) && Read(in, v.w);
    }

    template<typename T, typename WriteElemFn>
    void WriteVector(std::ostream& out, const std::vector<T>& vec, WriteElemFn&& writeElem)
    {
        uint32_t n = static_cast<uint32_t>(vec.size());
        Write(out, n);
        for (const auto& e : vec)
            writeElem(out, e);
    }

    template<typename T, typename ReadElemFn>
    bool ReadVector(std::istream& in, std::vector<T>& vec, ReadElemFn&& readElem)
    {
        uint32_t n = 0;
        if (!Read(in, n))
            return false;
        vec.resize(n);
        for (auto& e : vec)
            if (!readElem(in, e))
                return false;
        return true;
    }
}

// ---------------------------------------------------------------------------
// MaterialComponent <-> MaterialComponentData
// ---------------------------------------------------------------------------

MaterialComponentData FromMaterialComponent(const MaterialComponent& m)
{
    MaterialComponentData d;
    d._flags = m._flags;
    d.shaderType = static_cast<uint32_t>(m.shaderType);
    d.engineStencilRef = static_cast<uint32_t>(m.engineStencilRef);
    d.userBlendMode = static_cast<uint32_t>(m.userBlendMode);

    memcpy(d.baseColor, &m.baseColor.x, 4 * sizeof(float));
    memcpy(d.specularColor, &m.specularColor.x, 4 * sizeof(float));
    memcpy(d.emissiveColor, &m.emissiveColor.x, 4 * sizeof(float));
    memcpy(d.texMulAdd, &m.texMulAdd.x, 4 * sizeof(float));
    d.roughnessMax = m.roughnessMax;
	d.roughnessMin = m.roughnessMin;
    d.metalnessMax = m.metalnessMax;
	d.metalnessMin = m.metalnessMin;
    d.reflectance = m.reflectance;
    d.normalMapStrength = m.normalMapStrength;
    d.parallaxOcclusionMapping = m.parallaxOcclusionMapping;
    d.alphaRef = m.alphaRef;
    d.saturation = m.saturation;
    d.userStencilRef = m.userStencilRef;

    d.texAnimDirection[0] = m.texAnimDirection.x;
    d.texAnimDirection[1] = m.texAnimDirection.y;
    d.texAnimFrameRate = m.texAnimFrameRate;
    d.texAnimElapsedTime = m.texAnimElapsedTime;

    for (size_t i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
    {
        d.textures[i].name = m.textures[i].name;
        d.textures[i].uvset = m.textures[i].uvset;
    }

    d.useCustomShader    = m.useCustomShader;
    d.customShaderPath   = m.customShaderPath;
    d.customShadingModel = static_cast<uint8_t>(m.customShadingModel);
    d.customTextures.clear();
    d.customTextures.reserve(m.customTextures.size());
    for (const auto& [name, tex] : m.customTextures)
        d.customTextures.push_back({ name,
            MaterialComponentData::CustomTextureData{ tex.name, tex.uvset } });
    d.customParams.clear();
    d.customParams.reserve(m.customParams.size());
    for (const auto& [name, v] : m.customParams)
        d.customParams.emplace_back(name, v);
    d.layerMask = m.layerMask;
    d.cameraSource = m.cameraSource;
    d.outlinePixels = m.outlinePixels;
    return d;
}

void ToMaterialComponent(const MaterialComponentData& d, MaterialComponent& m)
{
    m._flags = d._flags;
    m.shaderType = static_cast<MaterialComponent::SHADERTYPE>(d.shaderType);
    m.engineStencilRef = static_cast<StencilRef>(d.engineStencilRef);
    m.userBlendMode = static_cast<BlendMode>(d.userBlendMode);

    memcpy(&m.baseColor.x, d.baseColor, 4 * sizeof(float));
    memcpy(&m.specularColor.x, d.specularColor, 4 * sizeof(float));
    memcpy(&m.emissiveColor.x, d.emissiveColor, 4 * sizeof(float));
    memcpy(&m.texMulAdd.x, d.texMulAdd, 4 * sizeof(float));
    m.roughnessMax = d.roughnessMax;
    m.roughnessMin = d.roughnessMin;

    m.reflectance = d.reflectance;
    m.metalnessMax = d.metalnessMax;
    m.metalnessMin = d.metalnessMin;

    m.normalMapStrength = d.normalMapStrength;
    m.parallaxOcclusionMapping = d.parallaxOcclusionMapping;
    m.alphaRef = d.alphaRef;
    m.saturation = d.saturation;
    m.SetUserStencilRef(d.userStencilRef);

    m.texAnimDirection.x = d.texAnimDirection[0];
    m.texAnimDirection.y = d.texAnimDirection[1];
    m.texAnimFrameRate = d.texAnimFrameRate;
    m.texAnimElapsedTime = d.texAnimElapsedTime;

    for (size_t i = 0; i < MaterialComponentData::TEXTURESLOT_COUNT; ++i)
    {
        m.textures[i].name = d.textures[i].name;
        m.textures[i].uvset = d.textures[i].uvset;
        m.textures[i].descriptorIndex = -1;
    }

    m.useCustomShader    = d.useCustomShader;
    m.customShaderPath   = d.customShaderPath;
    m.customShadingModel = static_cast<ShadingModel>(d.customShadingModel);
    m.customShaderID     = -1;          // re-resolved at first GBuffer pass tick
    m.customTextures.clear();
    for (const auto& [name, tex] : d.customTextures)
    {
        MaterialComponent::TextureMap t;
        t.name  = tex.name;
        t.uvset = tex.uvset;
        // descriptorIndex / gpuHandle / bindlessIndex start unresolved — the
        // resource loader fills them when the texture actually loads.
        m.customTextures.emplace(name, std::move(t));
    }
    m.customParams.clear();
    for (const auto& [name, v] : d.customParams)
        m.customParams.emplace(name, v);
    m.layerMask = d.layerMask;
    m.samplerDescriptor = -1;
    m.cameraSource = d.cameraSource;
    m.outlinePixels = d.outlinePixels;
}

// ---------------------------------------------------------------------------
// MeshComponent <-> MeshComponentData
// ---------------------------------------------------------------------------

MeshComponentData FromMeshComponent(const MeshComponent& m)
{
    MeshComponentData d;
    d._flags = m._flags;
    d.vertex_positions = m.vertex_positions;
    d.vertex_normals = m.vertex_normals;
    d.vertex_tangents = m.vertex_tangents;
    d.vertex_uvset_0 = m.vertex_uvset_0;
    d.vertex_uvset_1 = m.vertex_uvset_1;
    d.vertex_boneindices = m.vertex_boneindices;
    d.vertex_boneweights = m.vertex_boneweights;
    d.vertex_colors = m.vertex_colors;
    d.indices = m.indices;

    for (const auto& s : m.subsets)
    {
        MeshSubsetData sd;
        sd.surfaceName = s.surfaceName;
        sd.materialID = s.materialID;
        sd.indexOffset = s.indexOffset;
        sd.indexCount = s.indexCount;
        sd.materialIndex = s.materialIndex;
        sd.flags = s.flags;
        d.subsets.push_back(sd);
    }

    d.armatureID = m.armatureID;
    d.tessellationFactor = m.tessellationFactor;
    d.subsets_per_lod = m.subsets_per_lod;

    d.aabb_min[0] = m.aabb._min.x;
    d.aabb_min[1] = m.aabb._min.y;
    d.aabb_min[2] = m.aabb._min.z;
    d.aabb_max[0] = m.aabb._max.x;
    d.aabb_max[1] = m.aabb._max.y;
    d.aabb_max[2] = m.aabb._max.z;
    return d;
}

void ToMeshComponent(const MeshComponentData& d, MeshComponent& m)
{
    m._flags = d._flags;
    m.vertex_positions = d.vertex_positions;
    m.vertex_normals = d.vertex_normals;
    m.vertex_tangents = d.vertex_tangents;
    m.vertex_uvset_0 = d.vertex_uvset_0;
    m.vertex_uvset_1 = d.vertex_uvset_1;
    m.vertex_boneindices = d.vertex_boneindices;
    m.vertex_boneweights = d.vertex_boneweights;
    m.vertex_colors = d.vertex_colors;
    m.indices = d.indices;

    m.subsets.clear();
    for (const auto& sd : d.subsets)
    {
        MeshComponent::MeshSubset s;
        s.surfaceName = sd.surfaceName;
        s.materialID = sd.materialID;
        s.indexOffset = sd.indexOffset;
        s.indexCount = sd.indexCount;
        s.materialIndex = sd.materialIndex;
        s.flags = sd.flags;
        m.subsets.push_back(s);
    }

    m.armatureID = d.armatureID;
    m.tessellationFactor = d.tessellationFactor;
    m.subsets_per_lod = d.subsets_per_lod;

    m.aabb._min.x = d.aabb_min[0];
    m.aabb._min.y = d.aabb_min[1];
    m.aabb._min.z = d.aabb_min[2];
    m.aabb._max.x = d.aabb_max[0];
    m.aabb._max.y = d.aabb_max[1];
    m.aabb._max.z = d.aabb_max[2];

    m.ib.offset = ~0ull;
    m.vb_pos.offset = ~0ull;
    m.m_vertexBufferResource.Reset();
    m.m_indexBufferResource.Reset();
}

// ---------------------------------------------------------------------------
// Binary serialization
// ---------------------------------------------------------------------------

static void WriteMaterialData(std::ostream& out, const MaterialComponentData& m)
{
    Write(out, m._flags);
    Write(out, m.shaderType);
    Write(out, m.engineStencilRef);
    Write(out, m.userBlendMode);
    out.write(reinterpret_cast<const char*>(m.baseColor), sizeof(m.baseColor));
    out.write(reinterpret_cast<const char*>(m.specularColor), sizeof(m.specularColor));
    out.write(reinterpret_cast<const char*>(m.emissiveColor), sizeof(m.emissiveColor));
    out.write(reinterpret_cast<const char*>(m.texMulAdd), sizeof(m.texMulAdd));
    Write(out, m.roughnessMax);
    Write(out, m.roughnessMin);

    Write(out, m.reflectance);
    Write(out, m.metalnessMax);
    Write(out, m.metalnessMin);

    Write(out, m.normalMapStrength);
    Write(out, m.parallaxOcclusionMapping);
    Write(out, m.alphaRef);
    Write(out, m.saturation);
    Write(out, m.userStencilRef);
    out.write(reinterpret_cast<const char*>(m.texAnimDirection), sizeof(m.texAnimDirection));
    Write(out, m.texAnimFrameRate);
    Write(out, m.texAnimElapsedTime);
    for (size_t i = 0; i < MaterialComponentData::TEXTURESLOT_COUNT; ++i)
    {
        WriteString(out, m.textures[i].name);
        Write(out, m.textures[i].uvset);
    }
    Write(out, m.useCustomShader);
    WriteString(out, m.customShaderPath);
    Write(out, m.customShadingModel);
    // Custom textures: count + (key, name, uvset) tuples.
    {
        const uint32_t n = static_cast<uint32_t>(m.customTextures.size());
        Write(out, n);
        for (const auto& [key, tex] : m.customTextures)
        {
            WriteString(out, key);
            WriteString(out, tex.name);
            Write(out, tex.uvset);
        }
    }
    // Custom params: count + (key, 4 floats).
    {
        const uint32_t n = static_cast<uint32_t>(m.customParams.size());
        Write(out, n);
        for (const auto& [key, v] : m.customParams)
        {
            WriteString(out, key);
            out.write(reinterpret_cast<const char*>(v.data()), sizeof(v));
        }
    }
    Write(out, m.layerMask);
    Write(out, m.cameraSource);
    Write(out, m.outlinePixels);
}

static bool ReadMaterialData(std::istream& in, MaterialComponentData& m)
{
    if (!Read(in, m._flags) || !Read(in, m.shaderType) || !Read(in, m.engineStencilRef) || !Read(in, m.userBlendMode))
        return false;
    if (!in.read(reinterpret_cast<char*>(m.baseColor), sizeof(m.baseColor)).good()) return false;
    if (!in.read(reinterpret_cast<char*>(m.specularColor), sizeof(m.specularColor)).good()) return false;
    if (!in.read(reinterpret_cast<char*>(m.emissiveColor), sizeof(m.emissiveColor)).good()) return false;
    if (!in.read(reinterpret_cast<char*>(m.texMulAdd), sizeof(m.texMulAdd)).good()) return false;
    if (!Read(in, m.roughnessMax) || !Read(in, m.roughnessMin) || !Read(in, m.reflectance) || !Read(in, m.metalnessMax) || !Read(in, m.metalnessMin) || !Read(in, m.normalMapStrength) ||
        !Read(in, m.parallaxOcclusionMapping) || !Read(in, m.alphaRef) || !Read(in, m.saturation) || !Read(in, m.userStencilRef))
        return false;
    if (!in.read(reinterpret_cast<char*>(m.texAnimDirection), sizeof(m.texAnimDirection)).good()) return false;
    if (!Read(in, m.texAnimFrameRate) || !Read(in, m.texAnimElapsedTime))
        return false;
    for (size_t i = 0; i < MaterialComponentData::TEXTURESLOT_COUNT; ++i)
    {
        if (!ReadString(in, m.textures[i].name) || !Read(in, m.textures[i].uvset))
            return false;
    }
    if (!Read(in, m.useCustomShader)
        || !ReadString(in, m.customShaderPath)
        || !Read(in, m.customShadingModel))
        return false;

    {
        uint32_t n = 0;
        if (!Read(in, n)) return false;
        m.customTextures.clear();
        m.customTextures.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            std::string key;
            MaterialComponentData::CustomTextureData tex;
            if (!ReadString(in, key) || !ReadString(in, tex.name) || !Read(in, tex.uvset))
                return false;
            m.customTextures.emplace_back(std::move(key), std::move(tex));
        }
    }
    {
        uint32_t n = 0;
        if (!Read(in, n)) return false;
        m.customParams.clear();
        m.customParams.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            std::string key;
            std::array<float, 4> v{};
            if (!ReadString(in, key)) return false;
            if (!in.read(reinterpret_cast<char*>(v.data()), sizeof(v)).good()) return false;
            m.customParams.emplace_back(std::move(key), v);
        }
    }
    return Read(in, m.layerMask)
        && Read(in, m.cameraSource)
        && Read(in, m.outlinePixels);
}

static void WriteMeshData(std::ostream& out, const MeshComponentData& m)
{
    Write(out, m._flags);
    WriteVector(out, m.vertex_positions, WriteFloat3);
    WriteVector(out, m.vertex_normals, WriteFloat3);
    WriteVector(out, m.vertex_tangents, WriteFloat4);
    WriteVector(out, m.vertex_uvset_0, [](std::ostream& o, const DirectX::XMFLOAT2& v) { Write(o, v.x); Write(o, v.y); });
    WriteVector(out, m.vertex_uvset_1, [](std::ostream& o, const DirectX::XMFLOAT2& v) { Write(o, v.x); Write(o, v.y); });
    WriteVector(out, m.vertex_boneindices, WriteUint4);
    WriteVector(out, m.vertex_boneweights, WriteFloat4);
    WriteVector(out, m.vertex_colors, [](std::ostream& o, uint32_t v) { Write(o, v); });
    WriteVector(out, m.indices, [](std::ostream& o, uint32_t v) { Write(o, v); });

    uint32_t nSub = static_cast<uint32_t>(m.subsets.size());
    Write(out, nSub);
    for (const auto& s : m.subsets)
    {
        WriteString(out, s.surfaceName);
        Write(out, s.materialID);
        Write(out, s.indexOffset);
        Write(out, s.indexCount);
        Write(out, s.materialIndex);
        Write(out, s.flags);
    }

    Write(out, m.armatureID);
    Write(out, m.tessellationFactor);
    Write(out, m.subsets_per_lod);
    out.write(reinterpret_cast<const char*>(m.aabb_min), sizeof(m.aabb_min));
    out.write(reinterpret_cast<const char*>(m.aabb_max), sizeof(m.aabb_max));
}

static bool ReadMeshData(std::istream& in, MeshComponentData& m)
{
    if (!Read(in, m._flags))
        return false;
    auto readFloat2 = [](std::istream& i, DirectX::XMFLOAT2& v) { return Read(i, v.x) && Read(i, v.y); };
    if (!ReadVector(in, m.vertex_positions, ReadFloat3)) return false;
    if (!ReadVector(in, m.vertex_normals, ReadFloat3)) return false;
    if (!ReadVector(in, m.vertex_tangents, ReadFloat4)) return false;
    if (!ReadVector(in, m.vertex_uvset_0, readFloat2)) return false;
    if (!ReadVector(in, m.vertex_uvset_1, readFloat2)) return false;
    if (!ReadVector(in, m.vertex_boneindices, ReadUint4)) return false;
    if (!ReadVector(in, m.vertex_boneweights, ReadFloat4)) return false;
    if (!ReadVector(in, m.vertex_colors, [](std::istream& i, uint32_t& v) { return Read(i, v); })) return false;
    if (!ReadVector(in, m.indices, [](std::istream& i, uint32_t& v) { return Read(i, v); })) return false;

    uint32_t nSub = 0;
    if (!Read(in, nSub))
        return false;
    m.subsets.resize(nSub);
    for (auto& s : m.subsets)
    {
        if (!ReadString(in, s.surfaceName) || !Read(in, s.materialID) || !Read(in, s.indexOffset) ||
            !Read(in, s.indexCount) || !Read(in, s.materialIndex) || !Read(in, s.flags))
            return false;
    }

    if (!Read(in, m.armatureID) || !Read(in, m.tessellationFactor) || !Read(in, m.subsets_per_lod))
        return false;
    if (!in.read(reinterpret_cast<char*>(m.aabb_min), sizeof(m.aabb_min)).good()) return false;
    if (!in.read(reinterpret_cast<char*>(m.aabb_max), sizeof(m.aabb_max)).good()) return false;
    return true;
}

bool SerializeToBinary(const SceneData& data, const char* filepath)
{
    std::ofstream out(filepath, std::ios::binary);
    if (!out)
    {
        LOG_ERROR("SceneSerialization: failed to open for write: %s", filepath);
        return false;
    }
    Write(out, kBinaryMagic);
    Write(out, kBinaryVersion);
    WriteString(out, data.name);
    uint32_t nEnt = static_cast<uint32_t>(data.entities.size());
    Write(out, nEnt);

    for (const auto& e : data.entities)
    {
        Write(out, e.entity);
        WriteString(out, e.name);
        Write(out, e.hasMaterial);
        Write(out, e.hasMesh);
        if (e.hasMaterial)
            WriteMaterialData(out, e.material);
        if (e.hasMesh)
            WriteMeshData(out, e.mesh);
    }
    return out.good();
}

bool DeserializeFromBinary(const char* filepath, SceneData& out)
{
    std::ifstream in(filepath, std::ios::binary);
    if (!in)
    {
        LOG_ERROR("SceneSerialization: failed to open for read: %s", filepath);
        return false;
    }
    uint32_t magic = 0, version = 0;
    if (!Read(in, magic) || magic != kBinaryMagic)
    {
        LOG_ERROR("SceneSerialization: invalid binary magic");
        return false;
    }
    if (!Read(in, version) || version != kBinaryVersion)
    {
        LOG_ERROR("SceneSerialization: unsupported binary version");
        return false;
    }
    if (!ReadString(in, out.name))
        return false;
    uint32_t nEnt = 0;
    if (!Read(in, nEnt))
        return false;
    out.entities.resize(nEnt);

    for (auto& e : out.entities)
    {
        if (!Read(in, e.entity) || !ReadString(in, e.name) || !Read(in, e.hasMaterial) || !Read(in, e.hasMesh))
            return false;
        if (e.hasMaterial && !ReadMaterialData(in, e.material))
            return false;
        if (e.hasMesh && !ReadMeshData(in, e.mesh))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON serialization (nlohmann)
// ---------------------------------------------------------------------------

using Json = nlohmann::json;

static Json ToJson(const MaterialComponentData& m)
{
    Json j;
    j["_flags"] = m._flags;
    j["shaderType"] = m.shaderType;
    j["engineStencilRef"] = m.engineStencilRef;
    j["userBlendMode"] = m.userBlendMode;
    j["baseColor"] = { m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3] };
    j["specularColor"] = { m.specularColor[0], m.specularColor[1], m.specularColor[2], m.specularColor[3] };
    j["emissiveColor"] = { m.emissiveColor[0], m.emissiveColor[1], m.emissiveColor[2], m.emissiveColor[3] };
    j["texMulAdd"] = { m.texMulAdd[0], m.texMulAdd[1], m.texMulAdd[2], m.texMulAdd[3] };
    j["roughnessMax"] = m.roughnessMax;
    j["roughnessMin"] = m.roughnessMin;
    j["reflectance"] = m.reflectance;
    j["metalnessMax"] = m.metalnessMax;
    j["metalnessMin"] = m.metalnessMin;
    j["normalMapStrength"] = m.normalMapStrength;
    j["parallaxOcclusionMapping"] = m.parallaxOcclusionMapping;
    j["alphaRef"] = m.alphaRef;
    j["saturation"] = m.saturation;
    j["userStencilRef"] = m.userStencilRef;
    j["texAnimDirection"] = { m.texAnimDirection[0], m.texAnimDirection[1] };
    j["texAnimFrameRate"] = m.texAnimFrameRate;
    j["texAnimElapsedTime"] = m.texAnimElapsedTime;
    j["textures"] = Json::array();
    for (size_t i = 0; i < MaterialComponentData::TEXTURESLOT_COUNT; ++i)
    {
        Json t;
        t["name"] = m.textures[i].name;
        t["uvset"] = m.textures[i].uvset;
        j["textures"].push_back(t);
    }
    j["useCustomShader"]    = m.useCustomShader;
    j["customShaderPath"]   = m.customShaderPath;
    j["customShadingModel"] = m.customShadingModel;
    {
        Json arr = Json::array();
        for (const auto& [key, tex] : m.customTextures)
        {
            Json e;
            e["key"]   = key;
            e["name"]  = tex.name;
            e["uvset"] = tex.uvset;
            arr.push_back(std::move(e));
        }
        j["customTextures"] = std::move(arr);
    }
    {
        Json arr = Json::array();
        for (const auto& [key, v] : m.customParams)
        {
            Json e;
            e["key"] = key;
            e["v"]   = { v[0], v[1], v[2], v[3] };
            arr.push_back(std::move(e));
        }
        j["customParams"] = std::move(arr);
    }
    j["layerMask"] = m.layerMask;
    j["cameraSource"] = m.cameraSource;
    j["outlinePixels"] = m.outlinePixels;
    return j;
}

static bool FromJson(const Json& j, MaterialComponentData& m)
{
    if (!j.contains("_flags")) return false;
    m._flags = j["_flags"].get<uint32_t>();
    m.shaderType = j.value("shaderType", 0u);
    m.engineStencilRef = j.value("engineStencilRef", 0u);
    m.userBlendMode = j.value("userBlendMode", 0u);
    auto bc = j["baseColor"];
    for (int i = 0; i < 4; ++i) m.baseColor[i] = bc[i].get<float>();
    auto sc = j["specularColor"];
    for (int i = 0; i < 4; ++i) m.specularColor[i] = sc[i].get<float>();
    auto ec = j["emissiveColor"];
    for (int i = 0; i < 4; ++i) m.emissiveColor[i] = ec[i].get<float>();
    auto ta = j["texMulAdd"];
    for (int i = 0; i < 4; ++i) m.texMulAdd[i] = ta[i].get<float>();
    m.roughnessMax = j.value("roughnessMax", 1.0f);
    m.roughnessMin = j.value("roughnessMin", 0.0f);

    m.reflectance = j.value("reflectance", 0.5f);
    m.metalnessMax = j.value("metalnessMax", 1.0f);
    m.metalnessMin = j.value("metalnessMin", 0.0f);

    m.normalMapStrength = j.value("normalMapStrength", 1.0f);
    m.parallaxOcclusionMapping = j.value("parallaxOcclusionMapping", 0.0f);
    m.alphaRef = j.value("alphaRef", 1.0f);
    m.saturation = j.value("saturation", 1.0f);
    m.userStencilRef = j.value("userStencilRef", static_cast<uint8_t>(0));
    if (j.contains("texAnimDirection"))
    {
        m.texAnimDirection[0] = j["texAnimDirection"][0].get<float>();
        m.texAnimDirection[1] = j["texAnimDirection"][1].get<float>();
    }
    m.texAnimFrameRate = j.value("texAnimFrameRate", 0.0f);
    m.texAnimElapsedTime = j.value("texAnimElapsedTime", 0.0f);
    if (j.contains("textures"))
        for (size_t i = 0; i < (std::min)(j["textures"].size(), static_cast<size_t>(MaterialComponentData::TEXTURESLOT_COUNT)); ++i)
        {
            m.textures[i].name = j["textures"][i].value("name", "");
            m.textures[i].uvset = j["textures"][i].value("uvset", 0u);
        }
    m.useCustomShader    = j.value("useCustomShader", false);
    m.customShaderPath   = j.value("customShaderPath", std::string{});
    m.customShadingModel = j.value("customShadingModel", static_cast<uint8_t>(0));
    if (j.contains("customTextures"))
    {
        for (const auto& e : j["customTextures"])
        {
            MaterialComponentData::CustomTextureData tex;
            tex.name  = e.value("name",  std::string{});
            tex.uvset = e.value("uvset", 0u);
            m.customTextures.emplace_back(e.value("key", std::string{}), std::move(tex));
        }
    }
    if (j.contains("customParams"))
    {
        for (const auto& e : j["customParams"])
        {
            std::array<float, 4> v{};
            if (e.contains("v") && e["v"].is_array())
                for (size_t i = 0; i < (std::min<size_t>)(e["v"].size(), 4); ++i)
                    v[i] = e["v"][i].get<float>();
            m.customParams.emplace_back(e.value("key", std::string{}), v);
        }
    }
    m.layerMask = j.value("layerMask", ~0u);
    m.cameraSource = j.value("cameraSource", NullEntity);
    m.outlinePixels = j.value("outlinePixels", 2.0f);
    return true;
}

static Json ToJson(const MeshSubsetData& s)
{
    Json j;
    j["surfaceName"] = s.surfaceName;
    j["materialID"] = s.materialID;
    j["indexOffset"] = s.indexOffset;
    j["indexCount"] = s.indexCount;
    j["materialIndex"] = s.materialIndex;
    j["flags"] = s.flags;
    return j;
}

static void FromJson(const Json& j, MeshSubsetData& s)
{
    s.surfaceName = j.value("surfaceName", "");
    s.materialID = j.value("materialID", NullEntity);
    s.indexOffset = j.value("indexOffset", 0u);
    s.indexCount = j.value("indexCount", 0u);
    s.materialIndex = j.value("materialIndex", 0u);
    s.flags = j.value("flags", 0u);
}

static Json ToJson(const MeshComponentData& m)
{
    Json j;
    j["_flags"] = m._flags;
    auto vec3 = [](const DirectX::XMFLOAT3& v) { return Json::array({ v.x, v.y, v.z }); };
    auto vec4 = [](const DirectX::XMFLOAT4& v) { return Json::array({ v.x, v.y, v.z, v.w }); };
    auto vec2 = [](const DirectX::XMFLOAT2& v) { return Json::array({ v.x, v.y }); };
    auto vecu4 = [](const DirectX::XMUINT4& v) { return Json::array({ v.x, v.y, v.z, v.w }); };

    j["vertex_positions"] = Json::array();
    for (const auto& v : m.vertex_positions) j["vertex_positions"].push_back(vec3(v));
    j["vertex_normals"] = Json::array();
    for (const auto& v : m.vertex_normals) j["vertex_normals"].push_back(vec3(v));
    j["vertex_tangents"] = Json::array();
    for (const auto& v : m.vertex_tangents) j["vertex_tangents"].push_back(vec4(v));
    j["vertex_uvset_0"] = Json::array();
    for (const auto& v : m.vertex_uvset_0) j["vertex_uvset_0"].push_back(vec2(v));
    j["vertex_uvset_1"] = Json::array();
    for (const auto& v : m.vertex_uvset_1) j["vertex_uvset_1"].push_back(vec2(v));
    j["vertex_boneindices"] = Json::array();
    for (const auto& v : m.vertex_boneindices) j["vertex_boneindices"].push_back(vecu4(v));
    j["vertex_boneweights"] = Json::array();
    for (const auto& v : m.vertex_boneweights) j["vertex_boneweights"].push_back(vec4(v));
    j["vertex_colors"] = m.vertex_colors;
    j["indices"] = m.indices;

    j["subsets"] = Json::array();
    for (const auto& s : m.subsets) j["subsets"].push_back(ToJson(s));

    j["armatureID"] = m.armatureID;
    j["tessellationFactor"] = m.tessellationFactor;
    j["subsets_per_lod"] = m.subsets_per_lod;
    j["aabb_min"] = { m.aabb_min[0], m.aabb_min[1], m.aabb_min[2] };
    j["aabb_max"] = { m.aabb_max[0], m.aabb_max[1], m.aabb_max[2] };
    return j;
}

static bool FromJson(const Json& j, MeshComponentData& m)
{
    m._flags = j.value("_flags", 0u);
    auto loadVec3 = [](const Json& arr, std::vector<DirectX::XMFLOAT3>& out) {
        out.clear();
        for (const auto& a : arr)
            out.push_back({ a[0].get<float>(), a[1].get<float>(), a[2].get<float>() });
    };
    auto loadVec4 = [](const Json& arr, std::vector<DirectX::XMFLOAT4>& out) {
        out.clear();
        for (const auto& a : arr)
            out.push_back({ a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>() });
    };
    auto loadVec2 = [](const Json& arr, std::vector<DirectX::XMFLOAT2>& out) {
        out.clear();
        for (const auto& a : arr)
            out.push_back({ a[0].get<float>(), a[1].get<float>() });
    };
    auto loadUint4 = [](const Json& arr, std::vector<DirectX::XMUINT4>& out) {
        out.clear();
        for (const auto& a : arr)
            out.push_back({ a[0].get<uint32_t>(), a[1].get<uint32_t>(), a[2].get<uint32_t>(), a[3].get<uint32_t>() });
    };

    if (j.contains("vertex_positions")) loadVec3(j["vertex_positions"], m.vertex_positions);
    if (j.contains("vertex_normals")) loadVec3(j["vertex_normals"], m.vertex_normals);
    if (j.contains("vertex_tangents")) loadVec4(j["vertex_tangents"], m.vertex_tangents);
    if (j.contains("vertex_uvset_0")) loadVec2(j["vertex_uvset_0"], m.vertex_uvset_0);
    if (j.contains("vertex_uvset_1")) loadVec2(j["vertex_uvset_1"], m.vertex_uvset_1);
    if (j.contains("vertex_boneindices")) loadUint4(j["vertex_boneindices"], m.vertex_boneindices);
    if (j.contains("vertex_boneweights")) loadVec4(j["vertex_boneweights"], m.vertex_boneweights);
    if (j.contains("vertex_colors")) m.vertex_colors = j["vertex_colors"].get<std::vector<uint32_t>>();
    if (j.contains("indices")) m.indices = j["indices"].get<std::vector<uint32_t>>();

    m.subsets.clear();
    if (j.contains("subsets"))
        for (const auto& s : j["subsets"])
        {
            MeshSubsetData sd;
            FromJson(s, sd);
            m.subsets.push_back(sd);
        }

    m.armatureID = j.value("armatureID", NullEntity);
    m.tessellationFactor = j.value("tessellationFactor", 0.0f);
    m.subsets_per_lod = j.value("subsets_per_lod", 0u);
    if (j.contains("aabb_min"))
        for (int i = 0; i < 3; ++i) m.aabb_min[i] = j["aabb_min"][i].get<float>();
    if (j.contains("aabb_max"))
        for (int i = 0; i < 3; ++i) m.aabb_max[i] = j["aabb_max"][i].get<float>();
    return true;
}

bool SerializeToJSON(const SceneData& data, const char* filepath)
{
    Json root;
    root["name"] = data.name;
    root["version"] = static_cast<int>(kBinaryVersion);
    root["entities"] = Json::array();
    for (const auto& e : data.entities)
    {
        Json ent;
        ent["entity"] = e.entity;
        ent["name"] = e.name;
        ent["hasMaterial"] = e.hasMaterial;
        ent["hasMesh"] = e.hasMesh;
        if (e.hasMaterial)
            ent["material"] = ToJson(e.material);
        if (e.hasMesh)
            ent["mesh"] = ToJson(e.mesh);
        root["entities"].push_back(ent);
    }

    std::ofstream out(filepath);
    if (!out)
    {
        LOG_ERROR("SceneSerialization: failed to open JSON for write: %s", filepath);
        return false;
    }
    out << root.dump(2);
    return out.good();
}

bool DeserializeFromJSON(const char* filepath, SceneData& out)
{
    std::ifstream in(filepath);
    if (!in)
    {
        LOG_ERROR("SceneSerialization: failed to open JSON for read: %s", filepath);
        return false;
    }
    Json root;
    try
    {
        in >> root;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("SceneSerialization: JSON parse error: %s", e.what());
        return false;
    }
    out.name = root.value("name", "");
    out.entities.clear();
    if (!root.contains("entities"))
        return true;
    for (const auto& ent : root["entities"])
    {
        EntitySceneData e;
        e.entity = ent.value("entity", NullEntity);
        e.name = ent.value("name", "");
        e.hasMaterial = ent.value("hasMaterial", false);
        e.hasMesh = ent.value("hasMesh", false);
        if (e.hasMaterial && ent.contains("material"))
            FromJson(ent["material"], e.material);
        if (e.hasMesh && ent.contains("mesh"))
            FromJson(ent["mesh"], e.mesh);
        out.entities.push_back(e);
    }
    return true;
}

void SerializeToBoth(const SceneData& data, const char* binaryPath, const char* jsonPath)
{
    SerializeToBinary(data, binaryPath);
    SerializeToJSON(data, jsonPath);
}
