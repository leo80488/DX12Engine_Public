# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is a **Visual Studio 2022** project (MSVC v143 toolset, C++20, Windows 10 SDK). There is no CMake or Make build.

- **Build**: Open `DX12Engine.sln` in Visual Studio 2022, or use MSBuild:
  ```
  msbuild DX12Engine.sln /p:Configuration=Debug /p:Platform=x64
  msbuild Editor.vcxproj /p:Configuration=Debug /p:Platform=x64
  ```
- **Solution layout**: `DX12Engine.sln` builds 4 projects:
  - `EngineCore.vcxproj` (StaticLibrary) — engine code; produces `EngineCore.lib`
  - `Editor.vcxproj` (Application) — editor exe with ImGui + WITH_EDITOR define
  - `Game.vcxproj` (Application) — runtime-only exe (no editor)
  - `ShaderLab.vcxproj` (Application) — shader-authoring sandbox with WITH_SHADERLAB
  - Editor/Game/ShaderLab all depend on EngineCore.lib via `<ProjectReference>`.
- **`DX12.vcxproj` is legacy / not built**: it is *not* in `DX12Engine.sln` and duplicates files from EngineCore + Editor. Do not use it; build via the .sln instead.
- **Active configurations**: `Debug|x64` and `Release|x64`.
- **Build acceleration** (already configured in the 4 vcxprojs):
  - `/MP` MultiProcessorCompilation enabled — parallel cl.exe across cores.
  - PCH at `include/pch.h` + `src/pch.cpp` — force-included via `<ForcedIncludeFiles>pch.h</ForcedIncludeFiles>`. Holds windows.h / d3d12.h / DirectXMath / common STL. Project headers must NOT be added to pch.h or any header edit invalidates the PCH.
  - PCH excluded for: `external/lua/onelua.c` (C, not C++) and `external/imgui/*.cpp` + `ImGuizmo.cpp` (3rd-party).
  - `WholeProgramOptimization` (LTCG) **disabled** in Release for fast iteration. Re-enable only for shipping builds.
- **No automated tests** exist in this project.
- **Runtime**: Windows subsystem (`wWinMain`), not console. Output log is written to `DX12Log.txt` in the working directory.
- **F11** at runtime toggles between fullscreen viewport mode and the ImGui docking editor.

## Architecture Overview

### Entry Point & App Loop (`src/main.cpp`, `src/App.cpp`)
`wWinMain` initializes the async `Logger`, creates `App`, calls `App::Run()`. The `App` owns a `Window` (which owns `Graphics`), an `EditorLayer`, and two `RenderGraph` instances. The main loop is `Window::ProcessMessage()` → `Graphics::BeginFrame` → render graph execute → ImGui → `Graphics::EndFrame`.

### Graphics Abstraction Layer
**`include/Graphics/IGraphicsDevice.h`** — Platform-agnostic pure abstract interface. Exposes frame lifecycle (`BeginFrame`/`EndFrame`), ImGui integration, resize/fullscreen, viewport/render-target switching, and `GetHdrSceneSrvGpuHandle()` (returns `uint64_t`). `RenderGraph`, `RenderPass`, `App`, and `Window` all depend on this interface.

**`include/Graphics/GraphicsDX12.h` / `src/Graphics/GraphicsDX12.cpp`** — The Direct3D 12 implementation of `IGraphicsDevice`. Triple-buffered swap chain (`FrameCount = 3`), a single `ID3D12GraphicsCommandList`, and two descriptor heap allocators (`m_rtvAllocator`, `m_cbvSrvUavAllocator`). Adds DX12-specific non-virtual accessors: `GetDevice()`, `GetNativeCommandList()`, `GetRtvAllocator()`, `GetCbvSrvUavAllocator()`. There is a separate **HDR render target** (`m_hdrRenderTarget`) for the editor viewport. `ThrowIfFailed(hr)` / `ThrowIfFailedImpl` are defined here. `Window` owns a `std::unique_ptr<GraphicsDX12>` but `Gfx()` returns `IGraphicsDevice&`.

**`include/Graphics/Graphics.h`** — Compat shim only. Includes `IGraphicsDevice.h` + `GraphicsDX12.h` and defines `using Graphics = GraphicsDX12`. Existing code that uses `Graphics` continues to compile without changes.

**DX12-specific pass pattern**: `RenderPass::Init/Execute` take `IGraphicsDevice&`. Passes that need DX12 objects (device, command list) do: `auto& dx12 = static_cast<GraphicsDX12&>(gfx);` — safe within a DX12-only build.

### RHI Type Layer (`include/Graphics/GraphicsStruct.h`)
`namespace RHI` contains API-agnostic descriptor structs (`TextureDesc`, `GPUBufferDesc`, `PipelineStateDesc`, `SamplerDesc`, etc.) and enums (`Format`, `ResourceState`, `BindFlag`, etc.) mirroring Wicked Engine's RHI convention. These are used for future abstraction; the current `Graphics` class uses raw DX12 types directly.

