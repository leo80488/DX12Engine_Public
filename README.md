# DX12Engine 
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-20-54" src="https://github.com/user-attachments/assets/c2865b65-ad47-487d-9a8b-43042034a2f0" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-31-25" src="https://github.com/user-attachments/assets/3b6ea120-9fa1-4b4b-a96d-f1d7775dcb7a" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-55-46" src="https://github.com/user-attachments/assets/9c14df46-96ef-4418-9329-eca21c4c4903" />

![Built with Claude Code]
A real-time rendering engine and editor written in C++20 / Direct3D 12 (Shader Model 6.6, DXIL via DXC).

> Built against Visual Studio 2022, Windows 10 SDK, MSVC v143, C++20.

---

## 1. Core Architecture

| Layer            | Role                                                                       |
| ---------------- | -------------------------------------------------------------------------- |
| **ECS World**    | All entity / component data — pure game-side state.                        |
| **Renderer**     | Translates ECS data into GPU-ready `DrawPacket`s (no DX12 types).          |
| **RenderGraph**  | Lambda-based pass DAG, transient resources, automatic barrier injection.   |
| **RenderPass**   | Individual passes; consume RHI types only.                                 |
| **RHI**          | Abstract command list & resource handles (`RHICommandList`, `RGTextureHandle`). |
| **DX12 Backend** | Concrete D3D12 implementation hidden behind `IGraphicsDevice`.             |

- Triple-buffered swap chain (`FrameCount = 3`), separate HDR scene render target for the editor viewport.
- Two descriptor-heap allocators (RTV / CBV-SRV-UAV) with free-list recycling.
- Persistently-mapped UPLOAD heap ring buffers for per-frame data (instance, light, material, terrain, shadow VP, indirect args).
- PSO library cache on disk (`pso_cache.bin`) and DXIL shader blob cache (`shaders/shader_cache_dxil/`).
- Shader hot-reload via `ReadDirectoryChangesW` + per-pass `ReloadShaders()` virtual; transitive `#include` mtime tracking.
- Negative-cache for failed DXC compiles + failed PSO creates so a broken shader doesn't spam the log every frame.
- DXC (DXIL, SM 6.6, HV 2018) for **all** HLSL — `D3DCompile` is fully retired.

---

## 2. Rendering Pipeline

### 2.1 Geometry / Visibility

- **Deferred G-Buffer** (Albedo / Normal / Surface / Emissive / Depth / Velocity).
- **PVF (Per-Vertex Format) bindless geometry**: per-attribute `ByteAddressBuffer` SRVs through a
  bindless mesh descriptor heap (no input layouts, no per-mesh root sig changes).
- **GPU frustum & cluster culling** (`InstanceCull.cs`, `ClusterCull.cs`, `ClusterCullProbes.cs`).
- **Hi-Z depth pyramid** (reverse-Z, 2-channel min/max) — `HiZGenerate.cs`, `HiZReduce.cs`.
- **ExecuteIndirect** indirect-draw path with GPU-built draw count.
- Scene **BVH** rebuilt per frame for CPU-side queries (frustum tests, picking).
- Mesh-shader **terrain pipeline** (`Terrain.as / .ms / .ps`) with quadtree LOD and amplification-shader-driven CSM caster path.

### 2.2 Lighting

- **Clustered Forward+ / Clustered Deferred** light culling (`ClusterBuild.cs`, `ClusterCull.cs`).
- Punctual lights: directional, point, spot. Spot lights opt into shadows on demand.
- **Cascaded Shadow Maps (CSM)** — 3 cascades, 2048², practical-split (λ=0.85), tight bounding spheres,
  texel snapping, Halton(2,3) sub-texel jitter, comparison sampler.
- **Spot shadow atlas** for opt-in spot-light shadows.
- **Cook-Torrance microfacet BRDF** (GGX + Smith + Schlick) shared between opaque & transparent paths.

### 2.3 Global Illumination

