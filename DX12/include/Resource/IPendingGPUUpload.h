#pragma once

// Pending GPU upload work: prepared on a background thread, then Execute() is called next frame on the main/render thread.
// Concrete implementations are provided by the renderer (e.g., create ID3D12Resource and copy D3D12_SUBRESOURCE_DATA).
namespace Resource
{
    class IPendingGPUUpload
    {
    public:
        virtual ~IPendingGPUUpload() = default;
        /** Runs on the main/render thread to perform the GPU upload (CreateResource + Copy, etc.). */
        virtual void Execute() = 0;
    };
}
