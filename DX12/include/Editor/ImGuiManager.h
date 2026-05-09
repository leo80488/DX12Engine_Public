#pragma once

// ImGui lifecycle and DX12/Win32 backend management
// - Context is created externally before Initialize() (by Window)
// - Initialize sets up Win32 + DX12 backends; Shutdown tears down backends and platform windows
// - DestroyContext() must be called by the owner after DestroyWindow

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <cstddef>

#include "Graphics/DescriptorHeapAllocator.h"

class ImGuiManager
{
public:
    ImGuiManager() = default;
    ~ImGuiManager() = default;
    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    /** Initialize: requires an existing ImGui context (e.g., Window already called CreateContext). Sets up Win32 + DX12 backends. */
    void Initialize(
        HWND hWnd,
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        DescriptorHeapAllocator& srvAllocator,
        UINT frameCount
    );

    /** Shutdown backends and platform windows; does not destroy the context (call DestroyContext() after DestroyWindow). */
    void Shutdown();

    /** Destroy the ImGui context; must be called after DestroyWindow(hwnd). */
    void DestroyContext();

    /** Update stage: NewFrame (Win32 + DX12 + ImGui::NewFrame). */
    void BeginFrame();

    /** Final render pass before Present: Render + ImGui_ImplDX12_RenderDrawData. */
    void Render(ID3D12GraphicsCommandList* commandList, DescriptorHeapAllocator& srvAllocator);

    bool IsInitialized() const { return m_initialized; }

    /** For ImGui DX12 backend callbacks (SrvDescriptorAllocFn / SrvDescriptorFreeFn). */
    void AddSrvAllocation(DescriptorAllocation a);
    void FreeSrvAllocation(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    DescriptorHeapAllocator* GetSrvAllocator() const { return m_srvAllocator; }

private:

    bool m_initialized{ false };
    DescriptorHeapAllocator* m_srvAllocator{ nullptr };
    std::vector<std::pair<std::size_t, DescriptorAllocation>> m_srvAllocations;
};
