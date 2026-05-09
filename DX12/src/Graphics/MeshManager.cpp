#include "Graphics/MeshManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/MeshLibrary.h"
#include "System/Log.h"

void MeshManager::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_descHeap.Init(gfx);
}

bool MeshManager::UploadMesh(const ProceduralMesh::MeshData& data, GPUMesh& out)
{
    if (!m_gfx) return false;

    auto makeRaw = [&](const void* rawData, uint64_t size) -> RHI::GPUBuffer
    {
        RHI::GPUBuffer buf;
        RHI::GPUBufferDesc d;
        d.size       = size;
        d.usage      = RHI::Usage::DEFAULT;
        d.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        d.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        m_gfx->CreateBuffer(d, buf, rawData);
        return buf;
    };

    const uint64_t posSz  = data.positions.size() * sizeof(data.positions[0]);
    const uint64_t norSz  = data.normals.size()   * sizeof(data.normals[0]);
    const uint64_t colSz  = data.colors.size()    * sizeof(data.colors[0]);
    const uint64_t idxSz  = data.indices.size()   * sizeof(data.indices[0]);
    const bool hasTangents = !data.tangents.empty();
    const bool hasUVs      = !data.uvs.empty();
    const uint64_t tanSz  = hasTangents ? data.tangents.size() * sizeof(data.tangents[0]) : 0;
    const uint64_t uvSz   = hasUVs      ? data.uvs.size()      * sizeof(data.uvs[0])      : 0;

    out.posBuffer    = makeRaw(data.positions.data(), posSz);
    out.normalBuffer = makeRaw(data.normals.data(),   norSz);
    out.colorBuffer  = makeRaw(data.colors.data(),    colSz);
    out.indexBuffer  = makeRaw(data.indices.data(),   idxSz);
    if (hasTangents) out.tangentBuffer = makeRaw(data.tangents.data(), tanSz);
    if (hasUVs)      out.uvBuffer      = makeRaw(data.uvs.data(),      uvSz);
    out.indexCount   = static_cast<uint32_t>(data.indices.size());

    const uint32_t posIdx = m_descHeap.RegisterBuffer(out.posBuffer);
    const uint32_t norIdx = m_descHeap.RegisterBuffer(out.normalBuffer);
    const uint32_t colIdx = m_descHeap.RegisterBuffer(out.colorBuffer);
    const uint32_t idxIdx = m_descHeap.RegisterBuffer(out.indexBuffer);
    const uint32_t tanIdx = hasTangents ? m_descHeap.RegisterBuffer(out.tangentBuffer)
                                        : RHI::kInvalidBufferIndex;
    const uint32_t uvIdx  = hasUVs      ? m_descHeap.RegisterBuffer(out.uvBuffer)
                                        : RHI::kInvalidBufferIndex;

    if (posIdx == RHI::kInvalidBufferIndex ||
        norIdx == RHI::kInvalidBufferIndex ||
        colIdx == RHI::kInvalidBufferIndex ||
        idxIdx == RHI::kInvalidBufferIndex)
    {
        LOG_ERROR("MeshManager::UploadMesh: bindless table full");
        return false;
    }

    RHI::MeshDescriptor md{};
    md.position.bufferIndex = posIdx;
    md.position.byteOffset  = 0;
    md.position.byteStride  = sizeof(float) * 3;
    md.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.normal.bufferIndex = norIdx;
    md.normal.byteOffset  = 0;
    md.normal.byteStride  = sizeof(float) * 3;
    md.normal.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    if (hasTangents && tanIdx != RHI::kInvalidBufferIndex)
    {
        md.tangent.bufferIndex = tanIdx;
        md.tangent.byteOffset  = 0;
        md.tangent.byteStride  = sizeof(float) * 4;
        md.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }

    if (hasUVs && uvIdx != RHI::kInvalidBufferIndex)
    {
        md.uv0.bufferIndex = uvIdx;
        md.uv0.byteOffset  = 0;
        md.uv0.byteStride  = sizeof(float) * 2;
        md.uv0.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }

    md.uv1.bufferIndex = colIdx;
    md.uv1.byteOffset  = 0;
    md.uv1.byteStride  = sizeof(float) * 3;
    md.uv1.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.indexBufferIndex = idxIdx;
    md.indexByteOffset  = 0;
    md.indexFormat      = 0;
    md.vertexCount      = static_cast<uint32_t>(data.positions.size());

    out.meshDescSlot = m_descHeap.RegisterMesh(md);
    return out.meshDescSlot != RHI::kInvalidBufferIndex;
}

void MeshManager::InitPrimitives()
{
    const ProceduralMesh::MeshData meshes[3] =
    {
        ProceduralMesh::Cube(),
        ProceduralMesh::Sphere(),
        ProceduralMesh::Cone(),
    };
    static const char* primNames[] = { "Cube", "Sphere", "Cone" };

    for (int i = 0; i < 3; ++i)
    {
        if (UploadMesh(meshes[i], m_primitives[i]))
            LOG_SUCCESS("MeshManager: uploaded %s (meshDescSlot=%u, %u indices)",
                        primNames[i], m_primitives[i].meshDescSlot,
                        m_primitives[i].indexCount);
        else
            LOG_ERROR("MeshManager: failed to upload %s", primNames[i]);
    }
}

