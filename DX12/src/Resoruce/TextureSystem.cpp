#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Resource/TextureSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include <dxgiformat.h>

namespace Resource
{
    // -------------------------------------------------------------------------
    // Path hashing (FNV-1a 64-bit — same algorithm as ResourceManager)
    // -------------------------------------------------------------------------

    uint64_t TextureSystem::HashPath(const std::string& path)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : path)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    // -------------------------------------------------------------------------
    // DXGI_FORMAT → RHI::Format conversion table
    // -------------------------------------------------------------------------

    RHI::Format TextureSystem::DxgiToRhiFormat(uint32_t dxgiFmt)
    {
        switch (static_cast<DXGI_FORMAT>(dxgiFmt))
        {
        // Typeless formats from DDS are mapped to a sampleable typed variant.
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return RHI::Format::R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return RHI::Format::R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return RHI::Format::R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return RHI::Format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return RHI::Format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return RHI::Format::R16G16_UNORM;
        case DXGI_FORMAT_R32_TYPELESS:          return RHI::Format::R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:          return RHI::Format::R16_UNORM;
        case DXGI_FORMAT_R8_TYPELESS:           return RHI::Format::R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS:          return RHI::Format::BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:          return RHI::Format::BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:          return RHI::Format::BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:          return RHI::Format::BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:          return RHI::Format::BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:         return RHI::Format::BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:          return RHI::Format::BC7_UNORM;

        case DXGI_FORMAT_R32G32B32A32_FLOAT:   return RHI::Format::R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_UINT:    return RHI::Format::R32G32B32A32_UINT;
        case DXGI_FORMAT_R32G32B32A32_SINT:    return RHI::Format::R32G32B32A32_SINT;
        case DXGI_FORMAT_R32G32B32_FLOAT:      return RHI::Format::R32G32B32_FLOAT;
        case DXGI_FORMAT_R32G32B32_UINT:       return RHI::Format::R32G32B32_UINT;
        case DXGI_FORMAT_R32G32B32_SINT:       return RHI::Format::R32G32B32_SINT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   return RHI::Format::R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM:   return RHI::Format::R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_UINT:    return RHI::Format::R16G16B16A16_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM:   return RHI::Format::R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT:    return RHI::Format::R16G16B16A16_SINT;
        case DXGI_FORMAT_R32G32_FLOAT:         return RHI::Format::R32G32_FLOAT;
        case DXGI_FORMAT_R32G32_UINT:          return RHI::Format::R32G32_UINT;
        case DXGI_FORMAT_R32G32_SINT:          return RHI::Format::R32G32_SINT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:    return RHI::Format::R10G10B10A2_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UINT:     return RHI::Format::R10G10B10A2_UINT;
        case DXGI_FORMAT_R11G11B10_FLOAT:      return RHI::Format::R11G11B10_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM:       return RHI::Format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return RHI::Format::R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UINT:        return RHI::Format::R8G8B8A8_UINT;
        case DXGI_FORMAT_R8G8B8A8_SNORM:       return RHI::Format::R8G8B8A8_SNORM;
        case DXGI_FORMAT_R8G8B8A8_SINT:        return RHI::Format::R8G8B8A8_SINT;
        case DXGI_FORMAT_B8G8R8A8_UNORM:       return RHI::Format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return RHI::Format::B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16_FLOAT:         return RHI::Format::R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_UNORM:         return RHI::Format::R16G16_UNORM;
        case DXGI_FORMAT_R16G16_UINT:          return RHI::Format::R16G16_UINT;
        case DXGI_FORMAT_R16G16_SNORM:         return RHI::Format::R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT:          return RHI::Format::R16G16_SINT;
        case DXGI_FORMAT_D32_FLOAT:            return RHI::Format::D32_FLOAT;
        case DXGI_FORMAT_R32_FLOAT:            return RHI::Format::R32_FLOAT;
        case DXGI_FORMAT_R32_UINT:             return RHI::Format::R32_UINT;
        case DXGI_FORMAT_R32_SINT:             return RHI::Format::R32_SINT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return RHI::Format::D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R8G8_UNORM:           return RHI::Format::R8G8_UNORM;
        case DXGI_FORMAT_R8G8_UINT:            return RHI::Format::R8G8_UINT;
        case DXGI_FORMAT_R8G8_SNORM:           return RHI::Format::R8G8_SNORM;
        case DXGI_FORMAT_R8G8_SINT:            return RHI::Format::R8G8_SINT;
        case DXGI_FORMAT_R16_FLOAT:            return RHI::Format::R16_FLOAT;
        case DXGI_FORMAT_D16_UNORM:            return RHI::Format::D16_UNORM;
        case DXGI_FORMAT_R16_UNORM:            return RHI::Format::R16_UNORM;
        case DXGI_FORMAT_R16_UINT:             return RHI::Format::R16_UINT;
        case DXGI_FORMAT_R16_SNORM:            return RHI::Format::R16_SNORM;
        case DXGI_FORMAT_R16_SINT:             return RHI::Format::R16_SINT;
        case DXGI_FORMAT_R8_UNORM:             return RHI::Format::R8_UNORM;
        case DXGI_FORMAT_R8_UINT:              return RHI::Format::R8_UINT;
        case DXGI_FORMAT_R8_SNORM:             return RHI::Format::R8_SNORM;
        case DXGI_FORMAT_R8_SINT:              return RHI::Format::R8_SINT;
        case DXGI_FORMAT_BC1_UNORM:            return RHI::Format::BC1_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB:       return RHI::Format::BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM:            return RHI::Format::BC2_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB:       return RHI::Format::BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM:            return RHI::Format::BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB:       return RHI::Format::BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC4_UNORM:            return RHI::Format::BC4_UNORM;
        case DXGI_FORMAT_BC4_SNORM:            return RHI::Format::BC4_SNORM;
        case DXGI_FORMAT_BC5_UNORM:            return RHI::Format::BC5_UNORM;
        case DXGI_FORMAT_BC5_SNORM:            return RHI::Format::BC5_SNORM;
        case DXGI_FORMAT_BC6H_UF16:            return RHI::Format::BC6H_UF16;
        case DXGI_FORMAT_BC6H_SF16:            return RHI::Format::BC6H_SF16;
        case DXGI_FORMAT_BC7_UNORM:            return RHI::Format::BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB:       return RHI::Format::BC7_UNORM_SRGB;
        case DXGI_FORMAT_NV12:                 return RHI::Format::NV12;
        default:                               return RHI::Format::UNKNOWN;
        }
    }

    // -------------------------------------------------------------------------
    // GPU upload (caller must hold m_mutex)
    // -------------------------------------------------------------------------

    void TextureSystem::UploadToGPU(const TextureResource& res, TextureEntry& entry, IGraphicsDevice& gfx)
    {
        const DirectX::TexMetadata& meta = res.GetMetadata();
        const DirectX::ScratchImage& img = res.GetImage();

        RHI::TextureDesc desc;
        desc.width      = static_cast<uint32_t>(meta.width);
        desc.height     = static_cast<uint32_t>(meta.height);
        desc.depth      = static_cast<uint32_t>(meta.depth);
        desc.array_size = static_cast<uint32_t>(meta.arraySize);
        desc.mip_levels = static_cast<uint32_t>(meta.mipLevels);
        desc.format     = DxgiToRhiFormat(static_cast<uint32_t>(meta.format));
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        desc.layout     = RHI::ResourceState::SHADER_RESOURCE;

        switch (meta.dimension)
        {
        case DirectX::TEX_DIMENSION_TEXTURE1D: desc.type = RHI::TextureDesc::Type::TEXTURE_1D; break;
        case DirectX::TEX_DIMENSION_TEXTURE3D: desc.type = RHI::TextureDesc::Type::TEXTURE_3D; break;
        default:                               desc.type = RHI::TextureDesc::Type::TEXTURE_2D; break;
        }

        if (meta.IsCubemap())
            desc.misc_flags |= RHI::ResourceMiscFlag::TEXTURECUBE;

        LOG_INFO("TextureSystem::UploadToGPU: %ux%u fmt=%u mips=%zu arraySize=%zu isCubemap=%d",
                 meta.width, meta.height, static_cast<unsigned>(meta.format),
                 meta.mipLevels, meta.arraySize, meta.IsCubemap() ? 1 : 0);

        if (desc.format == RHI::Format::UNKNOWN)
        {
            LOG_ERROR("TextureSystem: unsupported DXGI format %u (%ux%u, mips=%zu)",
                      static_cast<unsigned>(meta.format), meta.width, meta.height, meta.mipLevels);
            return;
        }

        // Build one SubresourceData per image (mip × array slice).
        const size_t subCount = img.GetImageCount();
        std::vector<RHI::SubresourceData> subData(subCount);
        const DirectX::Image* images = img.GetImages();
        for (size_t i = 0; i < subCount; ++i)
        {
            subData[i].data_ptr    = images[i].pixels;
            subData[i].row_pitch   = static_cast<uint32_t>(images[i].rowPitch);
            subData[i].slice_pitch = static_cast<uint32_t>(images[i].slicePitch);
        }

        if (gfx.CreateTexture(desc, entry.texture, subData.data()))
        {
            entry.gpuReady = true;
        }
        else
        {
            LOG_ERROR("TextureSystem: CreateTexture failed (format=%u, %ux%u mips=%zu)",
                      static_cast<unsigned>(meta.format), meta.width, meta.height, meta.mipLevels);
        }
    }

    // -------------------------------------------------------------------------
    // Slot pool helpers  (caller must hold m_mutex)
    // -------------------------------------------------------------------------

    uint32_t TextureSystem::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void TextureSystem::FreeSlot(uint32_t index)
    {
        TextureEntry& e = m_slots[index];
        m_pathHashToSlot.erase(e.pathHash);
        // Bump generation — never wraps to 0 (0 = null sentinel).
        e.generation = (e.generation % Handle::MAX_GEN) + 1;
        e.texture    = {};
        e.refCount   = 0;
        e.rmHandle   = Handle{};
        e.gpuReady   = false;
        e.pathHash   = 0;
        m_freeList.push_back(index);
    }

    bool TextureSystem::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        return m_slots[idx].generation == h.Generation();
    }

    // -------------------------------------------------------------------------
    // Acquire
    // -------------------------------------------------------------------------

    TextureHandle TextureSystem::Acquire(const std::string& path,
                                         ResourceManager&   rm,
                                         IGraphicsDevice&   /*gfx*/)
    {
        if (path.empty())
            return Handle{};

        const uint64_t key = HashPath(path);
        std::lock_guard<std::mutex> lock(m_mutex);

        // Dedup: same path → same slot, just inc refcount.
        auto it = m_pathHashToSlot.find(key);
        if (it != m_pathHashToSlot.end())
        {
            const uint32_t idx = it->second;
            TextureEntry& entry = m_slots[idx];
            entry.refCount++;
            return Handle::Make(idx, ResourceType::Texture, entry.generation);
        }

        uint32_t idx = AllocSlot();
        TextureEntry& entry = m_slots[idx];
        if (entry.generation == 0) entry.generation = 1;  // never issue gen=0

        entry.pathHash = key;
        entry.rmHandle = rm.Load(path, ResourceType::Texture);
        entry.refCount = 1;
        m_pathHashToSlot[key] = idx;

        // GPU promotion is deferred to Tick (called after BeginFrame / WaitForPreviousFrame).
        return Handle::Make(idx, ResourceType::Texture, entry.generation);
    }

    // -------------------------------------------------------------------------
    // Release
    // -------------------------------------------------------------------------

    void TextureSystem::Release(TextureHandle handle, IGraphicsDevice& gfx)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        if (!IsValidHandle(handle))
            return;

        const uint32_t idx = handle.Index();
        TextureEntry& entry = m_slots[idx];
        if (entry.refCount == 0)
            return;

        if (--entry.refCount == 0)
        {
            // Don't destroy immediately — the current frame may still reference the texture.
            // Tick() (called after BeginFrame/WaitForPreviousFrame) flushes this list safely.
            if (entry.gpuReady)
                m_pendingDestroy.push_back(std::move(entry.texture));
            FreeSlot(idx);
        }
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    bool TextureSystem::IsReady(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return false;
        return m_slots[handle.Index()].gpuReady;
    }

    const RHI::Texture* TextureSystem::GetTexture(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return nullptr;
        // No lock: main-thread only access (hot path).
        if (!IsValidHandle(handle))
            return nullptr;
        const TextureEntry& e = m_slots[handle.Index()];
        return e.gpuReady ? &e.texture : nullptr;
    }

    Handle TextureSystem::GetResourceManagerHandle(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return Handle{};
        if (!IsValidHandle(handle))
            return Handle{};
        return m_slots[handle.Index()].rmHandle;
    }

    // -------------------------------------------------------------------------
    // Tick — flush deferred destroys, promote RM-ready entries to GPU
    // -------------------------------------------------------------------------

    void TextureSystem::Tick(ResourceManager& rm, IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Flush deferred-destroy list first.
        // Safe: Tick is called after BeginFrame/WaitForPreviousFrame.
        for (auto& tex : m_pendingDestroy)
            gfx.DestroyTexture(tex);
        m_pendingDestroy.clear();

        // Promote RM-ready entries to GPU.
        for (auto& entry : m_slots)
        {
            if (entry.refCount == 0 || entry.gpuReady || !entry.rmHandle.IsValid())
                continue;
            if (rm.GetState(entry.rmHandle) != ResourceState::Ready)
                continue;

            const auto* texRes = rm.Get<TextureResource>(entry.rmHandle);
            if (texRes)
                UploadToGPU(*texRes, entry, gfx);
            else
                LOG_WARNING("TextureSystem::Tick: resource is not a TextureResource");
        }
    }

    // -------------------------------------------------------------------------
    // Shutdown
    // -------------------------------------------------------------------------

    void TextureSystem::Shutdown(IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& tex : m_pendingDestroy)
            gfx.DestroyTexture(tex);
        m_pendingDestroy.clear();
        for (auto& entry : m_slots)
        {
            if (entry.refCount > 0 && entry.gpuReady)
                gfx.DestroyTexture(entry.texture);
        }
        m_slots.clear();
        m_freeList.clear();
        m_pathHashToSlot.clear();
    }

} // namespace Resource
