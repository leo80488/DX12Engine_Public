#pragma once

// CollisionMeshBaker — walks every MeshLibRef entity in the world, runs each
// referenced source mesh through meshoptimizer's edge-collapse simplifier,
// and writes ALL simplified results into one combined .meshlib file using the
// engine's native asset format. The resulting library can be loaded by
// MeshLibrary like any other mesh asset — useful as collision proxies, far-
// LODs, or anything else that wants a low-poly version of the scene.
//
// Output: one .meshlib. Vertex stride is 32 B (pos + normal + uv) with
// normals/uvs zeroed — collision/LOD callers don't need them, and the engine
// already handles missing tangents via GBuffer.vs's synthesised basis.
//
// Multi-threaded: Begin() dispatches the entire queue to TaskSystem workers.
// Each worker decodes + simplifies its assigned slice into a per-item output
// slot (no contention). Tick() polls an atomic counter to feed the progress
// bar, and merges + writes the final .meshlib on the call that observes the
// final completion.
//
//   BakeJob job;
//   job.Begin(world, opts);
//   while (job.Tick()) { /* paint progress bar */ }
//   // job.IsComplete() — read job.Results() / job.OutputPath()

#include "ECS/ECS.h"
#include "Resource/AssetHeader.h"   // Resource::MeshLibraryEntry (accumulator type)

class DebugWirePass;
namespace DX12Physics { class PhysicsSystem; }

#include <DirectXMath.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Tools::CollisionMesh {

struct BakeOptions
{
    float       targetTriangleRatio = 0.10f;
    float       targetError         = 0.05f;
    uint32_t    minTriangles        = 32;
    bool        skipSkinned         = true;
    std::string outputPath          = "asset/collision/baked_collision.meshlib";
};

struct BakeItemResult
{
    std::string sourcePath;
    uint32_t    sourceMeshId   = 0;     // meshId in the source .meshlib (or 0 for .imsh)
    uint32_t    bakedMeshId    = ~0u;   // entry index in the OUTPUT .meshlib
    uint32_t    origTriCount   = 0;
    uint32_t    bakedTriCount  = 0;
    uint32_t    origVertCount  = 0;
    uint32_t    bakedVertCount = 0;
    float       actualError    = 0.f;
    bool        success        = false;
    bool        skipped        = false;
    std::string reason;
};

// Per-frame wireframe overlay for every Mesh ColliderComponent within
// `maxDistance` of `cameraPos`. The first time a particular .meshlib path
// is requested, its CPU geometry is decoded and cached on the static side
// of this helper — subsequent frames only matrix-transform + push lines.
// Caller is responsible for clearing DebugWirePass first (Renderer::BeginFrame
// already does that). Drops silently if the per-frame line budget exceeds
// DebugWirePass::kMaxVertices.
// Per-frame wireframe overlay for every Mesh ColliderComponent within
// `maxDistance` of `cameraPos`. File bytes are pulled from PhysicsSystem's
// existing blob cache (the same one body creation uses) so toggling the
// wireframe does NOT trigger duplicate .meshlib reads or duplicate caching —
// blob ownership stays with PhysicsSystem and the wireframe just decodes
// the slice each frame from those bytes. Drops silently when DebugWirePass
// is full (see DebugWirePass::kMaxVertices).
void EmitDebugWireframe(World& world,
                        const DirectX::XMFLOAT3& cameraPos,
                        float maxDistance,
                        DebugWirePass& dbg,
                        DX12Physics::PhysicsSystem& physics,
                        uint32_t color = 0xff60a0e0u);

class BakeJob
{
public:
    BakeJob()  = default;
    ~BakeJob();

    BakeJob(const BakeJob&)            = delete;
    BakeJob& operator=(const BakeJob&) = delete;

    // Snapshot every renderable MeshLibRef entity into the work queue (dedup
    // by (sourcePath, meshId)), then dispatch the per-item decode+simplify
    // jobs to TaskSystem workers. Returns immediately.
    void Begin(World& world, const BakeOptions& opts);

    // Poll worker progress; when the queue has finished, merge + write the
    // combined .meshlib on the calling (main) thread. Returns true while
    // background work remains, false once IsComplete().
    bool Tick();

    bool         IsRunning()    const { return m_active && !m_completed; }
    bool         IsComplete()   const { return m_completed; }
    uint32_t     Total()        const { return static_cast<uint32_t>(m_queue.size()); }
    uint32_t     Processed()    const { return m_processedCount.load(std::memory_order_acquire); }
    float        Progress()     const
    {
        const uint32_t total = Total();
        return total == 0 ? 1.f : static_cast<float>(Processed()) / static_cast<float>(total);
    }
    const std::vector<BakeItemResult>& Results() const { return m_results; }
    const std::string& OutputPath()     const { return m_opts.outputPath; }
    bool         WriteFailed()  const { return m_writeFailed; }

    uint64_t     OrigTriTotal() const { return m_origTriTotal; }
    uint64_t     BakedTriTotal()const { return m_bakedTriTotal; }

    // Wait for any in-flight workers, then clear state.
    void Reset();

    // Public so the editor can build a (sourcePath, sourceMeshId) → bakedMeshId
    // map for swap-in-preview without re-iterating the world.
    struct Item { std::string path; uint32_t meshId; };
    const std::vector<Item>& Queue() const { return m_queue; }

private:
    // Worker-thread output for one queue item. Owned per-slot — workers never
    // touch each other's slots, so no locking is needed during the parallel
    // phase.
    struct PerItemOutput
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t>          indices;
        DirectX::XMFLOAT3              aabbMin = { 0, 0, 0 };
        DirectX::XMFLOAT3              aabbMax = { 0, 0, 0 };
        float                          actualError = 0.f;
        uint32_t                       origVertCount = 0;
        uint32_t                       origTriCount  = 0;
        bool                           success       = false;
        std::string                    reason;
    };

    // Runs on a worker thread.
    void DecodeAndSimplify(const Item& item, PerItemOutput& out);
    // Main-thread serial pass that appends one item's output into the shared
    // pools and produces the user-facing BakeItemResult entry.
    void AppendOne(const Item& item, const PerItemOutput& slot, BakeItemResult& outResult);
    // Main-thread: build the final blob and write to disk.
    bool Finalize();

    void WaitForWorkers();

    BakeOptions                              m_opts;
    std::vector<Item>                        m_queue;
    std::vector<PerItemOutput>               m_perItem;          // sized to queue
    std::atomic<uint32_t>                    m_processedCount{ 0 };
    std::atomic<uint32_t>                    m_inFlightChunks{ 0 };
    bool                                     m_active      = false;
    bool                                     m_completed   = false;
    bool                                     m_writeFailed = false;

    // Output pools (32 B/vertex pos+normal+uv).
    std::vector<uint8_t>                     m_mergedVB;
    std::vector<uint32_t>                    m_mergedIB;
    std::vector<Resource::MeshLibraryEntry>  m_entries;

    std::vector<BakeItemResult>              m_results;
    uint64_t                                 m_origTriTotal  = 0;
    uint64_t                                 m_bakedTriTotal = 0;
};

} // namespace Tools::CollisionMesh