void MeshManager::InitBillboardQuad()
{
    ProceduralMesh::MeshData quad;
    quad.positions = {
        { -0.5f,  0.5f, 0.f }, {  0.5f,  0.5f, 0.f },
        { -0.5f, -0.5f, 0.f }, {  0.5f, -0.5f, 0.f }
    };
    quad.normals = {
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f },
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }
    };
    quad.uvs = {
        { 0.f, 0.f }, { 1.f, 0.f },
        { 0.f, 1.f }, { 1.f, 1.f }
    };
    quad.tangents = {
        { 1.f, 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f, 1.f },
        { 1.f, 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f, 1.f }
    };
    quad.colors = {
        { 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f },
        { 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f }
    };
    quad.indices = { 0, 1, 2, 2, 1, 3 };

    if (UploadMesh(quad, m_billboardQuad))
    {
        m_billboardMeshDescSlot = m_billboardQuad.meshDescSlot;
        LOG_SUCCESS("MeshManager: billboard quad mesh registered (slot %u)",
                    m_billboardMeshDescSlot);
    }
    else
        LOG_ERROR("MeshManager: failed to create billboard quad mesh");
}

uint32_t MeshManager::RegisterMeshLibMesh(Resource::MeshLibrary& lib,
                                          const MeshLibRef& ref)
{
    if (!ref.IsValid()) return RHI::kInvalidBufferIndex;

    if (ref.cachedGeneration == m_meshLibDescGeneration &&
        ref.cachedDescSlot   != RHI::kInvalidBufferIndex)
        return ref.cachedDescSlot;

    const uint32_t libIdx = ref.libHandle.Index();
    const uint64_t cacheKey = (uint64_t(libIdx) << 32) | uint64_t(ref.meshId);

    auto [descIt, inserted] = m_meshLibDescCache.try_emplace(cacheKey, RHI::kInvalidBufferIndex);
    if (!inserted)
    {
        ref.cachedDescSlot   = descIt->second;
        ref.cachedGeneration = m_meshLibDescGeneration;
        return descIt->second;
    }

    const auto* entry = lib.GetEntry(ref.libHandle, ref.meshId);
    if (!entry) return RHI::kInvalidBufferIndex;

    const RHI::GPUBuffer* vb = lib.GetVertexBuffer(ref.libHandle);
    const RHI::GPUBuffer* ib = lib.GetIndexBuffer (ref.libHandle);
    if (!vb || !ib || !vb->IsValid() || !ib->IsValid())
    {
        LOG_ERROR("MeshManager::RegisterMeshLibMesh: library %u has invalid VB/IB", libIdx);
        return RHI::kInvalidBufferIndex;
    }

    const uint32_t vStride    = lib.GetVertexStride(ref.libHandle);
    const bool     hasTangent = lib.GetHasTangent(ref.libHandle);

    MeshLibBindless& slots = m_meshLibBindless[libIdx];
    if (slots.vbIdx == RHI::kInvalidBufferIndex)
    {
        slots.vbIdx = m_descHeap.RegisterBuffer(*vb);
        slots.ibIdx = m_descHeap.RegisterBuffer(*ib);
        if (slots.vbIdx == RHI::kInvalidBufferIndex || slots.ibIdx == RHI::kInvalidBufferIndex)
        {
            LOG_ERROR("MeshManager::RegisterMeshLibMesh: bindless buffer table full");
            return RHI::kInvalidBufferIndex;
        }
    }

    RHI::MeshDescriptor md{};
    md.position.bufferIndex = slots.vbIdx;
    md.position.byteOffset  = 0;
    md.position.byteStride  = vStride;
    md.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.normal.bufferIndex   = slots.vbIdx;
    md.normal.byteOffset    = 12;
    md.normal.byteStride    = vStride;
    md.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.uv0.bufferIndex      = slots.vbIdx;
    md.uv0.byteOffset       = 24;
    md.uv0.byteStride       = vStride;
    md.uv0.format           = static_cast<uint32_t>(RHI::VertexFormat::Float2);

    // Tangent stream — only present in 48-byte libraries (MESHLIB_FLAG_HAS_TANGENT).
    // Legacy 32-byte libraries leave md.tangent at its default-init INVALID_BUFFER
    // so GBuffer.vs's synthesise-from-N branch handles them.
    if (hasTangent)
    {
        md.tangent.bufferIndex = slots.vbIdx;
        md.tangent.byteOffset  = 32;
        md.tangent.byteStride  = vStride;
        md.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }

    md.indexBufferIndex     = slots.ibIdx;
    md.indexByteOffset      = entry->indexStart * 4u;
    md.indexFormat          = 1;
    md.vertexCount          = entry->vertexCount;

    const uint32_t slot = m_descHeap.RegisterMesh(md);
    if (slot != RHI::kInvalidBufferIndex)
    {
        descIt->second       = slot;
        ref.cachedDescSlot   = slot;
        ref.cachedGeneration = m_meshLibDescGeneration;
    }
    else
    {
        m_meshLibDescCache.erase(descIt);
    }
    return slot;
}

void MeshManager::OnWorldClear()
{
    m_meshLibDescCache.clear();
    m_meshLibBindless.clear();
    ++m_meshLibDescGeneration;
}
