#include "ECS/Components.h"
#include "Graphics/GraphicsDX12.h"
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// MaterialComponent
// ---------------------------------------------------------------------------

uint32_t MaterialComponent::GetStencilRef() const
{
    if (engineStencilRef == StencilRef::Custom)
        return static_cast<uint32_t>(userStencilRef);
    return 0u;
}

BlendMode MaterialComponent::GetBlendMode() const
{
    return userBlendMode;
}

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------

uint32_t MeshComponent::GetLODCount() const
{
    return subsets_per_lod == 0 ? 1u : (static_cast<uint32_t>(subsets.size()) / subsets_per_lod);
}

void MeshComponent::GetLODSubsetRange(uint32_t lod, uint32_t& first_subset, uint32_t& last_subset) const
{
    first_subset = 0;
    last_subset = static_cast<uint32_t>(subsets.size());
    if (subsets_per_lod > 0)
    {
        uint32_t lodCount = GetLODCount();
        lod = (lod >= lodCount) ? (lodCount - 1) : lod;
        first_subset = subsets_per_lod * lod;
        last_subset = first_subset + subsets_per_lod;
    }
}

static void ComputeAABBFromPositions(
    const std::vector<DirectX::XMFLOAT3>& positions,
    AABB& outAABB)
{
    if (positions.empty())
    {
        outAABB._min = outAABB._max = DirectX::XMFLOAT3(0, 0, 0);
        return;
    }
    float minX = positions[0].x, minY = positions[0].y, minZ = positions[0].z;
    float maxX = minX, maxY = minY, maxZ = minZ;
    for (size_t i = 1; i < positions.size(); ++i)
    {
        const auto& p = positions[i];
        minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
        minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y);
        minZ = (std::min)(minZ, p.z); maxZ = (std::max)(maxZ, p.z);
    }
    outAABB._min = DirectX::XMFLOAT3(minX, minY, minZ);
    outAABB._max = DirectX::XMFLOAT3(maxX, maxY, maxZ);
}

void MeshComponent::CreateRenderData(GraphicsDX12* backend)
{
    if (!backend || vertex_positions.empty() || indices.empty())
        return;

    ID3D12Device* device = backend->GetDevice();
    if (!device)
        return;

    DeleteRenderData();

    ComputeAABBFromPositions(vertex_positions, aabb);

    const size_t vertexCount = vertex_positions.size();
    const size_t indexCount = indices.size();

    // Simplified: single vertex format POS(12) + NORMAL(12) + UV(8) = 32 bytes
    struct Vertex
    {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
    };
    const size_t vertexBufferSize = vertexCount * sizeof(Vertex);
    const size_t indexBufferSize = indexCount * sizeof(uint32_t);

    auto newDefaultBuffer = [device](size_t size, ID3D12Resource** outResource) -> bool
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(outResource));
        return SUCCEEDED(hr);
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> vbResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> ibResource;
    if (!newDefaultBuffer(vertexBufferSize, &vbResource) || !newDefaultBuffer(indexBufferSize, &ibResource))
        return;

    std::vector<Vertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i)
    {
        vertices[i].px = vertex_positions[i].x;
        vertices[i].py = vertex_positions[i].y;
        vertices[i].pz = vertex_positions[i].z;
        if (i < vertex_normals.size())
        {
            vertices[i].nx = vertex_normals[i].x;
            vertices[i].ny = vertex_normals[i].y;
            vertices[i].nz = vertex_normals[i].z;
        }
        else
        {
            vertices[i].nx = vertices[i].ny = 0.f;
            vertices[i].nz = 1.f;
        }
        if (i < vertex_uvset_0.size())
        {
            vertices[i].u = vertex_uvset_0[i].x;
            vertices[i].v = vertex_uvset_0[i].y;
        }
        else
        {
            vertices[i].u = vertices[i].v = 0.f;
        }
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    if (SUCCEEDED(vbResource->Map(0, &readRange, &mapped)))
    {
        std::memcpy(mapped, vertices.data(), vertexBufferSize);
        vbResource->Unmap(0, nullptr);
    }
    if (SUCCEEDED(ibResource->Map(0, &readRange, &mapped)))
    {
        std::memcpy(mapped, indices.data(), indexBufferSize);
        ibResource->Unmap(0, nullptr);
    }

    m_vertexBufferResource = vbResource;
    m_indexBufferResource = ibResource;

    m_vertexBufferView.BufferLocation = m_vertexBufferResource->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);

    m_indexBufferView.BufferLocation = m_indexBufferResource->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    vb_pos.offset = 0;
    vb_pos.size = vertexBufferSize;
    ib.offset = 0;
    ib.size = indexBufferSize;
}

void MeshComponent::DeleteRenderData()
{
    m_vertexBufferResource.Reset();
    m_indexBufferResource.Reset();
    m_vertexBufferView = {};
    m_indexBufferView = {};
    ib.offset = ~0ull;
    vb_pos.offset = ~0ull;
}

void MeshComponent::ComputeNormals(bool smooth)
{
    if (vertex_positions.empty() || indices.empty())
        return;

    vertex_normals.resize(vertex_positions.size());
    for (auto& n : vertex_normals)
        n = DirectX::XMFLOAT3(0, 0, 0);

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        if (i0 >= vertex_positions.size() || i1 >= vertex_positions.size() || i2 >= vertex_positions.size())
            continue;

        DirectX::XMVECTOR p0 = DirectX::XMLoadFloat3(&vertex_positions[i0]);
        DirectX::XMVECTOR p1 = DirectX::XMLoadFloat3(&vertex_positions[i1]);
        DirectX::XMVECTOR p2 = DirectX::XMLoadFloat3(&vertex_positions[i2]);
        DirectX::XMVECTOR e1 = DirectX::XMVectorSubtract(p1, p0);
        DirectX::XMVECTOR e2 = DirectX::XMVectorSubtract(p2, p0);
        DirectX::XMVECTOR n = DirectX::XMVector3Cross(e1, e2);

        DirectX::XMFLOAT3 fn;
        DirectX::XMStoreFloat3(&fn, n);
        vertex_normals[i0].x += fn.x; vertex_normals[i0].y += fn.y; vertex_normals[i0].z += fn.z;
        vertex_normals[i1].x += fn.x; vertex_normals[i1].y += fn.y; vertex_normals[i1].z += fn.z;
        vertex_normals[i2].x += fn.x; vertex_normals[i2].y += fn.y; vertex_normals[i2].z += fn.z;
    }

    for (size_t i = 0; i < vertex_normals.size(); ++i)
    {
        DirectX::XMVECTOR n = DirectX::XMLoadFloat3(&vertex_normals[i]);
        n = DirectX::XMVector3Normalize(n);
        DirectX::XMStoreFloat3(&vertex_normals[i], n);
    }
}

void MeshComponent::FlipCulling()
{
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        std::swap(indices[i + 1], indices[i + 2]);
}

void MeshComponent::Recenter()
{
    if (vertex_positions.empty())
        return;

    AABB box;
    ComputeAABBFromPositions(vertex_positions, box);
    float cx = (box._min.x + box._max.x) * 0.5f;
    float cy = (box._min.y + box._max.y) * 0.5f;
    float cz = (box._min.z + box._max.z) * 0.5f;

    for (auto& p : vertex_positions)
    {
        p.x -= cx;
        p.y -= cy;
        p.z -= cz;
    }
    ComputeAABBFromPositions(vertex_positions, aabb);
}