- **DDGI (Dynamic Diffuse Global Illumination)** — DXR / inline `RayQuery` compute trace,
  L1 spherical-harmonics irradiance + 16² depth probes, multi-bounce, adaptive ray dispatch
  via `ExecuteIndirect`, probe relocation, Halton(2,3) sphere sampling with frame-rotated
  contiguous-block stride, damped random rotation, emissive surface support (bindless texture
  sampling at the closest hit). Tuned & verified on Sponza / Bistro.
- **Reflection Probes** — cube-array prefiltered specular probes baked at runtime
  (6 forward captures + 42 prefilter dispatches per probe).
- **Screen-Space Reflections (SSR)** — Hi-Z stochastic GGX trace ported from FidelityFX-SSSR /
  Wicked Engine: trace → resolve → temporal accumulate → upsample → composite,
  with confidence-based blend against reflection-probe fallback.
- **Sky Spherical Harmonics** — `SkySHProjection.cs` projects the dynamic sky into L2 SH for IBL diffuse.
- **Voxel scene** (`SceneVoxelize.cs`, `SceneVoxelClear.cs`) — voxel grid used as an occlusion source.

### 2.4 Atmosphere & Sky

- Hillaire 2020 sky model: `AtmosphereTransmittance.cs`, `AtmosphereMultiScatter.cs`,
  `AtmosphereSkyView.cs`, `AerialPerspective.cs`, `SkyAtmosphere.cs`.
- Pre-baked stars (`StarsBake.cs`), HDRI skybox visual override.
- IBL pipeline — irradiance cube + radiance cube + pre-integrated BRDF LUT (`SpecularPrefilter.cs`, `GenerateLUT.cs`).

### 2.5 Volumetrics

- **Froxel volumetric fog** — density / light-injection / temporal-reproject / scatter pipeline
  (`FroxelDensity.cs`, `FroxelLightInject.cs`, `FroxelTemporal.cs`, `FroxelScatter.cs`).
  Per-light volumetric contribution, TAA-aware history, voxel-occlusion gated.
- **Volumetric raymarch** for sun god-rays (`VolumetricRaymarch.cs`, `VolumetricRaymarchTemporal.cs`,
  `VolumetricRaymarchApply.ps`).

### 2.6 Post-Process Stack

- **Auto Exposure** — log-luminance histogram, eye adaptation.
- **Bloom** — 13-tap "Sledgehammer" downsample with Karis-average, 3×3 tent upsample.
- **Lens Flare** — procedural directional-light flare composited pre-tonemap.
- **TAA** — Karis 2014 / Salvi 2016 hybrid: nearest-depth velocity dilation, 9-tap Catmull-Rom history,
  variance clipping with luma gamma, soft-edge disocclusion, anti-flicker cross blur, Halton jitter.
- **XeGTAO** — verbatim port of Intel's XeGTAO main pass + depth pre-filter + denoise + temporal accumulation.
- **Tonemap** — final HDR → LDR with exposure & color-grading.
- **CAS** — AMD FidelityFX Contrast Adaptive Sharpening, LDS-tiled HDR-aware port.
- **Color Grading** parameter block.
- **Post-Process Volumes** — Unreal-style blendable volume system (`VolumeSystem`,
  `EntityVolumeSource`, `ParameterBlender`) with scripted overrides.

### 2.7 Special Effects

- **Decal system** — clustered, screen-space-projected decals with material library + clustered cull.
- **Outline** — three-sub-pass system: inverted-hull silhouette + Object-ID + screen-space Roberts
  edge cross on normal/depth/ID.
- **Glass-shatter** — captures tonemap output, simulates shard physics, composites until duration elapses.
- **Particle system** — GPU emit/update compute pipeline, ring-buffer pool, indirect-draw render.
- **Trail system** — GPU-driven ribbon trails, control-point compute update.
- **Tracer system** — cylindrical-billboard "thin laser" tracers, GPU pool (4096 slots).
- **Beam system** — CS-generated procedural-tube heavy beams, parallel-transport frame, Rodrigues' rotation,
  optional perpendicular wobble. Inner-core opaque + outer-glow additive shaders.
