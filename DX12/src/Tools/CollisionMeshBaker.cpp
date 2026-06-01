#include "Tools/CollisionMeshBaker.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // MeshSourcePath, MeshLibRef
#include "ECS/PhysicsComponents.h"     // ColliderComponent (debug wireframe walk)
#include "Physics/PhysicsSystem.h"     // EnsureCollisionBlob — shared blob cache
#include "Resource/AssetFS.h"          // AssetFS::Get().ReadFile
#include "Resource/AssetHeader.h"      // MAGIC_*, accessor helpers
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "System/Log.h"
#include "System/TaskSystem.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>   // debug-wireframe decoded-slice cache

#ifdef _DEBUG
#pragma comment(lib, "meshoptimizer_debug.lib")
#else
#pragma comment(lib, "meshoptimizer_release.lib")
#endif

namespace Tools::CollisionMesh {

namespace {

// 32-byte vertex written to the output .meshlib. Normal / uv left zero —
// collision and LOD callers don't need them, and GBuffer.vs's synthesised
// basis lets the library still render acceptably for previewing the shape.
struct PackedVertex32
{
    float px, py, pz;
    float nx, ny, nz;
    float u,  v;
};
static_assert(sizeof(PackedVertex32) == 32, "PackedVertex32 layout drift");

std::string ToLowerExt(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool DecodeImsh(const std::vector<uint8_t>& blob,
                std::vector<DirectX::XMFLOAT3>& outPositions,
                std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
        return false;

    const auto* meta = Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
    const uint32_t nv      = meta->vertexCount;
    const uint32_t ni      = meta->indexCount;
    const uint32_t vStride = meta->vertexStride;
    if (nv == 0 || ni == 0 || vStride < sizeof(float) * 3)
        return false;

    const uint8_t* payload = Resource::GetPayload(blob.data());
    const uint8_t* vbBase  = payload;
    const uint8_t* ibBase  = vbBase + size_t(nv) * vStride;

    outPositions.resize(nv);
    for (uint32_t i = 0; i < nv; ++i)
        std::memcpy(&outPositions[i], vbBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));

    outIndices.resize(ni);
    std::memcpy(outIndices.data(), ibBase, size_t(ni) * sizeof(uint32_t));
    return true;
}

bool DecodeMeshLibEntry(const std::vector<uint8_t>& blob, uint32_t meshId,
                        std::vector<DirectX::XMFLOAT3>& outPositions,
                        std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
        return false;

    const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
    if (meshId >= meta->meshCount) return false;

    const uint8_t* payload   = Resource::GetPayload(blob.data());
    const auto*    entries   = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
    const auto&    entry     = entries[meshId];
    if (entry.vertexCount == 0 || entry.indexCount == 0) return false;

    const uint32_t vStride   = meta->vertexStride;
    const uint8_t* vbBase    = payload + size_t(meta->meshCount) * sizeof(Resource::MeshLibraryEntry);
    const uint8_t* ibBase    = vbBase + size_t(meta->vertexCount) * vStride;
    const uint8_t* vertsBase = vbBase + size_t(entry.vertexStart) * vStride;
    const auto*    idxData   = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;

    outPositions.resize(entry.vertexCount);
    for (uint32_t i = 0; i < entry.vertexCount; ++i)
        std::memcpy(&outPositions[i], vertsBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));

