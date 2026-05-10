# Engine Systems

## ECS

Minimal data-oriented ECS (`ECS::World`, pool-per-component, free-list entity IDs).
Components include scene-graph (`Parent`, `Children`, `LocalTransform`, `GlobalTransform`,
`Visibility`, `RenderLayer`, `WorldAabb`), animation, physics, AI, audio, UI, decal, terrain,
follow / socket, beam / particle / trail, reflection-probe / DDGI volume.

## Resource System (offline cook + runtime handle)

- **Importers** — Mesh / Material / Shader / Texture / Animation / Skeleton / Scene /
  PMX / VMD / VRM (MMD support) / Decal-Material / Audio.
- **Cooked formats** — `.imsh`, `.imat`, `.itex`, `.ishdr`, `.ianim`, `.iskel`, `.iscn`, `.iworld`.
- **Runtime systems** — `TextureSystem` (path → GPU texture refcount, BC compression),
  `MeshSystem` (DEFAULT-heap VB/IB, interleaved POS+NOR+UV 32B), `MaterialSystem` (CBV ring upload),
  `AnimationClipSystem`, `MeshLibrary`, `DecalMaterialLibrary`.
- **Async loading** — `ResourceManager` returns handle immediately; I/O on `TaskSystem` workers;
  GPU uploads deferred to main thread (`ProcessPendingGPUUploads`).

## Scene Graph

- `SceneLoader` (Assimp), `SceneImporter` (offline cook → `.iscn` + `.imsh` set), `SceneInstanceLoader` (runtime, no Assimp).
- `TransformSystem::Propagate` — BFS hierarchy update, propagates `Visibility::inherited_hidden`,
  recomputes `WorldAabb`.

## Animation

- Skeletal — Assimp / VMD / VRM clip import, blend trees, morph targets, IK, sockets, Follow-entity / Follow-socket.
- `SkinnedMeshSubsystem` + `SkinningPass` (compute), `PoseRingBuffer` for triple-buffered skinning.
- `BoneCloth` / `ChainPhysics` (spring bones for hair, skirts, chests).

## Physics

- **Jolt Physics** integration (60 Hz fixed step, accumulator inside `PhysicsSystem::Update`).
- `RigidBodyComponent` + `ColliderComponent` / `CapsuleColliderComponent`. Lazy Jolt body creation.
- Independent **chain-physics CS** for cosmetic bone simulation (no Jolt cost).

## Audio

- **XAudio2 + X3DAudio** (`AudioEngine`) with submix bus per `BusType` (Music / SFX / Voice / Ambient / UI).
- `AudioClipSystem` deduplicates clips by path; `Audio3DSystem` handles spatialisation.
- `AudioImporter` cooks to `.iaud`.

## AI

- **Behavior Tree** runtime (`BTAsset`, `BTNode`, `BTNodes`, `ActionRegistry`).
- BT trees authored in **Lua**, parsed into a C++ node tree, hot-reloadable.
- Shares the `ScriptSystem` Lua VM; AI ticks after scripts and before physics.
- **AI LOD** system (`AILODSystem`) for distance-based tick-rate reduction.

## Scripting

- **Lua 5.4** + **sol2** binding.
- Per-entity `ScriptComponent`, global scripts (`AddGlobalScript`).
- Hot-reload via `FileWatcher`.
- Time-scale support (`SetTimeScale`) for hit-stop / bullet-time without freezing real-time timers.
- `Engine.AfterDelay` callbacks tick on real (unscaled) dt.
- Lua bindings for math types, UI, BT actions, ECS commands.

## Reflection

- `Reflect::Descriptor<T>` + `REFLECT_BEGIN/END` macros — declarative struct-field metadata.
- Drives the editor's component inspector (auto-generated ImGui widgets, eliminates ~30 hand-written blocks).
- Field kinds: scalar / color3+4 / enum / bool / string / quaternion-as-Euler / direction-as-AzEl / array / vector / drag-drop string / collapsing header.
- `MaterialSchema` — same idea applied to material params; `MaterialSerializer` is schema-driven (one-line addition for a new editable field).
- Custom-shader inspector reads DXC shader reflection (`ShaderReflect::Reflection`), auto-builds widgets for cbuffer vars and texture slots.

## Editor (`Editor.exe`)

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

## ShaderLab (`ShaderLab.exe`)

- Reuses the entire `EngineCore.lib` + `EditorLayer`.
- Boots into a sphere + 3-point lights + IBL scene tuned for shader iteration.
- HDRI swap from `asset/IBL/<name>/`, mesh swap (Cube / Sphere / Cone), turntable orbit.
- Drag a `.ps.hlsl` onto the Shader Path field → `MaterialReflectionSync` auto-populates the inspector.
- Sub-second iteration loop verified end-to-end.

## Tools

- **Asset packer** (`tools/pack_assets.py`) — cook + bundle assets.
- **Game packager** (`tools/package_game.py`) — pack `Game.exe` + cooked assets for distribution.
