#pragma once

// Scene + Components 序列化／反序列化，同時支援 Binary 與 JSON 輸出。
// 使用方式：將 ECS World 的 entity + Material/Mesh 組裝成 SceneData，再呼叫 SerializeToBoth；
// 或從 DeserializeFromBinary/DeserializeFromJSON 得到 SceneData 後，再寫回 World + components。

#include "ECS/ECS.h"
#include "ECS/Components.h"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 可序列化的 Component 資料（無 D3D12、ComPtr、descriptor 等執行期資源）
// ---------------------------------------------------------------------------

struct MaterialComponentData
{
    uint32_t _flags = 0;
    uint32_t shaderType = 0;
    uint32_t engineStencilRef = 0;
    uint32_t userBlendMode = 0;

    float baseColor[4] = { 1, 1, 1, 1 };
    float specularColor[4] = { 1, 1, 1, 1 };
    float emissiveColor[4] = { 1, 1, 1, 0 };
    float texMulAdd[4] = { 1, 1, 0, 0 };
    float roughnessMax = 1.0f;
    float roughnessMin = 0.0f;

    float reflectance = 0.02f;
    float metalnessMax = 1.0f;
    float metalnessMin = 0.0f;

    float normalMapStrength = 1.0f;
    float parallaxOcclusionMapping = 0.0f;
    float alphaRef = 1.0f;
    float saturation = 1.0f;
    uint8_t userStencilRef = 0;

    float texAnimDirection[2] = { 0, 0 };
    float texAnimFrameRate = 0.0f;
    float texAnimElapsedTime = 0.0f;

    float outlinePixels = 2.0f;

    struct TextureMapData
    {
        std::string name;
        uint32_t uvset = 0;
    };
    static constexpr size_t TEXTURESLOT_COUNT = 5;
    TextureMapData textures[TEXTURESLOT_COUNT];

    // Custom GBuffer PS — see MaterialComponent comment block. Authoritative
    // identity is the path; the runtime int handle is intentionally NOT
    // serialized (re-resolved on load).
    bool         useCustomShader     = false;
    std::string  customShaderPath;
    uint8_t      customShadingModel  = 0;   // ShadingModel::Standard

    // Reflection-driven parameter stores. Keys are the shader binding / CB
    // variable names; values mirror MaterialComponent::customTextures /
    // customParams but strip the runtime descriptor handles.
    struct CustomTextureData { std::string name; uint32_t uvset = 0; };
    std::vector<std::pair<std::string, CustomTextureData>>              customTextures;
    std::vector<std::pair<std::string, std::array<float, 4>>>           customParams;

    uint32_t layerMask = ~0u;
    Entity cameraSource = NullEntity;
};

struct MeshSubsetData
{
    std::string surfaceName;
    Entity materialID = NullEntity;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    uint32_t flags = 0;
};

struct MeshComponentData
{
    uint32_t _flags = 0;
    std::vector<DirectX::XMFLOAT3> vertex_positions;
    std::vector<DirectX::XMFLOAT3> vertex_normals;
    std::vector<DirectX::XMFLOAT4> vertex_tangents;
    std::vector<DirectX::XMFLOAT2> vertex_uvset_0;
    std::vector<DirectX::XMFLOAT2> vertex_uvset_1;
    std::vector<DirectX::XMUINT4> vertex_boneindices;
    std::vector<DirectX::XMFLOAT4> vertex_boneweights;
    std::vector<uint32_t> vertex_colors;
    std::vector<uint32_t> indices;

    std::vector<MeshSubsetData> subsets;

    Entity armatureID = NullEntity;
    float tessellationFactor = 0.0f;
    uint32_t subsets_per_lod = 0;

    float aabb_min[3] = { 0, 0, 0 };
    float aabb_max[3] = { 0, 0, 0 };
};

// 單一 Entity 的序列化資料（含 name 與可選的 Material / Mesh）
struct EntitySceneData
{
    Entity entity = NullEntity;
    std::string name;
    bool hasMaterial = false;
    bool hasMesh = false;
    MaterialComponentData material;
    MeshComponentData mesh;
};

// 整個場景的序列化資料
struct SceneData
{
    std::string name;
    std::vector<EntitySceneData> entities;
};

// ---------------------------------------------------------------------------
// 與現有 Component 互轉（用於從 ECS 組裝 SceneData，或從 SceneData 寫回 ECS）
// ---------------------------------------------------------------------------

MaterialComponentData FromMaterialComponent(const MaterialComponent& m);
void ToMaterialComponent(const MaterialComponentData& d, MaterialComponent& m);

MeshComponentData FromMeshComponent(const MeshComponent& m);
void ToMeshComponent(const MeshComponentData& d, MeshComponent& m);

// ---------------------------------------------------------------------------
// 序列化：輸出到單一格式
// ---------------------------------------------------------------------------

/** 將 SceneData 寫成二進位檔。 */
bool SerializeToBinary(const SceneData& data, const char* filepath);

/** 將 SceneData 寫成 JSON 檔（可讀）。 */
bool SerializeToJSON(const SceneData& data, const char* filepath);

/** 同時寫出 Binary 與 JSON（同一份資料、兩種格式）。 */
void SerializeToBoth(const SceneData& data, const char* binaryPath, const char* jsonPath);

// ---------------------------------------------------------------------------
// 反序列化：從單一格式讀入
// ---------------------------------------------------------------------------

/** 從二進位檔讀出 SceneData。 */
bool DeserializeFromBinary(const char* filepath, SceneData& out);

/** 從 JSON 檔讀出 SceneData。 */
bool DeserializeFromJSON(const char* filepath, SceneData& out);
