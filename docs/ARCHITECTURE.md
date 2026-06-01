**English** | [日本語](ARCHITECTURE.ja.md)

# Architecture

| Layer            | Role                                                                            |
| ---------------- | ------------------------------------------------------------------------------- |
| **ECS World**    | All entity / component data — pure game-side state.                             |
| **Scheduler**    | 16-phase, dependency-aware parallel system tick over the ECS World.             |
| **Renderer**     | Translates ECS data into GPU-ready `DrawPacket`s (no DX12 types). Split across `Renderer.cpp` + `Renderer_Scene/_DDGI/_IBL/_Terrain/_Accessors.cpp`. |
| **RenderGraph**  | Lambda- and class-based pass DAG, transient resources, automatic barrier injection. |
| **RenderPass**   | Individual passes; consume RHI types only.                                      |
| **RHI**          | Abstract command list & resource handles (`RHICommandList`, `RGTextureHandle`). |
| **DX12 Backend** | Concrete D3D12 implementation hidden behind `IGraphicsDevice` (`GraphicsDX12.cpp` + `_Resources/_Translation/_Capture.cpp`). |

## System Scheduler

- **16 ordered tick phases** (`TickPhase`): Input → Gameplay (Pre/Logic/Post) → AI → FixedPhysics (Pre/Step/Post) → PhysicsInterpolation → Animation → BoneAttachment → SecondaryPhysics → PreRender → Render → PostRender.
- Each phase descriptor flags `isFixedTimestep` / `allowsParallel` / `requiresMainThread`. Phase boundaries are the **only** sync points.
- **Dependency-aware parallel batching**: systems declare `Read<T>` / `Write<T>` / `ExclusiveResource(id)` access; a generation-cached linear-greedy planner groups non-conflicting systems into batches dispatched on the `TaskSystem`.
- **Deferred structural edits** — `CommandBuffer` queues `AddComponent` / `RemoveComponent` / `DestroyEntity` and flushes at the phase boundary so phase N+1 observes a consistent world.
- `FrameContext` separates unscaled `deltaTime` from `scaledDeltaTime` (hit-stop / bullet-time) and carries `physicsAlpha`, frame indices, and the editor play-state gate.

## Frame & Resource Management

- Triple-buffered swap chain (`FrameCount = 3`), separate HDR scene render target for the editor viewport.
- **Four** descriptor-heap allocators (RTV / DSV / CBV-SRV-UAV / Sampler), each with static free-list recycling plus a fence-reclaimed dynamic ring region for per-frame descriptor tables.
- Persistently-mapped UPLOAD heap ring buffers for per-frame data (instance, light, material, terrain, shadow VP, indirect args).
- **Multi-queue command-list model**: `BeginCommandList(queue)` is thread-safe; passes declare ordering via `AddCommandListDependency`. `EndFrame` Kahn-topological-sorts the recorded lists and inserts per-queue GPU fence Wait/Signal **only** across queue boundaries (graphics / compute / copy).
- **Cross-queue async compute**: `SetExternalWait(pass, depCL)` lands a one-shot per-frame GPU wait at exactly the consumer pass — e.g. the DDGI async-compute list resolves on `LightingPass` while GBuffer / Shadow / SkyIBL keep overlapping.
- Slot-based per-frame deferred release **plus** fence-keyed deferred release for async-compute resources that outlive 3 frames; transient placed-resource aliasing (`CreateTexturePlaced`).
- PSO library cache on disk (`pso_cache.bin`) and DXIL shader blob cache (`shaders/shader_cache_dxil/`).
- Reverse-Z depth across the entire engine.

## Shader Pipeline

- DXC (DXIL, SM 6.6, HV 2018; `lib_6_5` for RT) for **all** HLSL — `D3DCompile` is fully retired.
- `.ishdr` blob cache keyed by source + permutation + entry, invalidated by transitive `#include` mtime staleness.
- Shader hot-reload via `ReadDirectoryChangesW` + per-pass `ReloadShaders()` virtual.
- Negative-cache for failed DXC compiles + failed PSO creates so a broken shader doesn't spam the log every frame.

## Frame Execution

`RenderGraph::Execute` runs each frame in three phases:

1. **Serial setup** — open one command list per pass, set the builtin target, emit barriers (`EmitBarriersBeforePass` via a per-virtual-texture state tracker), chain same-queue + external cross-queue dependencies, open a GPU timestamp region.
2. **Parallel recording** — passes record concurrently via `TaskSystem::ParallelFor` (threshold ≥ 4 passes; serial below).
3. **Serial close** — close timestamp regions; `GPUProfiler` reports per-queue timings and a critical-path "effective" frame ms (max over graphics / compute / copy).

## Notable Engineering Details

- **Bindless everything** — meshes (per-attribute `ByteAddressBuffer` SRVs via a 16384-slot `g_Buffers[]` table), materials (texture indices in `MaterialGPUData`), shadow cascades, reflection probes, DDGI volumes.
- **Custom shader path on transparent materials** — `TransparentPass` honors `dp.customPSID`, so any additive-blend material can use a custom PS (energy fields, holograms, beam outer-glow).
- **Material schema** — single `kMaterialPBRSchema` table drives serializer + inspector; adding an editable field is a one-line change.
- **PSO permutation cache** (`PermutationKey` keyed by shader-define mask + render-state) over an on-disk `ID3D12PipelineLibrary`.
- **Pre-allocated GPU pools** for VFX (tracers, beams, particles, trails) so allocation cost is amortized at startup.
- **Outline / tracer / beam manual barrier patterns** — bypass the graph for the depth-as-SRV-and-DSV cases the auto-barrier tracker can't disambiguate.
- **Pragma-driven linking** — most third-party libs are linked inline via `#pragma comment(lib, ...)`; the build only needs the library search paths (see [BUILD.md](BUILD.md)).
