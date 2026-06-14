#include "Graphics/MeshManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"   // ComputeMeshLibVertexLayout / MESHLIB_FLAG_*
#include "System/Log.h"

void MeshManager::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_descHeap.Init(gfx);
}

bool MeshManager::UploadMesh(const ProceduralMesh::MeshData& data, GPUMesh& out, bool persistent)
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
    const uint64_t idxSz  = data.indices.size()   * sizeof(data.indices[0]);
    const bool hasTangents = !data.tangents.empty();
    const bool hasUVs      = !data.uvs.empty();
    const bool hasColors   = !data.colors.empty();
    const uint64_t tanSz  = hasTangents ? data.tangents.size() * sizeof(data.tangents[0]) : 0;
    const uint64_t uvSz   = hasUVs      ? data.uvs.size()      * sizeof(data.uvs[0])      : 0;
    const uint64_t colSz  = hasColors   ? data.colors.size()   * sizeof(data.colors[0])   : 0;

    out.posBuffer    = makeRaw(data.positions.data(), posSz);
    out.normalBuffer = makeRaw(data.normals.data(),   norSz);
    out.indexBuffer  = makeRaw(data.indices.data(),   idxSz);
    if (hasTangents) out.tangentBuffer = makeRaw(data.tangents.data(), tanSz);
    if (hasUVs)      out.uvBuffer      = makeRaw(data.uvs.data(),      uvSz);
    if (hasColors)   out.colorBuffer   = makeRaw(data.colors.data(),   colSz);
    out.indexCount   = static_cast<uint32_t>(data.indices.size());
    out.vertexCount  = static_cast<uint32_t>(data.positions.size());

    // Pick register variant by lifetime. Persistent path lands in the
    // low reserved slot range so the cached slot indices stored on @p out
    // survive every world reload (no manual re-register needed in
    // MeshManager::OnWorldClear). World path is dedup'd and cleared with
    // the rest of the per-world allocations.
    auto regBuf = [&](const RHI::GPUBuffer& b) {
        return persistent ? m_descHeap.RegisterPersistentBuffer(b)
                          : m_descHeap.RegisterBuffer(b);
    };

    const uint32_t posIdx = regBuf(out.posBuffer);
    const uint32_t norIdx = regBuf(out.normalBuffer);
    const uint32_t idxIdx = regBuf(out.indexBuffer);
    const uint32_t tanIdx = hasTangents ? regBuf(out.tangentBuffer)
                                        : RHI::kInvalidBufferIndex;
    const uint32_t uvIdx  = hasUVs      ? regBuf(out.uvBuffer)
                                        : RHI::kInvalidBufferIndex;
    const uint32_t colIdx = hasColors   ? regBuf(out.colorBuffer)
                                        : RHI::kInvalidBufferIndex;

    if (posIdx == RHI::kInvalidBufferIndex ||
        norIdx == RHI::kInvalidBufferIndex ||
        idxIdx == RHI::kInvalidBufferIndex ||
        (hasColors && colIdx == RHI::kInvalidBufferIndex))
    {
        LOG_ERROR("MeshManager::UploadMesh: bindless table full (persistent=%d)", persistent ? 1 : 0);
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

    // Procedural per-vertex color → dedicated color stream (Float3; the shader
    // defaults alpha to 1). uv1 stays absent — primitives have no 2nd UV set.
    if (hasColors && colIdx != RHI::kInvalidBufferIndex)
    {
        md.color.bufferIndex = colIdx;
        md.color.byteOffset  = 0;
        md.color.byteStride  = sizeof(float) * 3;
        md.color.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
    }

    md.indexBufferIndex = idxIdx;
    md.indexByteOffset  = 0;
    md.indexFormat      = 0;
    md.vertexCount      = out.vertexCount;

    out.meshDescSlot = persistent ? m_descHeap.RegisterPersistentMesh(md)
                                  : m_descHeap.RegisterMesh(md);
    return out.meshDescSlot != RHI::kInvalidBufferIndex;
}

void MeshManager::InitPrimitives()
{
    constexpr int kCount = static_cast<int>(PrimitiveMeshType::Count);
    const ProceduralMesh::MeshData meshes[kCount] =
    {
        ProceduralMesh::Cube(),
        ProceduralMesh::Sphere(),
        ProceduralMesh::Cone(),
        ProceduralMesh::Plane(),
        ProceduralMesh::Torus(),
    };
    static const char* primNames[] = { "Cube", "Sphere", "Cone", "Plane", "Torus" };

    for (int i = 0; i < kCount; ++i)
    {
        // persistent=true: primitive slots stay valid across every world reload
        // (entities created by MeshSpawner reference them by cached slot index).
        if (UploadMesh(meshes[i], m_primitives[i], /*persistent=*/true))
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

    // persistent=true: billboard slot is referenced by every BillboardComponent
    // across every world; must survive reloads.
    if (UploadMesh(quad, m_billboardQuad, /*persistent=*/true))
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

    const uint32_t vStride  = lib.GetVertexStride(ref.libHandle);
    const uint32_t libFlags = lib.GetMeshLibFlags(ref.libHandle);
    const Resource::MeshLibVertexLayout L =
        Resource::ComputeMeshLibVertexLayout(libFlags);

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
    md.position.byteOffset  = L.posOffset;
    md.position.byteStride  = vStride;
    md.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.normal.bufferIndex   = slots.vbIdx;
    md.normal.byteOffset    = L.normalOffset;
    md.normal.byteStride    = vStride;
    md.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.uv0.bufferIndex      = slots.vbIdx;
    md.uv0.byteOffset       = L.uv0Offset;
    md.uv0.byteStride       = vStride;
    md.uv0.format           = static_cast<uint32_t>(RHI::VertexFormat::Float2);

    // Optional interleaved streams — present only when the matching
    // MESHLIB_FLAG_* bit set the corresponding offset. Absent streams keep
    // their default-init INVALID_BUFFER so the shader helpers fall back
    // gracefully (tangent → synthesised TBN, uv1 → (0,0), color → white).
    if (L.tangentOffset != 0xFFFFFFFFu)
    {
        md.tangent.bufferIndex = slots.vbIdx;
        md.tangent.byteOffset  = L.tangentOffset;
        md.tangent.byteStride  = vStride;
        md.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }
    if (L.uv1Offset != 0xFFFFFFFFu)
    {
        md.uv1.bufferIndex = slots.vbIdx;
        md.uv1.byteOffset  = L.uv1Offset;
        md.uv1.byteStride  = vStride;
        md.uv1.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (L.colorOffset != 0xFFFFFFFFu)
    {
        md.color.bufferIndex = slots.vbIdx;
        md.color.byteOffset  = L.colorOffset;
        md.color.byteStride  = vStride;
        md.color.format      = static_cast<uint32_t>(RHI::VertexFormat::R8G8B8A8);
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
    // Reset the descriptor heap's world-scoped slot allocators only — slots
    // in the persistent low range [0..kPermanent*Slots) keep their SRVs
    // intact, so the primitives + billboard quad uploaded at Init time
    // (via UploadMesh(..., persistent=true)) and the skinned vertex ring
    // (via RegisterPersistentBuffer) keep their cached slot indices valid
    // across the reload — no caller-side re-register needed any more.
    //
    // Caller must FlushAndWait before this so the GPU isn't still reading
    // the world slots (Renderer::OnWorldClear runs mid-frame but the editor
    // "Load World..." path flushes first).
    m_descHeap.OnWorldClear();
}
