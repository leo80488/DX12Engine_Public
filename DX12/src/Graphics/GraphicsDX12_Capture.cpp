#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "System/Log.h"
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

// CaptureTextureToPNG / SaveTextureCubeToITEX / LoadITEXIntoTextureCube —
// synchronous GPU-readback I/O used by ShaderLab's Capture Frame button and
// the reflection-probe bake persistence pipeline. All three FlushAndWait
// internally so they're safe to call only at frame boundaries.

// ===========================================================================
// CaptureTextureToPNG — synchronous read-back of a 2D R8G8B8A8 texture into
// a PNG file. Targets ShaderLab's "Capture Frame" button.
// ===========================================================================

#include "DirectXTex.h"
#include "Resource/AssetHeader.h"   // .itex container — AssetHeader + TextureMetadata
#include "Resource/AssetFS.h"       // pak-first read for baked probe cubemaps
#include <fstream>

bool GraphicsDX12::CaptureTextureToPNG(const RHI::Texture& tex,
                                       RHI::ResourceState  currentState,
                                       const char*         path)
{
    if (!path || !*path)
    {
        LOG_ERROR("CaptureTextureToPNG: null/empty path");
        return false;
    }

    ID3D12Resource* res = GetTextureResource(tex);
    if (!res)
    {
        LOG_ERROR("CaptureTextureToPNG: source texture has no D3D12 resource");
        return false;
    }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        LOG_ERROR("CaptureTextureToPNG: only Texture2D supported (got dim=%d)",
                  desc.Dimension);
        return false;
    }
    // Limit to formats DirectXTex SaveToWICFile + PNG codec accept directly.
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        LOG_ERROR("CaptureTextureToPNG: unsupported format %d", desc.Format);
        return false;
    }

    // Compute footprint for a single-subresource readback.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   numRows      = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalSize    = 0;
    m_device->GetCopyableFootprints(&desc, 0, 1, 0,
                                    &footprint, &numRows, &rowSizeBytes, &totalSize);

    // READBACK buffer for the GPU → CPU copy.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width              = totalSize;
        bd.Height             = 1;
        bd.DepthOrArraySize   = 1;
        bd.MipLevels          = 1;
        bd.Format             = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count   = 1;
        bd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readback))))
        {
            LOG_ERROR("CaptureTextureToPNG: readback buffer alloc failed");
            return false;
        }
    }

    // Make sure prior submissions touching the source are done so the state
    // we transition from matches what the renderer last left.
    FlushAndWait();

    // One-shot command list dedicated to the copy. Cheap (no allocator reuse
    // since this only runs on a tools button press).
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>  cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))))
    {
        LOG_ERROR("CaptureTextureToPNG: command allocator alloc failed");
        return false;
    }
    if (FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    {
        LOG_ERROR("CaptureTextureToPNG: command list alloc failed");
        return false;
    }

    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);

    // Transition: from caller-provided state → COPY_SOURCE
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource         = res;
        src.Type              = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex  = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource         = readback.Get();
        dst.Type              = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint   = footprint;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Transition back so the renderer's tracker stays consistent.
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = fromState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Block until the copy is done — synchronous read-back is the whole point.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        LOG_ERROR("CaptureTextureToPNG: fence alloc failed");
        return false;
    }
    m_commandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    // Map readback + densify rows (footprint.RowPitch is 256-aligned, the
    // DirectXTex Image we hand to SaveToWICFile wants a tight rowPitch).
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalSize) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        LOG_ERROR("CaptureTextureToPNG: readback Map failed");
        return false;
    }

    const uint64_t denseRowPitch = rowSizeBytes;
    const uint64_t denseSize     = denseRowPitch * static_cast<uint64_t>(numRows);
    std::vector<uint8_t> dense(static_cast<size_t>(denseSize));
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped);
    for (UINT row = 0; row < numRows; ++row)
    {
        std::memcpy(dense.data() + row * denseRowPitch,
                    srcBytes + row * footprint.Footprint.RowPitch,
                    static_cast<size_t>(denseRowPitch));
    }

    D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    // Save via DirectXTex. Convert path UTF-8 → wide for SaveToWICFile.
    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) { wpath.resize(wlen - 1); MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen); }
    }

    // Make sure parent dir exists — most users will pick captures/foo.png.
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(wpath).parent_path(), ec);
    }

    DirectX::Image img{};
    img.width      = footprint.Footprint.Width;
    img.height     = footprint.Footprint.Height;
    img.format     = footprint.Footprint.Format;
    img.rowPitch   = static_cast<size_t>(denseRowPitch);
    img.slicePitch = static_cast<size_t>(denseSize);
    img.pixels     = dense.data();

    const HRESULT hr = DirectX::SaveToWICFile(
        img, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
        wpath.c_str());
    if (FAILED(hr))
    {
        LOG_ERROR("CaptureTextureToPNG: SaveToWICFile failed (hr=0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }

    LOG_SUCCESS("CaptureTextureToPNG: saved '%s' (%ux%u)",
                path, img.width, img.height);
    return true;
}

// ===========================================================================
// Cubemap-array slice <-> .itex — reflection-probe bake persistence.
// Probe cubemaps are stored in the engine's native .itex container —
// [AssetHeader][TextureMetadata][DDS bytes] — identical to what
// TextureImporter writes and TextureLoader reads, so they're ordinary engine
// assets. One cube = 6 consecutive array slices × all mips; subresource
// layout for a Texture2D array is arraySlice*mipLevels + mip, so cube N owns
// the contiguous subresource block [N*6*mipLevels .. +6*mipLevels). Both
// helpers are synchronous (FlushAndWait + one-shot CL + fence), mirroring
// CaptureTextureToPNG — see the header for call-site rules.
// ===========================================================================

bool GraphicsDX12::SaveTextureCubeToITEX(const RHI::Texture& cubeArrayTex,
                                         uint32_t            cubeIndex,
                                         RHI::ResourceState  currentState,
                                         const char*         path)
{
    if (!path || !*path) { LOG_ERROR("SaveTextureCubeToITEX: null/empty path"); return false; }

    ID3D12Resource* res = GetTextureResource(cubeArrayTex);
    if (!res) { LOG_ERROR("SaveTextureCubeToITEX: texture has no D3D12 resource"); return false; }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    { LOG_ERROR("SaveTextureCubeToITEX: source is not a Texture2D array"); return false; }

    const uint32_t mipLevels = desc.MipLevels;
    const uint32_t firstFace = cubeIndex * 6;
    if (firstFace + 6 > desc.DepthOrArraySize)
    {
        LOG_ERROR("SaveTextureCubeToITEX: cubeIndex %u out of range (arraySize=%u)",
                  cubeIndex, desc.DepthOrArraySize);
        return false;
    }

    const uint32_t subCount = 6u * mipLevels;
    const uint32_t firstSub = firstFace * mipLevels;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subCount);
    std::vector<UINT>   numRows(subCount);
    std::vector<UINT64> rowSizes(subCount);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&desc, firstSub, subCount, 0,
                                    footprints.data(), numRows.data(),
                                    rowSizes.data(), &totalSize);

    // READBACK buffer for the GPU → CPU copy of all 6×mips subresources.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalSize;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        { LOG_ERROR("SaveTextureCubeToITEX: readback buffer alloc failed"); return false; }
    }

    FlushAndWait();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    { LOG_ERROR("SaveTextureCubeToITEX: command list alloc failed"); return false; }

    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);

    // Per-subresource transitions — only this cube's slices, leaving the rest
    // of the shared probe array untouched.
    std::vector<D3D12_RESOURCE_BARRIER> toCopy(subCount), backState(subCount);
    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_RESOURCE_BARRIER& b = toCopy[i];
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = firstSub + i;
        backState[i] = b;
        std::swap(backState[i].Transition.StateBefore, backState[i].Transition.StateAfter);
    }
    cl->ResourceBarrier(subCount, toCopy.data());

    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = res;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = firstSub + i;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprints[i];
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    cl->ResourceBarrier(subCount, backState.data());
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    { LOG_ERROR("SaveTextureCubeToITEX: fence alloc failed"); return false; }
    m_commandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalSize) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    { LOG_ERROR("SaveTextureCubeToITEX: readback Map failed"); return false; }

    // Assemble a DirectXTex cube (6 faces × mipLevels), densifying each row
    // from the 256-aligned readback footprint into the tight DDS row pitch.
    DirectX::ScratchImage scratch;
    HRESULT hr = scratch.InitializeCube(desc.Format,
        footprints[0].Footprint.Width, footprints[0].Footprint.Height, 1, mipLevels);
    if (FAILED(hr))
    {
        D3D12_RANGE wn{ 0, 0 }; readback->Unmap(0, &wn);
        LOG_ERROR("SaveTextureCubeToITEX: InitializeCube failed (hr=0x%08X)", (unsigned)hr);
        return false;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t i = face * mipLevels + mip;
            const DirectX::Image* img = scratch.GetImage(mip, face, 0);
            if (!img) continue;
            const uint8_t* srcRows = base + footprints[i].Offset;
            const size_t   copyPitch =
                static_cast<size_t>(std::min<UINT64>(rowSizes[i], img->rowPitch));
            for (UINT row = 0; row < numRows[i]; ++row)
                std::memcpy(img->pixels + row * img->rowPitch,
                            srcRows + row * footprints[i].Footprint.RowPitch,
                            copyPitch);
        }

    D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    // Encode the cube to a DDS blob in memory, then wrap it in the engine's
    // .itex container — [AssetHeader][TextureMetadata][DDS bytes] — exactly
    // like Resource::TextureImporter does.
    DirectX::Blob ddsBlob;
    hr = DirectX::SaveToDDSMemory(scratch.GetImages(), scratch.GetImageCount(),
                                  scratch.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsBlob);
    if (FAILED(hr))
    {
        LOG_ERROR("SaveTextureCubeToITEX: SaveToDDSMemory failed (hr=0x%08X)", (unsigned)hr);
        return false;
    }

    const DirectX::TexMetadata& m = scratch.GetMetadata();
    Resource::TextureMetadata texMeta{};
    texMeta.width     = static_cast<uint32_t>(m.width);
    texMeta.height    = static_cast<uint32_t>(m.height);
    texMeta.depth     = static_cast<uint32_t>(m.depth);
    texMeta.mipLevels = static_cast<uint16_t>(m.mipLevels);
    texMeta.format    = static_cast<uint16_t>(m.format);
    texMeta.dimension = 6; // Cube

    Resource::AssetHeader header{};
    header.magic        = Resource::MAGIC_TEXTURE;
    header.version      = Resource::ASSET_VERSION;
    header.resourceType = static_cast<uint16_t>(Resource::ResourceType::Texture);
    header.metadataSize = sizeof(Resource::TextureMetadata);
    header.dataSize     = static_cast<uint32_t>(ddsBlob.GetBufferSize());

    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) { wpath.resize(wlen - 1); MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen); }
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(wpath).parent_path(), ec);
    }

    std::ofstream out(wpath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LOG_ERROR("SaveTextureCubeToITEX: cannot open '%s' for writing", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header),  sizeof(header));
    out.write(reinterpret_cast<const char*>(&texMeta), sizeof(texMeta));
    out.write(reinterpret_cast<const char*>(ddsBlob.GetBufferPointer()),
              static_cast<std::streamsize>(ddsBlob.GetBufferSize()));
    if (!out)
    {
        LOG_ERROR("SaveTextureCubeToITEX: write failed for '%s'", path);
        return false;
    }

    LOG_SUCCESS("SaveTextureCubeToITEX: saved '%s' (cube %u, %u mips)",
                path, cubeIndex, mipLevels);
    return true;
}

