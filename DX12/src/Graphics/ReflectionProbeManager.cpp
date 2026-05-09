#include "Graphics/ReflectionProbeManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "System/Log.h"

#include <cstring>

bool ReflectionProbeManager::Init(IGraphicsDevice& gfx)
{
    using namespace Reflection;

    // ---- Cubemap-array ----
    {
        RHI::TextureDesc td{};
        td.width      = kProbeCubemapSize;
        td.height     = kProbeCubemapSize;
        td.array_size = kMaxReflectionProbes * 6;
        td.mip_levels = kProbeCubemapMips;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bind_flags = RHI::BindFlag::SHADER_RESOURCE
                      | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.misc_flags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        td.debug_name = "ReflectionProbeManager.CubemapArray";

        if (!gfx.CreateTexture(td, m_array))
        {
            LOG_ERROR("ReflectionProbeManager: cubemap-array create failed");
        }
        else
        {
            m_arraySrv = gfx.GetTextureSRVGpuHandle(m_array);
            LOG_INFO("ReflectionProbeManager: cubemap-array ready (%u cubes, %u mips, %ux%u)",
                     kMaxReflectionProbes, kProbeCubemapMips,
                     kProbeCubemapSize, kProbeCubemapSize);
        }
    }

    // ---- StructuredBuffer<Reflection::GPUReflectionProbe> ----
    {
        RHI::GPUBufferDesc desc{};
        desc.size       = static_cast<uint64_t>(kMaxReflectionProbes) * sizeof(Reflection::GPUReflectionProbe);
        desc.stride     = sizeof(Reflection::GPUReflectionProbe);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!gfx.CreateBuffer(desc, m_buffer))
        {
            LOG_ERROR("ReflectionProbeManager: StructuredBuffer create failed");
            return false;
        }
        m_bufferMapped = static_cast<Reflection::GPUReflectionProbe*>(gfx.MapBuffer(m_buffer));
        m_bufferSrv    = gfx.GetBufferSRVGpuHandle(m_buffer);

        if (m_bufferMapped)
            std::memset(m_bufferMapped, 0,
                        kMaxReflectionProbes * sizeof(Reflection::GPUReflectionProbe));
    }

    if (!m_capturePass.Init(gfx))
    {
        LOG_ERROR("ReflectionProbeManager: capture pass init failed");
        return false;
    }
    return true;
}

void ReflectionProbeManager::EnqueueBake(uint32_t cubeSlice)
{
    if (cubeSlice >= Reflection::kMaxReflectionProbes) return;
    for (uint32_t s : m_bakeQueue) if (s == cubeSlice) return;
    m_bakeQueue.push_back(cubeSlice);
}

uint32_t ReflectionProbeManager::PeekBake() const
{
    return m_bakeQueue.empty() ? ~0u : m_bakeQueue.front();
}

void ReflectionProbeManager::PopBake()
{
    if (!m_bakeQueue.empty()) m_bakeQueue.erase(m_bakeQueue.begin());
}