    outIndices.resize(entry.indexCount);
    for (uint32_t i = 0; i < entry.indexCount; ++i)
        outIndices[i] = idxData[i] - entry.vertexStart;
    return true;
}

bool SimplifyPositionsIndices(std::vector<DirectX::XMFLOAT3>& positions,
                              std::vector<uint32_t>&          indices,
                              const BakeOptions&              opts,
                              float&                          outError,
                              DirectX::XMFLOAT3&              outAabbMin,
                              DirectX::XMFLOAT3&              outAabbMax)
{
    outError = 0.f;
    if (positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
        return false;

    const size_t stride = sizeof(DirectX::XMFLOAT3);

    std::vector<uint32_t> remap(positions.size());
    const size_t welded = meshopt_generateVertexRemap(
        remap.data(),
        indices.data(), indices.size(),
        positions.data(), positions.size(), stride);

    std::vector<DirectX::XMFLOAT3> weldedVerts(welded);
    meshopt_remapVertexBuffer(weldedVerts.data(), positions.data(),
                              positions.size(), stride, remap.data());
    std::vector<uint32_t> weldedIdx(indices.size());
    meshopt_remapIndexBuffer(weldedIdx.data(), indices.data(),
                             indices.size(), remap.data());

    const size_t origTris   = weldedIdx.size() / 3;
    size_t targetTris       = static_cast<size_t>(std::max<uint32_t>(
        opts.minTriangles,
        static_cast<uint32_t>(origTris * opts.targetTriangleRatio)));
    if (targetTris > origTris) targetTris = origTris;
    const size_t targetIndexCount = targetTris * 3;

    std::vector<uint32_t> simplified(weldedIdx.size());
    const size_t outIndexCount = meshopt_simplify(
        simplified.data(),
        weldedIdx.data(), weldedIdx.size(),
        reinterpret_cast<const float*>(weldedVerts.data()),
        weldedVerts.size(), stride,
        targetIndexCount,
        opts.targetError,
        /*options*/ 0,
        &outError);
    simplified.resize(outIndexCount);
    if (simplified.empty()) return false;

    std::vector<uint32_t> compactRemap(weldedVerts.size());
    const size_t finalVertCount = meshopt_optimizeVertexFetchRemap(
        compactRemap.data(),
        simplified.data(), simplified.size(),
        weldedVerts.size());

    std::vector<DirectX::XMFLOAT3> finalVerts(finalVertCount);
    meshopt_remapVertexBuffer(finalVerts.data(), weldedVerts.data(),
                              weldedVerts.size(), stride, compactRemap.data());
    std::vector<uint32_t> finalIdx(simplified.size());
    meshopt_remapIndexBuffer(finalIdx.data(), simplified.data(),
                             simplified.size(), compactRemap.data());

    outAabbMin = finalVerts[0];
    outAabbMax = finalVerts[0];
    for (const auto& p : finalVerts)
    {
        outAabbMin.x = std::min(outAabbMin.x, p.x);
        outAabbMin.y = std::min(outAabbMin.y, p.y);
        outAabbMin.z = std::min(outAabbMin.z, p.z);
        outAabbMax.x = std::max(outAabbMax.x, p.x);
        outAabbMax.y = std::max(outAabbMax.y, p.y);
        outAabbMax.z = std::max(outAabbMax.z, p.z);
    }

    positions = std::move(finalVerts);
    indices   = std::move(finalIdx);
    return true;
}

} // namespace

// =========================================================================
// BakeJob
// =========================================================================

BakeJob::~BakeJob()
{
    WaitForWorkers();
}

void BakeJob::WaitForWorkers()
{
    // Spin until all chunks have finished. Cheap busy-wait — typical exit
    // case is "queue already drained" so this falls through immediately.
    while (m_inFlightChunks.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void BakeJob::Reset()
{
    WaitForWorkers();
    m_opts = {};
    m_queue.clear();
    m_perItem.clear();
    m_processedCount.store(0, std::memory_order_relaxed);
    m_inFlightChunks.store(0, std::memory_order_relaxed);
    m_active      = false;
    m_completed   = false;
    m_writeFailed = false;
    m_mergedVB.clear();
    m_mergedIB.clear();
    m_entries.clear();
    m_results.clear();
    m_origTriTotal  = 0;
    m_bakedTriTotal = 0;
}

void BakeJob::Begin(World& world, const BakeOptions& opts)
{
    Reset();
    m_opts   = opts;
    m_active = true;

    std::set<std::pair<std::string, uint32_t>> seen;
    world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
    {
        const MeshSourcePath* sp = world.GetComponent<MeshSourcePath>(e);
        if (!sp || sp->path.empty()) return;
        if (!seen.insert({ sp->path, ref.meshId }).second) return;
        m_queue.push_back({ sp->path, ref.meshId });
    });

    const uint32_t N = static_cast<uint32_t>(m_queue.size());
    LOG_INFO("CollisionMeshBaker: queued %u unique source meshes", N);

    if (N == 0)
    {
        m_completed = true;
        m_active    = false;
        return;
    }

    m_perItem.assign(N, PerItemOutput{});

    // Split [0, N) across as many chunks as the pool has workers. Each chunk
    // runs DecodeAndSimplify serially over its slice and bumps the atomics
    // when done. Main thread polls in Tick().
    const uint32_t workers   = std::max(1u, TaskSystem::Get().GetWorkerCount());
    const uint32_t chunks    = std::min(N, workers);
    const uint32_t chunkSize = (N + chunks - 1) / chunks;

    m_inFlightChunks.store(chunks, std::memory_order_release);

    for (uint32_t t = 0; t < chunks; ++t)
    {
        const uint32_t lo = t * chunkSize;
        const uint32_t hi = std::min(N, lo + chunkSize);
        if (lo >= hi) { m_inFlightChunks.fetch_sub(1, std::memory_order_acq_rel); continue; }

        TaskSystem::Get().Push([this, lo, hi]()
        {
            for (uint32_t i = lo; i < hi; ++i)
            {
                DecodeAndSimplify(m_queue[i], m_perItem[i]);
                m_processedCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_inFlightChunks.fetch_sub(1, std::memory_order_acq_rel);
        }, TaskSystem::TaskPriority::High);
    }

    LOG_INFO("CollisionMeshBaker: dispatched %u parallel chunks (%u workers)", chunks, workers);
}

bool BakeJob::Tick()
{
    if (!m_active || m_completed)
        return false;

    // Still work in flight? Just paint the progress bar this frame.
    if (m_inFlightChunks.load(std::memory_order_acquire) != 0)
        return true;

    // All workers done — serial merge phase on the main thread.
    m_results.reserve(m_queue.size());
    for (uint32_t i = 0; i < m_queue.size(); ++i)
    {
        BakeItemResult r{};
        AppendOne(m_queue[i], m_perItem[i], r);
        m_results.push_back(std::move(r));
    }
    Finalize();
    m_active    = false;
    m_completed = true;
    return false;
}

// Runs on a worker thread. Must not touch m_merged* / m_entries / m_results.
void BakeJob::DecodeAndSimplify(const Item& item, PerItemOutput& out)
{
    namespace fs = std::filesystem;

    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(item.path, blob) || blob.empty())
    {
        out.success = false; out.reason = "cannot read source file";
        return;
    }

    const std::string ext = ToLowerExt(fs::path(item.path));
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;
    bool decoded = false;
    if (ext == ".meshlib")   decoded = DecodeMeshLibEntry(blob, item.meshId, positions, indices);
    else if (ext == ".imsh") decoded = DecodeImsh(blob, positions, indices);
    else                     { out.reason = "unsupported source extension"; return; }

    if (!decoded)
    {
        out.success = false; out.reason = "decode failed";
        return;
    }

    out.origVertCount = static_cast<uint32_t>(positions.size());
    out.origTriCount  = static_cast<uint32_t>(indices.size() / 3);

    if (!SimplifyPositionsIndices(positions, indices, m_opts,
                                   out.actualError, out.aabbMin, out.aabbMax))
    {
        out.success = false; out.reason = "simplification failed";
        return;
    }

    out.positions = std::move(positions);
    out.indices   = std::move(indices);
    out.success   = true;
}

// Runs on the main thread after all workers finish.
void BakeJob::AppendOne(const Item& item, const PerItemOutput& slot, BakeItemResult& r)
{
    r.sourcePath    = item.path;
    r.sourceMeshId  = item.meshId;
    r.origVertCount = slot.origVertCount;
    r.origTriCount  = slot.origTriCount;

    if (!slot.success)
    {
        r.success = false;
        r.reason  = slot.reason.empty() ? "unknown failure" : slot.reason;
        return;
    }

    const uint32_t vertexStart = static_cast<uint32_t>(
        m_mergedVB.size() / sizeof(PackedVertex32));
    const uint32_t indexStart  = static_cast<uint32_t>(m_mergedIB.size());

    const size_t oldVBSize = m_mergedVB.size();
    m_mergedVB.resize(oldVBSize + slot.positions.size() * sizeof(PackedVertex32));
    auto* vbDst = reinterpret_cast<PackedVertex32*>(m_mergedVB.data() + oldVBSize);
    for (size_t i = 0; i < slot.positions.size(); ++i)
    {
        vbDst[i] = {};
        vbDst[i].px = slot.positions[i].x;
        vbDst[i].py = slot.positions[i].y;
        vbDst[i].pz = slot.positions[i].z;
        // normal/uv left zero.
    }

    m_mergedIB.reserve(m_mergedIB.size() + slot.indices.size());
    for (uint32_t idx : slot.indices)
        m_mergedIB.push_back(idx + vertexStart);

    Resource::MeshLibraryEntry entry{};
    entry.vertexStart       = vertexStart;
    entry.vertexCount       = static_cast<uint32_t>(slot.positions.size());
    entry.indexStart        = indexStart;
    entry.indexCount        = static_cast<uint32_t>(slot.indices.size());
    entry.aabbMin[0] = slot.aabbMin.x; entry.aabbMin[1] = slot.aabbMin.y; entry.aabbMin[2] = slot.aabbMin.z;
    entry.aabbMax[0] = slot.aabbMax.x; entry.aabbMax[1] = slot.aabbMax.y; entry.aabbMax[2] = slot.aabbMax.z;
    entry.defaultMaterialIdx = 0xFFFFFFFFu;
    entry.flags             = 0;
    m_entries.push_back(entry);

    r.bakedMeshId    = static_cast<uint32_t>(m_entries.size() - 1);
    r.bakedVertCount = entry.vertexCount;
    r.bakedTriCount  = entry.indexCount / 3;
    r.actualError    = slot.actualError;
    r.success        = true;

    m_origTriTotal  += r.origTriCount;
    m_bakedTriTotal += r.bakedTriCount;
}

bool BakeJob::Finalize()
{
    namespace fs = std::filesystem;

    if (m_entries.empty())
    {
        LOG_WARNING("CollisionMeshBaker: no meshes survived the bake — nothing written");
        return false;
    }

    std::error_code ec;
    const fs::path outPath = m_opts.outputPath;
    if (outPath.has_parent_path())
        fs::create_directories(outPath.parent_path(), ec);

    const uint32_t entryBytes  = static_cast<uint32_t>(
        m_entries.size() * sizeof(Resource::MeshLibraryEntry));
    const uint32_t vbBytes     = static_cast<uint32_t>(m_mergedVB.size());
    const uint32_t ibBytes     = static_cast<uint32_t>(m_mergedIB.size() * sizeof(uint32_t));
    const uint32_t vertexCount = vbBytes / static_cast<uint32_t>(sizeof(PackedVertex32));
    const uint32_t indexCount  = static_cast<uint32_t>(m_mergedIB.size());

    Resource::MeshLibraryMetadata meta{};
    meta.meshCount    = static_cast<uint32_t>(m_entries.size());
    meta.vertexCount  = vertexCount;
    meta.indexCount   = indexCount;
    meta.vertexStride = static_cast<uint16_t>(sizeof(PackedVertex32));
    meta.indexStride  = 4;
    meta.flags        = 0;
    meta.reserved     = 0;

    Resource::AssetHeader hdr{};
    hdr.magic        = Resource::MAGIC_MESHLIB;
    hdr.version      = Resource::ASSET_VERSION;
    hdr.resourceType = static_cast<uint16_t>(Resource::ResourceType::MeshLibrary);
    hdr.metadataSize = sizeof(Resource::MeshLibraryMetadata);
    hdr.dataSize     = entryBytes + vbBytes + ibBytes;
    hdr.flags        = 0;
    hdr.reserved     = 0;

    const size_t totalBytes = sizeof(Resource::AssetHeader)
                            + sizeof(Resource::MeshLibraryMetadata)
                            + entryBytes + vbBytes + ibBytes;
    std::vector<uint8_t> blob(totalBytes);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr,  sizeof(hdr));   dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta));  dst += sizeof(meta);
    std::memcpy(dst, m_entries.data(), entryBytes); dst += entryBytes;
    if (vbBytes > 0)  { std::memcpy(dst, m_mergedVB.data(), vbBytes); dst += vbBytes; }
    if (ibBytes > 0)  { std::memcpy(dst, m_mergedIB.data(), ibBytes); }

    std::ofstream f(m_opts.outputPath, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        LOG_ERROR("CollisionMeshBaker: cannot open '%s' for writing",
                  m_opts.outputPath.c_str());
        m_writeFailed = true;
        return false;
    }
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
    if (!f.good())
    {
        LOG_ERROR("CollisionMeshBaker: write failed for '%s'", m_opts.outputPath.c_str());
        m_writeFailed = true;
        return false;
    }

    LOG_INFO("CollisionMeshBaker: wrote '%s' (%u meshes, %u verts, %u indices, "
             "%llu -> %llu tris, %zu bytes)",
             m_opts.outputPath.c_str(),
             meta.meshCount, vertexCount, indexCount,
             static_cast<unsigned long long>(m_origTriTotal),
             static_cast<unsigned long long>(m_bakedTriTotal),
             totalBytes);
    return true;
}

