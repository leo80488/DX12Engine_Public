#include "Resource/BCCompressor.h"
#include "System/Log.h"

#pragma comment(lib, "d3d11.lib")

namespace Resource
{
    // -------------------------------------------------------------------------
    BCCompressor& BCCompressor::Get()
    {
        static BCCompressor instance; // constructed once, thread-safe (C++11)
        return instance;
    }

    // -------------------------------------------------------------------------
    BCCompressor::BCCompressor()
    {
        InitDevice();
    }

    // -------------------------------------------------------------------------
    void BCCompressor::InitDevice()
    {
        // Try hardware device first, fall through to WARP on failure.
        const D3D_DRIVER_TYPE driverTypes[] =
        {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,   // software fallback still faster than CPU BC7
        };

        // BC6H / BC7 require feature level 11.0.
        const D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        for (D3D_DRIVER_TYPE dt : driverTypes)
        {
            D3D_FEATURE_LEVEL actualLevel{};
            HRESULT hr = D3D11CreateDevice(
                nullptr,                   // default adapter
                dt,
                nullptr,                   // no software module
                0,                         // no debug / other flags
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                m_device.GetAddressOf(),
                &actualLevel,
                nullptr                    // immediate context not needed
            );

            if (SUCCEEDED(hr))
            {
                const char* dtName = (dt == D3D_DRIVER_TYPE_HARDWARE) ? "hardware" : "WARP";
                LOG_SUCCESS("BCCompressor: D3D11 %s device ready (FL 0x%X) for GPU BC compression",
                            dtName, static_cast<unsigned>(actualLevel));
                return;
            }
        }

        LOG_WARNING("BCCompressor: D3D11 device creation failed — BC compression will use CPU fallback");
        m_device = nullptr;
    }

    // -------------------------------------------------------------------------
    HRESULT BCCompressor::Compress(const DirectX::Image*       srcImages,
                                    size_t                       nimages,
                                    const DirectX::TexMetadata&  metadata,
                                    DXGI_FORMAT                  format,
                                    DirectX::ScratchImage&       outImage)
    {
        // --- GPU path (serialised; DX11 immediate context is not thread-safe) --
        if (m_device)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            HRESULT hr = DirectX::Compress(
                m_device.Get(),
                srcImages, nimages, metadata,
                format,
                DirectX::TEX_COMPRESS_DEFAULT,
                DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
                outImage);

            if (SUCCEEDED(hr))
                return hr;

            LOG_WARNING("BCCompressor: GPU Compress failed (fmt=0x%X hr=0x%08X), falling back to CPU",
                        static_cast<unsigned>(format),
                        static_cast<unsigned>(hr));
        }

        // --- CPU fallback (parallel threads + BC7_QUICK for BC7) --------------
        DirectX::TEX_COMPRESS_FLAGS cpuFlags = DirectX::TEX_COMPRESS_PARALLEL;
        if (format == DXGI_FORMAT_BC7_UNORM ||
            format == DXGI_FORMAT_BC7_UNORM_SRGB)
            cpuFlags |= DirectX::TEX_COMPRESS_BC7_QUICK;

        return DirectX::Compress(
            srcImages, nimages, metadata,
            format, cpuFlags,
            DirectX::TEX_THRESHOLD_DEFAULT,
            outImage);
    }

    // -------------------------------------------------------------------------
    void BCCompressor::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device.Reset();
        LOG_INFO("BCCompressor: shutdown");
    }
}