bool GraphicsDX12::LoadITEXIntoTextureCube(RHI::Texture&      cubeArrayTex,
                                           uint32_t           cubeIndex,
                                           RHI::ResourceState currentState,
                                           const char*        path)
{
    if (!path || !*path) { LOG_ERROR("LoadITEXIntoTextureCube: null/empty path"); return false; }

    ID3D12Resource* res = GetTextureResource(cubeArrayTex);
    if (!res) { LOG_ERROR("LoadITEXIntoTextureCube: texture has no D3D12 resource"); return false; }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    const uint32_t mipLevels = desc.MipLevels;
    const uint32_t firstFace = cubeIndex * 6;
    if (firstFace + 6 > desc.DepthOrArraySize)
    { LOG_ERROR("LoadITEXIntoTextureCube: cubeIndex %u out of range", cubeIndex); return false; }

    // Read the whole .itex blob via AssetFS (game.ipak first, then loose disk),
    // then parse it exactly like TextureLoader: ValidateHeader → GetPayload →
    // LoadFromDDSMemory. A raw std::ifstream here used to fail in PACKED builds
    // (the baked probe .itex lives only inside game.ipak), forcing a needless
    // re-bake every launch.
    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
    {
        LOG_WARNING("LoadITEXIntoTextureCube: cannot read '%s'", path);
        return false;
    }

    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_TEXTURE))
    {
        LOG_WARNING("LoadITEXIntoTextureCube: '%s' is not a valid .itex texture", path);
        return false;
    }
    const Resource::AssetHeader* hdr     = Resource::GetHeader(blob.data());
    const uint8_t*               ddsData = Resource::GetPayload(blob.data());

    DirectX::TexMetadata  meta{};
    DirectX::ScratchImage scratch;
    HRESULT hr = DirectX::LoadFromDDSMemory(ddsData, hdr->dataSize,
                                            DirectX::DDS_FLAGS_NONE, &meta, scratch);
    if (FAILED(hr))
    {
        LOG_WARNING("LoadITEXIntoTextureCube: LoadFromDDSMemory('%s') failed (hr=0x%08X)",
                    path, (unsigned)hr);
        return false;
    }

    // Strict layout match — there is no resize/convert path here; a mismatch
    // just makes the caller fall back to a fresh bake.
    if (!meta.IsCubemap() || meta.arraySize < 6 ||
        meta.width     != desc.Width  ||
        meta.height    != desc.Height ||
        meta.mipLevels != mipLevels   ||
        meta.format    != desc.Format)
    {
        LOG_WARNING("LoadITEXIntoTextureCube: '%s' layout mismatch — "
                    "got %zux%zu mips=%zu fmt=%d cube=%d; want %llux%llu mips=%u fmt=%d",
                    path, meta.width, meta.height, meta.mipLevels, (int)meta.format,
                    meta.IsCubemap() ? 1 : 0,
                    (unsigned long long)desc.Width, (unsigned long long)desc.Height,
                    mipLevels, (int)desc.Format);
        return false;
    }

    const uint32_t subCount = 6u * mipLevels;
    const uint32_t firstSub = firstFace * mipLevels;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subCount);
    std::vector<UINT>   numRows(subCount);
    std::vector<UINT64> rowSizes(subCount);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&desc, firstSub, subCount, 0,
                                    footprints.data(), numRows.data(),
                                    rowSizes.data(), &totalSize);

    // UPLOAD staging buffer — fill with DDS subresources repitched to the
    // 256-aligned footprint row pitch.
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalSize;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        { LOG_ERROR("LoadITEXIntoTextureCube: upload buffer alloc failed"); return false; }
    }

    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(upload->Map(0, &noRead, &mapped)))
    { LOG_ERROR("LoadITEXIntoTextureCube: upload Map failed"); return false; }

    uint8_t* base = static_cast<uint8_t*>(mapped);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t i = face * mipLevels + mip;
            const DirectX::Image* img = scratch.GetImage(mip, face, 0);
            if (!img) continue;
            uint8_t*     dstRows   = base + footprints[i].Offset;
            const size_t copyPitch =
                static_cast<size_t>(std::min<UINT64>(rowSizes[i], img->rowPitch));
            for (UINT row = 0; row < numRows[i]; ++row)
                std::memcpy(dstRows + row * footprints[i].Footprint.RowPitch,
                            img->pixels + row * img->rowPitch,
                            copyPitch);
        }
    upload->Unmap(0, nullptr);

    FlushAndWait();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    { LOG_ERROR("LoadITEXIntoTextureCube: command list alloc failed"); return false; }

    const D3D12_RESOURCE_STATES homeState = ToD3D12ResourceState(currentState);

    std::vector<D3D12_RESOURCE_BARRIER> toCopy(subCount), backState(subCount);
    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_RESOURCE_BARRIER& b = toCopy[i];
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = homeState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = firstSub + i;
        backState[i] = b;
        std::swap(backState[i].Transition.StateBefore, backState[i].Transition.StateAfter);
    }
    cl->ResourceBarrier(subCount, toCopy.data());

    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[i];
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = res;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = firstSub + i;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    cl->ResourceBarrier(subCount, backState.data());
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    { LOG_ERROR("LoadITEXIntoTextureCube: fence alloc failed"); return false; }
    m_commandQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    LOG_SUCCESS("LoadITEXIntoTextureCube: loaded '%s' into cube %u (%u mips)",
                path, cubeIndex, mipLevels);
    return true;
}
