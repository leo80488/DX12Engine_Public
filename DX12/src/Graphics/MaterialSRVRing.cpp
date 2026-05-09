#include "Graphics/MaterialSRVRing.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

bool MaterialSRVRing::Init(IGraphicsDevice& gfxBase, uint32_t maxMatsPerFrame)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfxBase);

    const uint32_t descPerMat = GraphicsDX12::kCustomMatTextureSlots;
    m_descPerFrame = maxMatsPerFrame * descPerMat;

    auto& alloc = dx12.GetCbvSrvUavAllocator();
    m_descIncBytes = dx12.GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (uint32_t f = 0; f < kFramesInFlight; ++f)
    {
        m_frames[f] = alloc.AllocateStatic(m_descPerFrame);
        if (!m_frames[f].IsValid())
        {
            LOG_ERROR("MaterialSRVRing::Init: AllocateStatic(%u) failed on frame %u",
                      m_descPerFrame, f);
            return false;
        }
    }

    m_current = 0;
    m_cursor  = 0;
    LOG_INFO("MaterialSRVRing: initialised (%u descriptors × %u frames, inc=%u)",
             m_descPerFrame, kFramesInFlight, m_descIncBytes);
    return true;
}

void MaterialSRVRing::Shutdown()
{
    for (auto& f : m_frames)
        f = {};   // DescriptorAllocation handles its own release via dtor / reset
    m_descPerFrame = 0;
    m_descIncBytes = 0;
    m_cursor       = 0;
    m_current      = 0;
    m_fallbackCpu  = {};
}

void MaterialSRVRing::BeginFrame(uint32_t frameIdx)
{
    m_current = frameIdx % kFramesInFlight;
    m_cursor  = 0;
}

MaterialSRVRing::Slice MaterialSRVRing::Allocate(uint32_t count)
{
    if (count == 0) return {};
    if (m_cursor + count > m_descPerFrame)
    {
        LOG_WARNING("MaterialSRVRing::Allocate: ring full (%u + %u > %u)",
                    m_cursor, count, m_descPerFrame);
        return {};
    }

    const auto& frame = m_frames[m_current];
    // `GpuCpuHandle` is the CPU-writable handle for the shader-visible heap,
    // i.e. what CopyDescriptorsSimple needs as its destination.
    D3D12_CPU_DESCRIPTOR_HANDLE baseCpu = frame.GetGpuCpuHandle();
    D3D12_GPU_DESCRIPTOR_HANDLE baseGpu = frame.GetGpuHandle();

    Slice out;
    out.cpu.ptr       = baseCpu.ptr + static_cast<std::size_t>(m_cursor) * m_descIncBytes;
    out.gpu           = baseGpu.ptr + static_cast<uint64_t>(m_cursor) * m_descIncBytes;
    out.descIncBytes  = m_descIncBytes;
    m_cursor += count;
    return out;
}
