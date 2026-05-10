# Rendering Pipeline

## Geometry / Visibility

- **Deferred G-Buffer** (Albedo / Normal / Surface / Emissive / Depth / Velocity).
- **PVF (Per-Vertex Format) bindless geometry**: per-attribute `ByteAddressBuffer` SRVs through a
  bindless mesh descriptor heap (no input layouts, no per-mesh root sig changes).
- **GPU frustum & cluster culling** (`InstanceCull.cs`, `ClusterCull.cs`, `ClusterCullProbes.cs`).
- **Hi-Z depth pyramid** (reverse-Z, 2-channel min/max) — `HiZGenerate.cs`, `HiZReduce.cs`.
- **ExecuteIndirect** indirect-draw path with GPU-built draw count.
- Scene **BVH** rebuilt per frame for CPU-side queries (frustum tests, picking).
- Mesh-shader **terrain pipeline** (`Terrain.as / .ms / .ps`) with quadtree LOD and amplification-shader-driven CSM caster path.

## Lighting

- **Clustered Forward+ / Clustered Deferred** light culling (`ClusterBuild.cs`, `ClusterCull.cs`).
- Punctual lights: directional, point, spot. Spot lights opt into shadows on demand.
- **Cascaded Shadow Maps (CSM)** — 3 cascades, 2048², practical-split (λ=0.85), tight bounding spheres,
  texel snapping, Halton(2,3) sub-texel jitter, comparison sampler.
- **Spot shadow atlas** for opt-in spot-light shadows.
- **Cook-Torrance microfacet BRDF** (GGX + Smith + Schlick) shared between opaque & transparent paths.

## Global Illumination

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

## Atmosphere & Sky

- Hillaire 2020 sky model: `AtmosphereTransmittance.cs`, `AtmosphereMultiScatter.cs`,
  `AtmosphereSkyView.cs`, `AerialPerspective.cs`, `SkyAtmosphere.cs`.
- Pre-baked stars (`StarsBake.cs`), HDRI skybox visual override.
- IBL pipeline — irradiance cube + radiance cube + pre-integrated BRDF LUT (`SpecularPrefilter.cs`, `GenerateLUT.cs`).

## Volumetrics

- **Froxel volumetric fog** — density / light-injection / temporal-reproject / scatter pipeline
  (`FroxelDensity.cs`, `FroxelLightInject.cs`, `FroxelTemporal.cs`, `FroxelScatter.cs`).
  Per-light volumetric contribution, TAA-aware history, voxel-occlusion gated.
- **Volumetric raymarch** for sun god-rays (`VolumetricRaymarch.cs`, `VolumetricRaymarchTemporal.cs`,
  `VolumetricRaymarchApply.ps`).

## Post-Process Stack

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

## Special Effects

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

## World-Space & Screen-Space UI

- Strictly separated subsystems sharing **no** components.
- **Screen-space**: `UISystem` widget tree (`UIRootComponent`) plus flat-ECS items
  (`UIScreenSpace/Image/Text/Bar`). Renders through `UIPass` with cmd-merging by texture/material/clip.
- **World-space**: `WorldSpaceUIComponent` + content components (`WorldUIBar`, `WorldUIText`,
  `WorldUIImage`, `DamageNumberComponent`). Billboarded via `WorldUIBillboardPass`. Entity-driven with
  `LocalTransform` + optional `FollowEntity`.

## Other

- **Picking** (`PickingPass`, `PickingID.vs/.ps`) — GPU readback → entity ID resolution.
- **Debug wireframe** — `DebugWirePass` for AABB / frustum / spline overlays.
- **DDGI probe debug visualization** — instanced spheres positioned at probe trace origins.

---

## Render-Pass Catalog

Located under `DX12/include/RenderGraph/RenderPass/`.

| Category       | Passes                                                                                                |
| -------------- | ----------------------------------------------------------------------------------------------------- |
| **Geometry**   | GBuffer, Terrain, Skybox, Transparent, Picking, DebugWire                                             |
| **Shadows**    | Shadow (CSM), SpotShadow                                                                              |
| **Lighting**   | Lighting, Decal, SkyIBL                                                                               |
| **GI**         | DDGI, DDGIProbeDebug, ReflectionProbeCapture, SceneVoxel                                              |
| **SSR**        | SSR (trace), SSRResolve, SSRTemporal, SSRUpsample, SSRComposite, SSRDepthHierarchy, SceneColorPyramid |
| **Volumetric** | VolumetricFog (4-pass froxel)                                                                         |
| **Culling**    | Culling, Cluster, HiZ                                                                                 |
| **Skinning**   | Skinning                                                                                              |
| **VFX**        | Particles, Trails, Tracers (sim+render), BeamSim, GlassShatter                                        |
| **Outline**    | Outline (3 sub-passes)                                                                                |
| **Post**       | TAA, AutoExposure, Bloom, LensFlare, XeGTAO, CAS, ToneMap, ColorGrading                               |
| **UI**         | UI (screen), WorldUIBillboard                                                                         |
