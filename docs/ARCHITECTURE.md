# Architecture

| Layer            | Role                                                                            |
| ---------------- | ------------------------------------------------------------------------------- |
| **ECS World**    | All entity / component data — pure game-side state.                             |
| **Renderer**     | Translates ECS data into GPU-ready `DrawPacket`s (no DX12 types).               |
| **RenderGraph**  | Lambda-based pass DAG, transient resources, automatic barrier injection.        |
| **RenderPass**   | Individual passes; consume RHI types only.                                      |
| **RHI**          | Abstract command list & resource handles (`RHICommandList`, `RGTextureHandle`). |
| **DX12 Backend** | Concrete D3D12 implementation hidden behind `IGraphicsDevice`.                  |

## Frame & Resource Management

- Triple-buffered swap chain (`FrameCount = 3`), separate HDR scene render target for the editor viewport.
- Two descriptor-heap allocators (RTV / CBV-SRV-UAV) with free-list recycling.
- Persistently-mapped UPLOAD heap ring buffers for per-frame data (instance, light, material, terrain, shadow VP, indirect args).
- PSO library cache on disk (`pso_cache.bin`) and DXIL shader blob cache (`shaders/shader_cache_dxil/`).
- Reverse-Z depth across the entire engine.

## Shader Pipeline

- DXC (DXIL, SM 6.6, HV 2018) for **all** HLSL — `D3DCompile` is fully retired.
- Shader hot-reload via `ReadDirectoryChangesW` + per-pass `ReloadShaders()` virtual; transitive `#include` mtime tracking.
- Negative-cache for failed DXC compiles + failed PSO creates so a broken shader doesn't spam the log every frame.

## Notable Engineering Details

- **Bindless everything** — meshes (per-attribute SRVs), materials (texture indices in `MaterialGPUData`), shadow cascades, reflection probes, DDGI volumes.
- **Custom shader path on transparent materials** — `TransparentPass` honors `dp.customPSID`, so any additive-blend material can use a custom PS (energy fields, holograms, beam outer-glow).
- **Material schema** — single `kMaterialPBRSchema` table drives serializer + inspector; adding an editable field is a one-line change.
- **Per-pass dedicated render-worker threads** with binary semaphores (shadow / compute pools).
- **PSO permutation cache** (`PermutationKey` keyed by shader-define mask + render-state).
- **Pre-allocated GPU pools** for VFX (4096 tracers, 32 beams) so allocation cost is amortized at startup.
- **Outline / tracer / beam manual barrier patterns** — bypass the graph for the depth-as-SRV-and-DSV cases the auto-barrier tracker can't disambiguate.