### Render Graph (`include/RenderGraph/RenderGraph.h`, `src/RenderGraph/`)
`RG::RenderGraph` is a lightweight, linear pass manager:
1. `AddPass(unique_ptr<RenderPass>)` — register a pass
2. `Compile(gfx)` — calls `Setup()` (declare reads/writes via `RenderGraphBuilder`) and `Init()` (create PSO/resources) on each pass
3. `Execute(gfx, clearColor)` — calls `Execute()` on each pass in order

Passes target either `BuiltinTexture::SwapChainColor` or `BuiltinTexture::HdrSceneColor`. New passes inherit from `RG::RenderPass` and implement `GetName()`, `Setup()`, `Init()`, `Execute()`. See `TestTrianglePass` as the reference implementation.

### ECS (`include/ECS/ECS.h`, `include/ECS/Components.h`)
Minimal ECS: `Entity` is a `uint32_t`. `World` manages entity IDs with a free list. Components are plain structs (`MaterialComponent`, `MeshComponent`) stored externally — the World itself does **not** store component data; callers manage component arrays directly. `MeshComponent::CreateRenderData(Graphics*)` uploads vertex/index buffers to GPU.

### Resource System (`include/Resource/`, `src/Resoruce/`)
`Resource::ResourceManager` provides async asset loading:
- Call `Load(path)` → returns a `Handle` immediately; actual I/O runs on `TaskSystem` workers
- `GetState(handle)` polls `NotLoaded / Loading / Ready / Failed`
- GPU uploads are deferred: call `ProcessPendingGPUUploads(maxMs)` each frame on the main thread
- Loaders implement `IResourceLoader`; `TextureLoader` is provided (DDS via DirectXTex)
- Resources are ref-counted via `weak_ptr`; expired entries are automatically reloaded on next `Load()`

### Task System (`include/System/TaskSystem.h`, `src/System/TaskSystem.cpp`)
Singleton thread pool (`TaskSystem::Get()`). Spawns `N-1` worker threads. Two priority queues: `High` (physics/animation) processed before `Low` (resource I/O). Shut down explicitly via `TaskSystem::Get().Shutdown()` at program exit.

### Editor UI (`include/Editor/EditorLayer.h`, `src/Editor/`)
`EditorLayer` builds an ImGui docking layout: Hierarchy | Viewport (scene texture) | Inspector panels, plus a bottom Resource panel. `ImGuiManager` (`include/Editor/ImGuiManager.h`) wraps ImGui init/shutdown for DX12. The Viewport panel exposes play/pause/step-frame controls via `ViewportPlayState`.

### Scene Serialization (`include/SceneSerialization.h`, `src/SceneSerialization.cpp`)
POD mirror structs (`MaterialComponentData`, `MeshComponentData`) strip out all GPU/COM resources so scenes can be serialized. Supports both **binary** and **JSON** output via `SerializeToBinary`, `SerializeToJSON`, `SerializeToBoth`. Round-trip helpers: `FromMaterialComponent` / `ToMaterialComponent`.

### Supporting Systems
- **Logger** (`include/System/Log.h`): async singleton; producer enqueues via `LOG_INFO/WARNING/ERROR/SUCCESS`, a worker thread formats and writes to file + ImGui buffer. Use `LOG_HRESULT(expr, hr)` after DX12 calls.
- **DescriptorHeapAllocator** (`include/Graphics/DescriptorHeapAllocator.h`): single Tier1 heap with free-list recycling. `DescriptorAllocation::Free()` returns slots for reuse.
- **Allocators** (`include/System/allocator.h`): `Allocator::BlockAllocator<T>` (pool, 256-slot blocks) and `Allocator::LinearAllocator` (bump allocator).
- **Window** (`include/System/Window.h`): Win32 window; owns `Graphics`; handles `WM_SIZE` → `Graphics::Resize`, Alt+Enter → `Graphics::SetFullscreen`.

## Key Conventions

- `#define NOMINMAX` must appear before any Windows headers to avoid macro collisions with `std::min`/`std::max`. It is guarded with `#ifndef NOMINMAX` in most headers.
- All DX12 HRESULT calls use `ThrowIfFailed(expr)` (defined in `Graphics.h`), which throws `ExceptionHandle` with file/line info.
- The `RHI::` namespace enums/structs are **descriptors only** — no DX12 objects. The actual `Graphics` class uses raw `Microsoft::WRL::ComPtr<>` members.
- Include paths (set in `.vcxproj`): `$(ProjectDir)external`, `$(ProjectDir)external\DirectXTex`, `$(ProjectDir)include`.
- External libs: `DirectXTex_Debug.lib` / `DirectXTex_Release.lib` (prebuilt in `external/DirectXTex/lib/`), `d3d12.lib`, `dxgi.lib`, `d3dcompiler.lib` (via `#pragma comment(lib, ...)`).
