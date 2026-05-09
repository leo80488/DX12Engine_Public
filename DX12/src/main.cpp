// DX12 entry point: delegated to the App class

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stdexcept>

#include "System/Log.h"
#include "App.h"
#include "System/Exception_handle.h"
#include "System/TaskSystem.h"

#ifdef _DEBUG
#include <wrl/client.h>
#include <dxgidebug.h>
#include <dxgi1_3.h>
#pragma comment(lib, "dxguid.lib")
using Microsoft::WRL::ComPtr;
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    Logger::Get().Initialize("DX12Log.txt");
    LOG_INFO("Application started");

    int exitCode = 0;
    try
    {
        App app;
        exitCode = app.Run();
    }
    catch (const ExceptionHandle& e)
    {
        LOG_ERROR(e.what());
        MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(e.what());
        MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    LOG_INFO("Application exiting normally");
    TaskSystem::Get().Shutdown();

#ifdef _DEBUG
    // After App and all DX12 objects are destroyed, check for true app-level leaks.
    // DXGI_DEBUG_RLO_IGNORE_INTERNAL filters out objects the debug layer holds
    // for its own bookkeeping, so only genuine unreleased objects appear.
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            dxgiDebug->ReportLiveObjects(
                DXGI_DEBUG_ALL,
                DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif

    Logger::Get().Shutdown();

    return exitCode;
}