- **Skinned animation** — GPU skinning compute (`Skin.cs`), pose ring buffer, morph targets, IK,
  socket attachments, follow-entity / follow-socket components.
- **Chain physics** — bone-chain spring physics on GPU (`ChainPhysics.cs`).

### 2.8 World-Space & Screen-Space UI

- Strictly separated subsystems sharing **no** components.
- **Screen-space**: `UISystem` widget tree (`UIRootComponent`) plus flat-ECS items
  (`UIScreenSpace/Image/Text/Bar`). Renders through `UIPass` with cmd-merging by texture/material/clip.
- **World-space**: `WorldSpaceUIComponent` + content components (`WorldUIBar`, `WorldUIText`,
  `WorldUIImage`, `DamageNumberComponent`). Billboarded via `WorldUIBillboardPass`. Entity-driven with
  `LocalTransform` + optional `FollowEntity`.

### 2.9 Other

- **Picking** (`PickingPass`, `PickingID.vs/.ps`) — GPU readback → entity ID resolution.
- **Debug wireframe** — `DebugWirePass` for AABB / frustum / spline overlays.
- **DDGI probe debug visualization** — instanced spheres positioned at probe trace origins.
- **Reverse-Z** depth across the entire engine.

---

## 3. Render-Pass Catalog (in `include/RenderGraph/RenderPass/`)

| Category       | Passes                                                                                       |
| -------------- | -------------------------------------------------------------------------------------------- |
| **Geometry**   | GBuffer, Terrain, Skybox, Transparent, Picking, DebugWire                                    |
| **Shadows**    | Shadow (CSM), SpotShadow                                                                     |
| **Lighting**   | Lighting, Decal, SkyIBL                                                                      |
| **GI**         | DDGI, DDGIProbeDebug, ReflectionProbeCapture, SceneVoxel                                      |
| **SSR**        | SSR (trace), SSRResolve, SSRTemporal, SSRUpsample, SSRComposite, SSRDepthHierarchy, SceneColorPyramid |
| **Volumetric** | VolumetricFog (4-pass froxel)                                                                |
| **Culling**    | Culling, Cluster, HiZ                                                                        |
| **Skinning**   | Skinning                                                                                     |
| **VFX**        | Particles, Trails, Tracers (sim+render), BeamSim, GlassShatter                               |
| **Outline**    | Outline (3 sub-passes)                                                                       |
| **Post**       | TAA, AutoExposure, Bloom, LensFlare, XeGTAO, CAS, ToneMap, ColorGrading                      |
| **UI**         | UI (screen), WorldUIBillboard                                                                |

---

## 4. Engine Systems

### 4.1 ECS
Minimal data-oriented ECS (`ECS::World`, pool-per-component, free-list entity IDs).
Components include scene-graph (`Parent`, `Children`, `LocalTransform`, `GlobalTransform`,
`Visibility`, `RenderLayer`, `WorldAabb`), animation, physics, AI, audio, UI, decal, terrain,
follow / socket, beam / particle / trail, reflection-probe / DDGI volume.

### 4.2 Resource System (offline cook + runtime handle)
- **Importers** — Mesh / Material / Shader / Texture / Animation / Skeleton / Scene /
  PMX / VMD / VRM (MMD support) / Decal-Material / Audio.
- **Cooked formats** — `.imsh`, `.imat`, `.itex`, `.ishdr`, `.ianim`, `.iskel`, `.iscn`, `.iworld`.
- **Runtime systems** — `TextureSystem` (path → GPU texture refcount, BC compression),
  `MeshSystem` (DEFAULT-heap VB/IB, interleaved POS+NOR+UV 32B), `MaterialSystem` (CBV ring upload),
  `AnimationClipSystem`, `MeshLibrary`, `DecalMaterialLibrary`.
- **Async loading** — `ResourceManager` returns handle immediately; I/O on `TaskSystem` workers;
  GPU uploads deferred to main thread (`ProcessPendingGPUUploads`).

