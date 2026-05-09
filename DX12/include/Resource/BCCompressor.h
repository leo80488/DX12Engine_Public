#pragma once

// BCCompressor — singleton that owns a dedicated D3D11 device used exclusively
// for GPU-accelerated BC texture compression via DirectXTex.
//
// Why D3D11?
//   DirectXTex's GPU compress path (Compress overload taking ID3D11Device*)
//   is implemented with D3D11 compute shaders internally.  A separate device
//   avoids touching the engine's D3D12 device or its descriptor heaps.
//
// BC6H / BC7 GPU vs CPU (typical):
//   BC7  CPU quick   ~ 2–10 sec / 1024×1024   GPU ~ 50 ms
//   BC6H CPU         ~ 5–15 sec / 1024×1024   GPU ~ 80 ms
//   BC4 / BC5 CPU is already fast; GPU path is used for consistency.
//
// Thread safety:
//   Compress() acquires an internal mutex — safe to call from multiple
//   ResourceManager worker threads concurrently.

#include <d3d11.h>
#include <DirectXTex.h>
#include <wrl/client.h>
#include <mutex>

namespace Resource
{
    class BCCompressor
    {
    public:
        // Returns the process-wide singleton.
        static BCCompressor& Get();

        // GPU-accelerated block compress.  Falls back to CPU (parallel + BC7_QUICK)
        // if the D3D11 device is unavailable or GPU compress fails.
        HRESULT Compress(const DirectX::Image*        srcImages,
                         size_t                        nimages,
                         const DirectX::TexMetadata&   metadata,
                         DXGI_FORMAT                   format,
                         DirectX::ScratchImage&        outImage);

        bool IsGPUAvailable() const { return m_device != nullptr; }

        // Release the D3D11 device.  Call before process exit if desired.
        void Shutdown();

    private:
        BCCompressor();
        ~BCCompressor() = default;
        BCCompressor(const BCCompressor&)            = delete;
        BCCompressor& operator=(const BCCompressor&) = delete;

        void InitDevice();

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        std::mutex                           m_mutex;
    };
}
