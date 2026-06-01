#pragma once

// ReflectionProbeManager — owns the probe pool GPU resources:
//
//   - TextureCubeArray (kMaxReflectionProbes × 6 faces × kProbeCubemapMips)
//     that every probe samples via its slice index.
//   - StructuredBuffer<GPUReflectionProbe> — UPLOAD heap, persistently mapped,
//     rewritten each frame with active probe positions + AABB / influence data.
//   - ReflectionProbeCapturePass instance (PSO + temp cube + depth buffer used
//     to render each bake request).
//   - Bake queue — FIFO of slice indices waiting for a (re)bake. BuildScene
//     pushes dirty slices; Render() pops at most one per frame.
//
// Orchestration (walking ECS, packing probes, driving a bake, setting up the
// BakeContext) still lives inside Renderer because it has to coordinate with
// ClusterPass, SkyIBLPass, SceneBVH, and the current frame's DrawList. This
// class just holds the GPU state and the bake queue so Renderer.h doesn't have
// to spell out 6 separate members for them.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ReflectionProbeTypes.h"   // Reflection::GPUReflectionProbe
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include <cstdint>
#include <vector>

class IGraphicsDevice;

class ReflectionProbeManager
{
public:
    // Matches GraphicsDX12::FrameCount — probe StructuredBuffer is ring-allocated
    // because BuildScene_UploadProbes rewrites every visible slot each frame
    // (no dirty gate). A single UPLOAD buffer would race the GPU which is still
    // sampling frame N's data from LightingPass / ClusterPass / TransparentPass.
    static constexpr uint32_t kFrameCount = 3;

    // Create the cubemap-array + StructuredBuffer and initialise the capture
    // pass. Returns false if any step fails (capture pass init failure is
    // logged but non-fatal — the caller can still proceed without probes).
    bool Init(IGraphicsDevice& gfx);

    // Tear down GPU resources (unmap + destroy each ring slot). Idempotent.
    void Shutdown(IGraphicsDevice& gfx);

    // Raw UPLOAD-buffer pointer for per-frame packing — current frame's slot.
    // Writes to dst[slice] are visible to the GPU this frame. Returns nullptr
    // if Init() didn't manage to create the StructuredBuffer.
    Reflection::GPUReflectionProbe* GetUploadPointer(IGraphicsDevice& gfx) const;

    // Set / get the number of slots the shaders will read. BuildScene sets
    // this after packing; the cluster pass + lighting shader honour it.
    void     SetActiveProbeCount(uint32_t n) { m_activeCount = n; }
    uint32_t GetActiveProbeCount() const     { return m_activeCount; }

    // Enqueue a slice for baking. Deduped — slices already in the queue are
    // ignored. Out-of-range slices are dropped.
    void EnqueueBake(uint32_t cubeSlice);

    // Peek the next pending bake slice without removing it. Returns
    // ~0u when empty. Paired with PopBake() when the caller has confirmed
    // it can service the request this frame.
    uint32_t PeekBake() const;

    // Remove the front slice from the queue. No-op on empty queue.
    void PopBake();

    bool BakeQueueEmpty() const { return m_bakeQueue.empty(); }

    // GPU accessors — valid after Init() returns.
    RHI::Texture&       GetArrayTexture()         { return m_array; }
    const RHI::Texture& GetArrayTexture()   const { return m_array; }
    uint64_t            GetArraySrv()       const { return m_arraySrv;  }
    // Per-frame SRV of the probe StructuredBuffer — selects the ring slot
    // matching gfx.GetFrameIndex(). Renderer must rewire LightingPass /
    // TransparentPass / ClusterPass each frame.
    uint64_t            GetBufferSrv(IGraphicsDevice& gfx) const;

    // Capture pass instance — exposed so ProcessProbeBakeQueue can call
    // BakeProbe() on it with a context it builds from the scene state.
    ReflectionProbeCapturePass& GetCapturePass()  { return m_capturePass; }

private:
    RHI::Texture                m_array;
    uint64_t                    m_arraySrv    = 0;

    // Triple-buffered UPLOAD StructuredBuffer ring — one physical buffer +
    // mapped pointer + SRV per frame slot.
    RHI::GPUBuffer                  m_buffer[kFrameCount];
    Reflection::GPUReflectionProbe* m_bufferMapped[kFrameCount] = {};
    uint64_t                        m_bufferSrv[kFrameCount]    = {};

    uint32_t                    m_activeCount = 0;
    std::vector<uint32_t>       m_bakeQueue;
    ReflectionProbeCapturePass  m_capturePass;
};
