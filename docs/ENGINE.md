**English** | [日本語](ENGINE.ja.md)

# Engine Systems

## ECS

Data-oriented, **pool-per-component (sparse-set)** ECS (`ECS::World`). Each component type owns a pool with
`sparse[entity] → dense index` + parallel dense `owner`/`value` arrays, so `GetComponent` is two array loads
and `ForEach<T>` walks only entities that have `T`. Entity IDs recycle through a free list; create / destroy
are O(1) via swap-with-back.

- **`EntityHandle`** — generation-stamped reference (`IsHandleValid`) that defeats recycled-slot aliasing for
  long-lived references (follow targets, event payloads, attachments).
- **Entity-destroy listeners** fire *before* component erasure so caches (renderer textures, `GuidRegistry`)
  can finalize; listeners survive `World::Clear()`.
- Components include scene-graph (`Parent`, `Children`, `LocalTransform`, `GlobalTransform`, `Visibility`,
  `RenderLayer`, `WorldAabb`), animation / morph / socket / follow, physics + character-controller, AI / nav /
  intent, audio, video, UI, decal, terrain / grass / water, beam / particle / trail / billboard-FX,
  reflection-probe / DDGI / cloud / height-fog / TOD, post-process volume.

## System Scheduler

A 16-phase tick pipeline (`TickPhase`) drives every system; see
[ARCHITECTURE.md → System Scheduler](ARCHITECTURE.md#system-scheduler) for the phase list and the
dependency-aware parallel batching model. Systems are registered once in `App::RegisterTickSystems`
(`SystemRegistry`, partitioned per phase) and wrapped as `ISystem` adapters in `EngineSystems.cpp`.

- **`CommandBuffer`** — defers `AddComponent` / `RemoveComponent` / `DestroyEntity` / `Defer(lambda)`,
  flushed at the phase boundary.
- **`FrameContext`** — unscaled `deltaTime` vs `scaledDeltaTime` (hit-stop / bullet-time), `fixedDeltaTime`,
  `physicsAlpha`, frame indices, and the editor play-state gate (`runUpdate`).
- **`Command` API** — high-level gameplay edits (`EquipToSocket` / `UnequipItem` / `AttachToEntity` /
  `DetachFromEntity`) that publish equip / unequip events on the `EventBus`.
- **`LifetimeSystem`** — counts down `LifetimeComponent` on *unscaled* dt and auto-destroys (timed VFX cleanup).
- **GUID system** — RFC-4122 v4 `Guid` + a process-global `GuidRegistry` Guid↔Entity bimap (auto-unregister
  on destroy, rebuild after scene load); `AttachmentRef` resolves a stamped `GuidComponent` with an O(1)
  cached fast path — the backbone for save-stable cross-entity references.

## Resource System (offline cook + runtime handle)

- **Importers** — Mesh (OBJ) / Scene (Assimp FBX·glTF·OBJ·DAE) / PMX / VMD / VRM (MMD) / Skeleton /
  Animation / Texture / Shader / Material / Decal-Material / Audio.
- **Cooked formats** — `.meshlib`, `.imsh`, `.imorph`, `.imat`, `.itex`, `.ishdr`, `.ianim`, `.iskel`,
  `.iscn`, `.iworld`, `.aclip`, `.inav`. Every blob is `[24-byte AssetHeader][typed Metadata][payload]`
  (four-CC magic + version), accessed copy-free via `GetMetadata` / `GetPayload`.
- **`.meshlib`** (canonical mesh container) — one shared vertex pool + one shared `uint32` index pool + a
  `MeshLibraryEntry` table indexed by stable `meshId` (per-mesh range, AABB, default material). Two vertex
  layouts: 32-byte legacy (pos+normal+uv) and 48-byte with a `float4` tangent (`MESHLIB_FLAG_HAS_TANGENT`).
  The legacy merged-`.imsh` pool and `.imshpack` archive were retired during the P1–P6 rewrite; `.imsh` now
  only exists as the OBJ-importer output and a legacy decode path.
- **`AssetFS` / `.ipak`** — read-only virtual filesystem: a single `.ipak` archive (built by
  `tools/pack_assets.py`) is slurped into RAM with a normalized-path index and served by lock-free `memcpy`,
  with a transparent loose-file `std::ifstream` fallback so the editor runs without a pak.
- **Runtime systems** — `TextureSystem` (path→GPU texture refcount, DXGI→RHI format table, batched deferred
  upload), `MeshLibrary` / `MeshSystem` (DEFAULT-heap RAW VB/IB), `MaterialSystem` (CBV ring upload),
  `AnimationClipSystem`, `DecalMaterialLibrary`.
- **Texture pipeline** — `BCCompressor` runs GPU BC6H/BC7/BC4/BC5 via a dedicated D3D11 device (WARP / CPU
  fallback); `TextureImporter` classifies a `TexRole` by filename keyword (Color→BC7 sRGB, Normal→BC5,
  SingleChannel→BC4, HDR→BC6H, plus uncompressed RawHeight / RawLUT2 / RawColor roles) and generates mips.
- **Async loading** — `ResourceManager` returns a generational `Handle` immediately, runs I/O + import +
  cooked-cache write on `TaskSystem` workers, and pumps GPU uploads on the main thread within a time budget.
- **Collision baking** — `Tools/CollisionMeshBaker` uses **meshoptimizer** (weld / simplify / vertex-fetch)
  to bake merged collision `.meshlib` blobs for Jolt mesh shapes (+ a debug wireframe overlay).

## Scene Graph & Game Flow

- **`GameModeStack`** (`IGameMode`) is the active scene-flow model, now built on a single **data-driven `DataScene`**
  mode that replaces the old hardcoded Title / Game / End classes. Scenes are declared in a `game.json` registry
  (`name → .iscn` path + optional Lua scene script; `.iscn` may also carry its own `sceneScript`); the `startup_scene`
  boots a `DataScene` wrapping it, and transitions go through `requestReplaceMode`. Editor / ShaderLab keep their
  dedicated `TestScene` / `ShaderLabScene` tool modes.
- **`SceneManager`** coordinates the blocking world-reload pipeline (used by `DataScene::Init` and the Lua `Scene.Load`
  path): `WaitIdle` + GPU deferred release → `Renderer::OnWorldClear` (stash caches) → `PhysicsSystem::OnWorldClear`
  (destroy Jolt bodies before `World::Clear` to avoid recycled-entity-ID orphans) → `LoadScene` (`.iscn` + companion
  navmesh) → queue the scene-script swap. Unresolved names/paths fall back to `SpawnDefaultWorld`
  (camera + directional light + IBL skybox).
- **Scene scripts** — each scene may name a Lua script with deferred `OnSceneEnter` / `OnSceneUpdate` / `OnSceneExit`
  callbacks, serviced by `ScriptSystem` on the next tick while the world is valid (so they respect editor play/pause
  gating). Driven from Lua via `Scene.Load / LoadInstant / Reload / Current / List`.
- **`SceneInstanceLoader`** — runtime, Assimp-free `.iscn` instancer (`SceneLoader` runtime path is retired).
- **`TransformSystem::Propagate`** — BFS hierarchy update, propagates `Visibility::inherited_hidden`,
  recomputes `WorldAabb`; allocation-free in steady state.

## Input

Centralized polled input (`Input` singleton, `Input/InputSystem`): once per frame it snapshots
`GetAsyncKeyState` into a 256-byte curr/prev table and exposes `IsKeyDown`, `WasKeyPressed/Released` edge
queries, and `Axis(pos, neg)`; resets on focus loss. (Mouse remains in `System/Mouse`.)

## Player & Character Control

- **`PlayerControllerSystem`** — camera-relative WASD on an XZ basis, normalized diagonals, air-control
  scaling, walk / run, turn-to-face-heading, and a coyote-time + jump-buffer jump pipeline that drives
  `CharacterControllerComponent`.
- **`CharacterControllerComponent`** — a `JPH::CharacterVirtual` KCC motor (collide-and-slide, step-up,
  ground snap, slope limit) with gameplay gravity separate from Jolt's world gravity and a `MovementMode`
  (Walking / Falling / Swimming / Climbing / LaunchedTraversal). Mutually exclusive with `RigidBody`.
- **`CharacterStateSystem`** — a Lua-authored per-state **cross-fade state machine** (lazy clip acquire/bind,
  `pendingIdx → currentIdx` blend); exposed as `Character.AddState/SetState/...`.

## Cameras

- **`CameraSystem`** — FPS / free-fly camera plus first/third-person follow with a spring-arm collision
  sphere-sweep against `PhysicsSystem`.
- **`CameraStackSystem`** — a priority/weight **virtual-camera stack** (Cinemachine-style): per-channel
  hashed IDs, a per-VCam blend state machine, `tan(fov/2)` FOV blend, shortest-path nlerp rotation, weighted
  multi-contributor resolve into `LiveCameraComponent`, hard-cut signalling for temporal passes, and
  trauma²-driven shake. Exposed as `Camera.PushVCam/PopVCam/HardCutTo/AddShake/...`.

## Animation

- Skeletal — Assimp / VMD / VRM clip import, primary↔secondary **cross-fade** blending, morph targets,
  CCD IK, sockets, Follow-entity / Follow-socket. (Note: cross-fade FSM, not generic blend trees.)
- `SkinnedMeshSubsystem` + `SkinningPass` (compute `Skin.cs`), `PoseRingBuffer` for triple-buffered skinning;
  previous-frame skinned positions feed TAA velocity.
- **MMD support** — rotation-grant (付与 / D-bone) bones split pre-IK / post-IK; CCD-IK solver ported from
  *saba* (plane mode, angle limits, Euler decomposition).
- **Foot IK** — `FootIKTargetSystem` auto-detects left/right foot IK chains by bone name, raycasts the
  ground via physics, and applies an additive terrain-Y delta before the IK solve.
- `.ianim` carries SOA bone clips + morph clips + anim events + an optional tail AnimNotify JSON section.

## Physics

- **Jolt Physics** (60 Hz fixed step) integrated through `PhysicsSystem` with split scheduler entry points
  (`PreAllSteps` / `StepOnce` / `PostAllSteps`) so fixed-phase work runs between substeps.
- 8-category object-layer matrix (StaticEnv / DynamicProp / Character / Ragdoll / HurtBox / AttackBox /
  Projectile / Sensing) with custom broad-phase + layer-pair filters and a runtime `LayerMask` for queries.
- `RigidBodyComponent` + `ColliderComponent` / `CapsuleColliderComponent`; lazy body creation; shape-cast +
  ray queries (`CastRay/Capsule/SphereClosest`); buffered contacts drained as `ContactBeganEvent` to the
  `EventBus`; render-pose interpolation by `physicsAlpha`; parallel mesh-shape prewarm + blob cache.
- **Chain physics** — `ChainPhysicsSystem` is a **CPU** secondary-bone solver (Verlet + PBD distance
  constraints in 6 color groups for hair / skirt chains, spring-damper ODE for Chest/Butt jiggle bones,
  capsule-collider collisions) running in the `SecondaryPhysics` phase. *Not* a compute shader.

## Navigation

- **Recast / Detour** wrapper (`Nav::NavMeshSystem`): single-tile and tiled navmesh bakes from a triangle
  soup gathered from world mesh colliders (editor "Build NavMesh"), `.inav` serialization, `FindPath`
  (straight-path), raycast, and point projection; debug lines via `DebugWirePass`.
- **`NavAgentSystem`** — pathfinding + steering: waypoint follow with arrive / slowdown radii, repath gating,
  facing modes (FaceMovement / FaceTarget / Manual) with nlerp slerp, CCC-blocked stuck detection + replan,
  and an off-mesh-link traversal handshake. Writes the KCC's `desiredHorizontalVelocity`.

## AI

- **Behavior Tree** runtime (`BTAsset` shared tree + per-entity `BTInstance`): composites
  (Sequence / Selector / Parallel), decorators (Inverter / Repeater / Cooldown / BlackboardCondition),
  leaves (Action / Condition), plus `UtilitySelector` and `SubTree`. Trees authored in **Lua**, parsed to a
  C++ node tree, hot-reloaded via mtime polling; every tick records a trace for the editor BT visualiser.
- **`ActionRegistry`** — native-C++-then-Lua (`Actions[]` / `Conditions[]`) dispatch (today all-Lua).
- **Layered movement** — `AIIntentComponent` (strategic goal) → `AITacticalSystem` (Idle / Patrol /
  Investigate / Attack / Follow / Flee → nav fields) → `NavAgentComponent` → `CharacterControllerComponent`.
- **`AILODSystem`** — distance-tier tick-rate throttling (boss / combat / distant / offscreen).
- A string-keyed `BlackboardComponent` (8-type variant) is round-tripped to/from Lua.

## Scripting

- **Lua 5.4** + **sol2**, one shared VM. Script categories: Logic (per-entity `OnSpawn/OnUpdate/OnDestroy`),
  System (global `OnInit/OnUpdate/OnShutdown`), Service (stateless), UI, and Lua-as-data Config.
- `LuaBus` event queue (`Engine.Subscribe/Publish`) bridged to the C++ `EventBus` (incl. `ContactBeganEvent`).
- Hot-reload via `FileWatcher`; synchronous `OnDestroy` via the World entity-destroy listener.
- Time-scale (`SetTimeScale`) for hit-stop / bullet-time; `Engine.AfterDelay` timers tick on real (unscaled) dt.
- **Editor-exposed script variables** (schema + per-entity overrides injected before `OnSpawn`).
- Binding modules: math types, `Input`, `Time`, `Camera`, `Animation`, `Physics.Raycast`, `Nav.*`,
  `Character.*` / `Player.*`, `Intent.*` / `AI.*`, BT actions, `ui` widgets, ECS commands,
  `Scene.*` (data-driven scene flow), `PostProcess.*` (volume spawn / per-property override / transient envelopes).

## Audio

- **XAudio2 + X3DAudio** (`AudioEngine`) with a mastering voice + 5 submix buses (Music / SFX / Voice /
  Ambient / UI); generation-stamped voice handles over a fixed slot table.
- `AudioSystem` is event-driven (`PlaySound` / `StopSound` / `SetAudioParam` / `BusVolumeChanged`);
  `AudioClipSystem` path-deduplicates clips; `Audio3DSystem` derives Doppler from finite-difference velocity.
- `AudioImporter` cooks RIFF/WAVE (PCM-only v1, cue/label table) to **`.aclip`** (`AudioClipLoader` reads it).

## Video

Hardware video playback driven by an ECS `VideoComponent` and `VideoSystem`:

- **Decode** — an FFmpeg-backed `Mp4FrameSource` (built under `WITH_FFMPEG`) hands FFmpeg the engine's own
  `ID3D12Device` for **D3D12VA** hardware decode, with transparent software (`sws_scale`) fallback; or a
  hand-rolled `VideoDecoderDX12` (`ID3D12VideoDecoder`, DXVA H.264 / HEVC Main / Main10) on a dedicated
  VIDEO_DECODE queue with a cross-queue graphics-Wait fence. Both produce NV12 textures.
- **Composite** — `VideoPass` (fullscreen) and `VideoQuadPass` (depth-tested world-space quads) run the
  NV12 → RGB (BT.709) conversion; a built-in SMPTE colour-bar test pattern exists for author-driven playback.

## AnimNotify / Timeline

- **`TimelineSystem`** (Animation phase) dispatches `AnimNotify` / `NotifyState` events from two additive
  sources: clip-authored `notifyTracks` and per-entity `TimelineComponent` override tracks, using a
  loop-wrapping `(prev, curr]` cross-time window with Begin/Tick/End edges.
- Events route through six `Pending*` **mailbox components** (Hitbox / VFX / Camera / Audio / StateToggle /
  Generic) written in the Animation phase and drained the same frame in the BoneAttachment phase by their
  consumer systems — most built-out is `VFXSpawnSystem` (pure-ECS Particle/Trail/Beam/Mesh emitters + Renderer
  mailbox requests for Tracer/Decal/Afterimage, with attach modes + a `VFXPrefab` cache).
- `NotifyIO` round-trips notify tracks to/from JSON (used by the Timeline editor).

## Time of Day

`TODSystems` (5 single-concern systems driven from `BuildRenderScene`): advance/wrap `timeOfDay`, evaluate
sun direction from hour-angle + latitude, blend sun/moon colour with a day/night smoothstep, and sync the
result into the `SunLightTag` / `MoonLightTag` light entities.

## Reflection

- `Reflect::Descriptor<T>` + `REFLECT_BEGIN/END` macros — declarative, header-only struct-field metadata
  (~80 component descriptors live in `ComponentReflection.h`).
- Drives the editor's component inspector (auto-generated ImGui widgets, eliminating hand-written blocks).
- Field kinds: scalar / color3+4 / enum / bool / string / quaternion-as-Euler / direction-as-AzEl / angle /
  slider / `std::vector` + fixed-array (add/remove) / drag-drop string / conditional (`REFLECT_IF`) +
  collapsing-header groups / info-text / custom-draw hooks.
- `MaterialSchema` applies the same idea to material params (schema-driven `.imat`); `MaterialReflectionSync`
  auto-populates custom-shader params / textures from DXC shader reflection, filtering engine-reserved bindings.

## Editor (`Editor.exe`)

- **ImGui docking** layout: Hierarchy / Viewport / Inspector / Resource / Log; **F11** fullscreen viewport.
- **Asset Browser** — filesystem scan, live BC texture thumbnails (per-frame load budget), FA icons,
  drag-drop into scene / inspector.
- **Inspectors** — schema-driven material (PBR / NPR / Unlit + custom-shader) and reflection-driven component
  inspectors with hybrid `postDraw` hooks; Add-Component menu by category.
- **Viewport** — GPU picking, drag-to-move on a camera-facing plane, ImGuizmo translate / rotate / scale,
  Play / Pause / Step-Frame controls.
- **Floating windows** — Timeline (AnimNotify authoring with undo/redo), SSR Debug (per-stage preview tiles),
  UI Font Editor (live FreeType re-bake with CJK ranges), Camera Switcher (virtual-camera stack), Animation
  Debug (bone-matrix NaN/Inf diagnostics), Profiler, Phase Debug, Post-Process, Decal Materials.
- **Tools / Build** — Bake Collision Meshes, Bake NavMesh, Bake Reflection Probes, frame capture to PNG,
  Glass-Shatter trigger, per-pass debug toggles, Set Startup Scene (writes `game.json`), Package Game.
- **`EntityRefPicker`** — drag-drop `AttachmentRef` binding with automatic `GuidComponent` stamping.

## ShaderLab (`ShaderLab.exe`)

- Reuses the entire `EngineCore.lib` + `EditorLayer`; boots into a mesh + light-preset + IBL scene tuned for
  shader iteration. Mesh swap (Cube / Sphere / Cone / Plane / Torus), light presets (Three-Point / Key-Only /
  Pure-Black), HDRI swap from `asset/IBL/<name>/`, turntable orbit, frame capture.
- Drag a `.ps.hlsl` onto the Shader Path field → `MaterialReflectionSync` auto-populates the inspector.
- **NPR Ramp Editor** — 512×25 RGBA atlas with per-row two-stop gradients, live-bound into a material's
  RAMPMAP slot via bindless index.

## Tools

- **Asset packer** (`tools/pack_assets.py`) — cook + bundle assets into a `.ipak`.
- **Game packager** (`tools/package_game.py`) — pack `Game.exe` + cooked assets for distribution.
