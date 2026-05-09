#include "Editor/ImGuiManager.h"
#include "System/Log.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
#include <algorithm>

namespace
{
    void SrvAllocFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
    {
        ImGuiManager* mgr = static_cast<ImGuiManager*>(info->UserData);
        DescriptorAllocation a = mgr->GetSrvAllocator()->Allocate(1);
        a.CopyToGpu();
        *out_cpu = a.GetGpuCpuHandle();   // GPU-heap CPU handle for ImGui direct writes
        *out_gpu = a.GetGpuHandle();
        mgr->AddSrvAllocation(std::move(a));
    }

    void SrvFreeFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE /*gpu_handle*/)
    {
        ImGuiManager* mgr = static_cast<ImGuiManager*>(info->UserData);
        mgr->FreeSrvAllocation(cpu_handle);
    }
}

void ImGuiManager::Initialize(
    HWND hWnd,
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    DescriptorHeapAllocator& srvAllocator,
    UINT frameCount)
{
    if (m_initialized)
        return;

    m_srvAllocator = &srvAllocator;

    if (!ImGui_ImplWin32_Init(hWnd))
    {
        LOG_ERROR("ImGui Win32 init failed");
        return;
    }

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = commandQueue;
    info.NumFramesInFlight = frameCount;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = this;
    info.SrvDescriptorHeap = srvAllocator.GetGpuHeap();
    info.SrvDescriptorAllocFn = SrvAllocFn;
    info.SrvDescriptorFreeFn = SrvFreeFn;

    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        LOG_ERROR("ImGui DX12 init failed");
        return;
    }

    m_initialized = true;
}

void ImGuiManager::Shutdown()
{
    if (!m_initialized)
        return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyPlatformWindows();
    m_srvAllocations.clear();
    m_srvAllocator = nullptr;
    m_initialized = false;
}

void ImGuiManager::DestroyContext()
{
    ImGui::DestroyContext();
}

void ImGuiManager::BeginFrame()
{
    if (!m_initialized)
        return;
    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::Render(ID3D12GraphicsCommandList* commandList, DescriptorHeapAllocator& srvAllocator)
{
    if (!m_initialized)
        return;
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = { srvAllocator.GetGpuHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void ImGuiManager::AddSrvAllocation(DescriptorAllocation a)
{
    m_srvAllocations.push_back({ a.GetGpuCpuHandle().ptr, std::move(a) });
}

void ImGuiManager::FreeSrvAllocation(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
    const std::size_t ptr = cpuHandle.ptr;
    auto it = std::find_if(m_srvAllocations.begin(), m_srvAllocations.end(),
        [ptr](const std::pair<std::size_t, DescriptorAllocation>& p) { return p.first == ptr; });
    if (it != m_srvAllocations.end())
    {
        it->second.Free();
        m_srvAllocations.erase(it);
    }
}