// =========================================================================
// EmitDebugWireframe — per-frame collision wireframe overlay.
//
// File bytes come straight from PhysicsSystem's existing blob cache (the
// same one GetOrBuildMeshShape uses for body creation), so there is no
// duplicate I/O or duplicate cache when toggling the wireframe on. Slice
// decoding runs per visible entity each frame — cheap because the .meshlib
// blob is already in RAM and the per-tile decode is just pointer-chasing
// + a memcpy of ~few hundred positions.
// =========================================================================
void EmitDebugWireframe(World& world,
                        const DirectX::XMFLOAT3& cameraPos,
                        float maxDistance,
                        DebugWirePass& dbg,
                        DX12Physics::PhysicsSystem& physics,
                        uint32_t color)
{
    // maxDistance <= 0 → unlimited (every Mesh collider drawn).
    const bool  cullByDistance = maxDistance > 0.f;
    const float maxD2          = maxDistance * maxDistance;

    // Per-call scratch reused across entities — keeps memory peak at
    // O(largestMesh) and avoids reallocation in the hot loop.
    std::vector<DirectX::XMFLOAT3> worldScratch;

    // Decoded LOCAL-space slices, cached by "path#meshId". The decode is a
    // pure function of (blob, meshId), so doing it every frame was a top CPU
    // self-cost with the wireframe on. The same file always yields the same
    // geometry, so the cache stays valid across world reloads that re-reference
    // the same collision assets. Bounded by the number of unique collider
    // meshes ever seen — modest.
    struct CachedSlice
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t>          indices;
    };
    static std::unordered_map<std::string, CachedSlice> s_sliceCache;

    uint32_t seen = 0, drewMesh = 0, drewPrim = 0, skipped = 0;

    world.ForEach<ColliderComponent>([&](Entity e, ColliderComponent& c)
    {
        ++seen;
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) { ++skipped; return; }
        if (cullByDistance)
        {
            const float dx = gt->matrix._41 - cameraPos.x;
            const float dy = gt->matrix._42 - cameraPos.y;
            const float dz = gt->matrix._43 - cameraPos.z;
            if (dx*dx + dy*dy + dz*dz > maxD2) { ++skipped; return; }
        }

        using namespace DirectX;
        const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
        // Mirror PhysicsSystem::MakeShape — collider rotation + offset are
        // local-space and applied before the entity transform via
        // JPH::RotatedTranslatedShape. Jolt's wrap is `world = pos + rot * p`,
        // i.e. rotate the local point first, then translate. In DirectX
        // row-vector convention that's R * T * M, so the local shape coords
        // pass through the rotation, the translation, and the entity world
        // matrix in that order.
        const XMVECTOR shapeQ = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(c.rotationEulerDeg.x),
            XMConvertToRadians(c.rotationEulerDeg.y),
            XMConvertToRadians(c.rotationEulerDeg.z));
        const XMMATRIX shapeM = XMMatrixRotationQuaternion(shapeQ)
                              * XMMatrixTranslation(c.offset.x, c.offset.y, c.offset.z)
                              * M;
        XMFLOAT4X4 shapeFM;
        XMStoreFloat4x4(&shapeFM, shapeM);
        const XMFLOAT3 shapeOrigin{ shapeFM._41, shapeFM._42, shapeFM._43 };

        // ---- Non-Mesh primitive colliders -------------------------------
        // Reuse DebugWirePass's existing primitive helpers. Box/Sphere/
        // Capsule are drawn in entity-local space transformed by the world
        // matrix (translation column for the centre, basis axes for the
        // half-extents).
        if (c.shape == ColliderComponent::Shape::Box)
        {
            // Approximate as world-aligned AABB (ignores rotation in the
            // GlobalTransform). Acceptable for the debug viz; Jolt itself
            // honours rotation via the body pose.
            const XMFLOAT3 mn{ shapeOrigin.x - c.halfExtents.x,
                                shapeOrigin.y - c.halfExtents.y,
                                shapeOrigin.z - c.halfExtents.z };
            const XMFLOAT3 mx{ shapeOrigin.x + c.halfExtents.x,
                                shapeOrigin.y + c.halfExtents.y,
                                shapeOrigin.z + c.halfExtents.z };
            dbg.AddAABB(mn, mx, color);
            ++drewPrim;
            return;
        }
        if (c.shape == ColliderComponent::Shape::Sphere)
        {
            // Three great circles (XY/YZ/XZ) — recognisable sphere from any
            // angle. We use the entity's world translation as the centre but
            // ignore rotation (a sphere is rotation-invariant anyway) and
            // ignore non-uniform scale (which would no longer be a sphere
            // and isn't a configuration Jolt supports for SphereShape).
            dbg.AddSphere(shapeOrigin, c.radius, color);
            ++drewPrim;
            return;
        }
        if (c.shape == ColliderComponent::Shape::Capsule)
        {
            // Capsule axis along local +Y; transform endpoints by shapeM so
            // the offset travels with the entity rotation/scale.
            XMVECTOR a = XMVectorSet(0, -c.halfHeight, 0, 1);
            XMVECTOR b = XMVectorSet(0,  c.halfHeight, 0, 1);
            a = XMVector3Transform(a, shapeM);
            b = XMVector3Transform(b, shapeM);
            XMFLOAT3 fa, fb;
            XMStoreFloat3(&fa, a);
            XMStoreFloat3(&fb, b);
            dbg.AddCapsule(fa, fb, c.radius, color);
            ++drewPrim;
            return;
        }
        if (c.shape != ColliderComponent::Shape::Mesh) { ++skipped; return; }

        // ---- Mesh trimesh wireframe -------------------------------------
        if (c.meshLibPath.empty()) { ++skipped; return; }

        // Shared blob cache — lazily populates on first request, reused
        // by body creation. Subsequent toggles cost zero I/O.
        const std::vector<uint8_t>* blob = physics.EnsureCollisionBlob(c.meshLibPath);
        if (!blob) { ++skipped; return; }

        // Decode the per-entity slice ONCE and cache it (keyed by path#meshId);
        // every later frame reuses the decoded local-space geometry. Re-decoding
        // each frame was a top CPU self-cost with the wireframe on.
        const std::string key = c.meshLibPath + '#' + std::to_string(c.meshLibMeshId);
        auto cacheIt = s_sliceCache.find(key);
        if (cacheIt == s_sliceCache.end())
        {
            CachedSlice cs;
            const std::string ext = ToLowerExt(std::filesystem::path(c.meshLibPath));
            bool ok = false;
            if      (ext == ".meshlib") ok = DecodeMeshLibEntry(*blob, c.meshLibMeshId,
                                                                  cs.positions, cs.indices);
            else if (ext == ".imsh")    ok = DecodeImsh(*blob, cs.positions, cs.indices);
            if (!ok || cs.positions.empty() || cs.indices.size() < 3) { ++skipped; return; }
            cacheIt = s_sliceCache.emplace(key, std::move(cs)).first;
        }
        const CachedSlice& slice = cacheIt->second;

        // shapeM (= TranslationOffset * M) already in scope from primitive
        // branch above. Using it here applies c.offset consistently across
        // all four shape kinds.
        worldScratch.resize(slice.positions.size());
        for (size_t i = 0; i < slice.positions.size(); ++i)
        {
            XMVECTOR v = XMVectorSet(slice.positions[i].x,
                                      slice.positions[i].y,
                                      slice.positions[i].z, 1.f);
            v = XMVector3Transform(v, shapeM);
            XMStoreFloat3(&worldScratch[i], v);
        }

        for (size_t i = 0; i + 2 < slice.indices.size(); i += 3)
        {
            const uint32_t i0 = slice.indices[i+0];
            const uint32_t i1 = slice.indices[i+1];
            const uint32_t i2 = slice.indices[i+2];
            if (i0 >= worldScratch.size() || i1 >= worldScratch.size() || i2 >= worldScratch.size())
                continue;
            dbg.AddLine(worldScratch[i0], worldScratch[i1], color);
            dbg.AddLine(worldScratch[i1], worldScratch[i2], color);
            dbg.AddLine(worldScratch[i2], worldScratch[i0], color);
        }
        ++drewMesh;
    });

    // One-shot diagnostic — fires when the active collider count rises or
    // the skipped-count would surprise the user (e.g., wireframe missing
    // entities they expect to see). Throttle by static so it doesn't spam.
    static uint32_t s_lastSeen = 0;
    if (seen != s_lastSeen)
    {
        LOG_INFO("CollisionWireframe: scanned %u entities — drew %u mesh + %u primitive (Box/Sphere/Capsule), skipped %u",
                 seen, drewMesh, drewPrim, skipped);
        s_lastSeen = seen;
    }
}

} // namespace Tools::CollisionMesh