### 4.3 Scene Graph
- `SceneLoader` (Assimp), `SceneImporter` (offline cook → `.iscn` + `.imsh` set), `SceneInstanceLoader` (runtime, no Assimp).
- `TransformSystem::Propagate` — BFS hierarchy update, propagates `Visibility::inherited_hidden`,
  recomputes `WorldAabb`.

### 4.4 Animation
- Skeletal — Assimp / VMD / VRM clip import, blend trees, morph targets, IK, sockets, Follow-entity / Follow-socket.
- `SkinnedMeshSubsystem` + `SkinningPass` (compute), `PoseRingBuffer` for triple-buffered skinning.
- `BoneCloth` / `ChainPhysics` (spring bones for hair, skirts, chests).

### 4.5 Physics
- **Jolt Physics** integration (60 Hz fixed step, accumulator inside `PhysicsSystem::Update`).
- `RigidBodyComponent` + `ColliderComponent` / `CapsuleColliderComponent`. Lazy Jolt body creation.
- Independent **chain-physics CS** for cosmetic bone simulation (no Jolt cost).

### 4.6 Audio
- **XAudio2 + X3DAudio** (`AudioEngine`) with submix bus per `BusType` (Music / SFX / Voice / Ambient / UI).
- `AudioClipSystem` deduplicates clips by path; `Audio3DSystem` handles spatialisation.
- `AudioImporter` cooks to `.iaud`.

### 4.7 AI
- **Behavior Tree** runtime (`BTAsset`, `BTNode`, `BTNodes`, `ActionRegistry`).
- BT trees authored in **Lua**, parsed into a C++ node tree, hot-reloadable.
- Shares the `ScriptSystem` Lua VM; AI ticks after scripts and before physics.
- **AI LOD** system (`AILODSystem`) for distance-based tick-rate reduction.

### 4.8 Scripting
- **Lua 5.4** + **sol2** binding.
- Per-entity `ScriptComponent`, global scripts (`AddGlobalScript`).
- Hot-reload via `FileWatcher`.
- Time-scale support (`SetTimeScale`) for hit-stop / bullet-time without freezing real-time timers.
- `Engine.AfterDelay` callbacks tick on real (unscaled) dt.
- Lua bindings for math types, UI, BT actions, ECS commands.

### 4.9 Reflection
- `Reflect::Descriptor<T>` + `REFLECT_BEGIN/END` macros — declarative struct-field metadata.
- Drives the editor's component inspector (auto-generated ImGui widgets, eliminates ~30 hand-written blocks).
- Field kinds: scalar / color3+4 / enum / bool / string / quaternion-as-Euler / direction-as-AzEl / array / vector / drag-drop string / collapsing header.
- `MaterialSchema` — same idea applied to material params; `MaterialSerializer` is schema-driven (one-line addition for a new editable field).
- Custom-shader inspector reads DXC shader reflection (`ShaderReflect::Reflection`), auto-builds widgets for cbuffer vars and texture slots.

### 4.10 Editor (`Editor.exe`)
- **ImGui docking** layout: Hierarchy / Viewport / Inspector / Resource / Log.
- **Asset Browser** with filesystem scan (5 s polling), live texture thumbnails, FA icons for non-texture assets, drag-drop into scene / inspector.
- **Material Inspector** — schema-driven engine PBR/NPR/Unlit, plus reflection-driven custom-shader params.
- **Component Inspector** — descriptor-driven for pure-data components, hybrid postDraw for buttons / texture pickers / conditional UI.
- **Hierarchy Tree** with Parent/Children rendering.
- **Inline gizmo** (ImGuizmo) for translate / rotate / scale.
- **Play / Pause / Step-Frame** scene controls.
- **Frame Capture to PNG** (`Renderer::CaptureViewportToPNG` → DirectXTex `SaveToWICFile`).
- **Reflection-Probe Bake** + **Glass-Shatter Trigger** + per-pass debug toggles.
- **NPR Ramp Editor** (ShaderLab v0.4) — 512×25 RGBA atlas with two-stop gradient, live binding into the active material's RAMPMAP slot via bindless index.
- **Timeline Editor** (custom ImGui control).

### 4.11 ShaderLab (`ShaderLab.exe`)
- Reuses the entire `EngineCore.lib` + `EditorLayer`.
- Boots into a sphere + 3-point lights + IBL scene tuned for shader iteration.
- HDRI swap from `asset/IBL/<name>/`, mesh swap (Cube / Sphere / Cone), turntable orbit.
- Drag a `.ps.hlsl` onto the Shader Path field → `MaterialReflectionSync` auto-populates the inspector.
- Sub-second iteration loop verified end-to-end.

### 4.12 Tools
- **Asset packer** (`tools/pack_assets.py`) — cook + bundle assets.
- **Game packager** (`tools/package_game.py`) — pack `Game.exe` + cooked assets for distribution.

---

## 5. Notable Engineering Details

- **Bindless everything** — meshes (per-attribute SRVs), materials (texture indices in `MaterialGPUData`), shadow cascades, reflection probes, DDGI volumes.
- **Custom shader path on transparent materials** — `TransparentPass` honors `dp.customPSID`, so any additive-blend material can use a custom PS (energy fields, holograms, beam outer-glow).
- **Material schema** — single `kMaterialPBRSchema` table drives serializer + inspector; adding an editable field is a one-line change.
- **Per-pass dedicated render-worker threads** with binary semaphores (shadow / compute pools).
- **PSO permutation cache** (`PermutationKey` keyed by shader-define mask + render-state).
- **Pre-allocated GPU pools** for VFX (4096 tracers, 32 beams) so allocation cost is amortized at startup.
- **Outline / tracer / beam manual barrier patterns** — bypass the graph for the depth-as-SRV-and-DSV cases the auto-barrier tracker can't disambiguate.

---

## 6. Third-Party Libraries & References

### 6.1 Libraries shipped under `external/`

| Library                 | Use                                                         | License (upstream)        |
| ----------------------- | ----------------------------------------------------------- | ------------------------- |
| **DirectX-Headers**     | Up-to-date `d3d12.h` + Agility SDK headers                  | MIT                       |
| **DirectXTex**          | Texture loading, BC compression, WIC save (PNG capture)     | MIT                       |
| **DXC (dxcompiler.dll, dxil.dll)** | HLSL → DXIL compilation, reflection                | LLVM (Apache 2.0)         |
| **Dear ImGui** (+ ImGuizmo) | Editor UI, gizmo                                        | MIT                       |
| **Jolt Physics**        | Rigid-body physics                                          | MIT                       |
| **Lua 5.4**             | Scripting VM                                                | MIT                       |
| **sol2**                | C++ ↔ Lua binding                                           | MIT                       |
| **Assimp**              | Mesh / scene / skeleton / animation import (offline cook)   | BSD-3-Clause              |
| **FreeType**            | Font rasterization for the UI                               | FTL / GPL                 |
| **nlohmann/json**       | Scene serialization, post-process volume blobs              | MIT                       |
| **XAudio2 / X3DAudio**  | Audio mixer + 3D spatialisation                             | Windows SDK               |

> Assimp is consumed via **vcpkg** (`assimp:x64-windows`).
> DXC binaries are copied from Windows SDK 10.0.26100.0.

### 6.2 Algorithms / Techniques (with attributions)

| Subsystem            | Reference                                                                                          |
| -------------------- | -------------------------------------------------------------------------------------------------- |
| **PBR / BRDF**       | Cook-Torrance 1982, Disney BRDF (Burley 2012), Epic UE4 PBR (Karis 2013), Schlick Fresnel approximation |
| **CSM**              | Practical-Split (NVIDIA), bounding-sphere stabilisation (Valient 2012), texel snapping, Halton jitter |
| **DDGI**             | RTXGI / NVIDIA *Dynamic Diffuse Global Illumination* (Majercik et al. 2019), L1 SH probes, Wicked Engine adaptive ray-bucket scheme |
| **SSR (Hi-Z)**       | FidelityFX-SSSR (AMD) → Wicked Engine port (`ssr_raytraceCS.hlsl`)                                  |
| **TAA**              | Karis 2014 (*"High Quality Temporal Supersampling"*) + Salvi 2016 anti-flicker cross blur          |
| **XeGTAO**           | Intel XeGTAO (Filip Strugar 2016-2021) — `github.com/GameTechDev/XeGTAO` — verbatim port           |
| **CAS**              | AMD FidelityFX SDK 1.0, `ffx_cas.h` — LDS-tiled HDR-adapted port                                   |
| **Bloom**            | "Sledgehammer" 13-tap downsample + 3×3 tent upsample, Karis-average for first downsample (Jorge Jiménez / Call of Duty Advanced Warfare 2014) |
| **Atmosphere**       | Hillaire 2020 — *"A Scalable and Production Ready Sky and Atmosphere Rendering Technique"*           |
| **Volumetric Fog**   | Bart Wronski 2014 (*"Volumetric Fog: Unified Compute Shader Based Solution"*) — froxel grid + temporal reprojection |
| **Clustered Shading**| Olsson / Billeter / Assarsson 2012, Doom 2016 implementation                                       |
| **Outline**          | Inverted-hull (Genshin / NPR style) + screen-space Roberts edge on object-ID/normal/depth          |
| **Glass Shatter**    | Voronoi shard pre-cut + per-shard 2D rigid-body sim                                                |
| **Decals**           | Volume-projected (cube) clustered decals, similar to Doom 2016 / Wicked Engine decal cluster path  |
| **Beam (procedural tube)** | Parallel-transport frame, Rodrigues' rotation, per-segment hash noise wobble                 |
| **Tracer (cyl. billboard)** | `cross(beamAxis, toCamera)` cylindrical billboard with parallel-degenerate fallback         |
| **TAA Velocity Dilation**  | Drobot / Lottes nearest-depth dilation                                                       |
| **Reverse-Z Depth**  | Reed/Persson — recommended for D32_FLOAT depth precision                                            |
| **GPU Skinning**     | Standard linear-blend skinning + morph targets (compute)                                            |

### 6.3 Architecture Inspirations

- **Wicked Engine** (Turánszki, MIT) — RHI struct conventions, DDGI ray-bucket allocator, SSR port reference.
- **Granite / FrameGraph** (Themaister; Yuri O'Donnell GDC 2017 *"FrameGraph: Extensible Rendering Architecture in Frostbite"*) — render-graph design philosophy.
- **EnTT / flecs** — pool-per-component ECS data layout.
- **Unreal Engine** — post-process volume blending model, material schema concept.

---

## 7. Test Assets

Bundled under `asset/`:

- **Sponza** (Crytek / Frank Meinl / Intel) — classic GI / lighting reference scene.
- **Bistro** (Amazon Lumberyard) — DDGI verification scene.
- **soldier2** — skinned animation sample.
- **PMX/VMD test rig** — MMD pipeline verification.
- **IBL** — multiple HDRI cubemaps with pre-baked irradiance + radiance.

---

## 8. Build

```bat
:: Editor (with ImGui)
msbuild DX12Engine.sln /p:Configuration=Debug /p:Platform=x64

:: Game (no editor)
msbuild Game.vcxproj /p:Configuration=Release /p:Platform=x64

:: ShaderLab (shader-iteration tool)
msbuild ShaderLab.vcxproj /p:Configuration=Debug /p:Platform=x64
```

- **F11** at runtime toggles between the ImGui dockspace and a fullscreen viewport.
- Output log: `DX12Log.txt` in the working directory.

---

## 9. Status & Limitations

- Single-window, single-viewport.
- No automated tests.
- No DXR hardware-RT for primary visibility — DXR is used only by the DDGI inline-RayQuery trace.
- Hot-reload coverage today: 9 of ~30 passes are wired (GBuffer / Lighting / Shadow eager,
  Skybox / Transparent / Outline / SpotShadow / VolumetricFog / Picking lazy). Compute-only passes
  with hand-built PSOs need an explicit override to opt in.
- Animation: linear blend skinning only (no dual-quaternion / no compute spline path).
- Networking: not implemented.

---
