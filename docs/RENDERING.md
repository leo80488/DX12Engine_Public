**English** | [日本語](RENDERING.ja.md)

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

- **Clustered Forward+ / Clustered Deferred** light culling — 16×9×24 (3456) froxel clusters, fixed
  per-cluster slice assignment (no atomic counter), up to 4096 lights in a triple-buffered 64-byte ring
  (`ClusterBuild.cs`, `ClusterCull.cs`). Reflection probes are culled in parallel (`ClusterCullProbes.cs`).
- Punctual lights: directional, point, spot. Spot lights opt into shadows on demand.
- **Cascaded Shadow Maps (CSM)** — 4 cascades (3 near + 1 ultra-far **terrain-only** cascade rendered via the
  mesh-shader terrain path) in a 4096²×4 D32_FLOAT array, reversed-Z, practical log+uniform split (λ=0.85,
  near…400, far cascade to 2000), tight bounding-sphere fit, **texel-snap-only** stabilization (sub-texel
  Halton jitter was removed — it read as TAA shimmer; softening is done by spatial PCF Bayer rotation).
- Per-material `ShadowCullMode` (Back / Front / None + alpha-test bucket) with per-group depth bias and
  prefix-summed `ExecuteIndirect` caster groups.
- **Spot shadow atlas** — 2048²×8-slice array for opt-in spot-light shadows; skipped entirely when no caster opts in.
- **Cook-Torrance microfacet BRDF** (GGX + Smith + Schlick) shared between opaque & transparent paths;
  deferred shading runs 3 stencil-gated PSO variants (PBR / NPR / Unlit).

## Global Illumination

- **DDGI (Dynamic Diffuse Global Illumination)** — DXR / inline `RayQuery` compute trace (no RTPSO/SBT).
  Per-probe **L1 SH irradiance** stored in a `StructuredBuffer<DDGIProbeSH>` (48 B) — the octahedral
  irradiance atlas and its border CS were removed; only a 16²-texel depth atlas (R16G16F) remains for
  Chebyshev visibility. Up to **4 slot-indexed volumes**, adaptive per-probe ray-count buckets driving an
  `ExecuteIndirect` trace, probe relocation + classification, multi-bounce (reads previous-frame SH/depth),
  per-frame van der Corput rotation (amplitude ∝ √(π/raysPerProbe)), emissive surface support via bindless
  texture sampling at the closest hit. `DDGISceneAS` builds the TLAS per frame over static + procedural
  geometry with budgeted BLAS builds (16/frame) + compaction. Tuned & verified on Sponza / Bistro.
- **Reflection Probes** — runtime-baked `TextureCubeArray` (up to 64 cubes, 128², 7 mips): 6 reverse-Z scene
  faces + skybox into a temp cube, then 42 GGX prefilter dispatches per probe; clustered probe selection in
  the lighting shader, `SceneBVH::QueryAABB`-driven capture culling.
- **Screen-Space Reflections (SSR)** — a dedicated subsystem (`Graphics/SSR/`, orchestrated by `SSRSubsystem`
  on its own command lists) ported from FidelityFX-SSSR / Wicked Engine: DepthHierarchy (Hi-Z min/max) →
  Karis scene-color pyramid → stochastic GGX trace → spatial BRDF resolve → dual-reprojection temporal →
  bilateral upsample → Fresnel×envBRDF composite. Runs at **full render resolution**; LightingPass dampens
  IBL by (1−ssrConf) with one-frame latency. Optional transient scratch-alias heap.
- **Sky Spherical Harmonics** — projects the dynamic sky atmosphere cube into 9-coefficient (L1) SH for IBL
  diffuse, re-run only when the sun moves / changes intensity.
- **Voxel scene** — 128³ R8_UINT Tier-2 triangle voxelization (`SceneVoxelize.cs`, `SceneVoxelClear.cs`)
  used as a volumetric-fog occlusion source; skipped when no consumer is active.

## Atmosphere & Sky

- Hillaire 2020 sky model: `AtmosphereTransmittance.cs`, `AtmosphereMultiScatter.cs`,
  `AtmosphereSkyView.cs`, `AerialPerspective.cs` (32³ 3D LUT), `SkyAtmosphere.cs`.
  Transmittance + MultiScatter LUTs bake once; per-frame work is dirty-checked against sun direction/intensity.
- Pre-baked stars (`StarsBake.cs`), analytic sun + moon disks, HDRI skybox visual override.
- IBL pipeline — irradiance (9-coeff sky SH) + radiance cube (temporal 1-face-per-frame GGX prefilter) +
  pre-integrated BRDF LUT (`SpecularPrefilter.cs`, `GenerateLUT.cs`).
- **Volumetric clouds** — once-baked 128³ noise volume, quarter-res depth-aware raymarch (`CloudRaymarch.cs`)
  with alpha-over HDR composite, driven by an ECS `CloudComponent` (altitude band, coverage, density, wind).

## Volumetrics

- **Froxel volumetric fog** — density → light-injection → scatter → temporal-reproject pipeline
  (`FroxelDensity.cs`, `FroxelLightInject.cs`, `FroxelScatter.cs`, `FroxelTemporal.cs`). Sun (CSM) +
  clustered point/spot + screen-space + voxel-occupancy shadowing, camera-teleport history reset.
- **Volumetric raymarch** half-res god-ray tier with its own temporal pass (`VolumetricRaymarch.cs`,
  `VolumetricRaymarchTemporal.cs`, `VolumetricRaymarchApply.ps`); composited additively, then the froxel
  transmittance composites over HDR (6 compute stages + 2 graphics apply passes total).

## Post-Process Stack

- **Auto Exposure** — log-luminance histogram, eye adaptation.
- **Bloom** — 13-tap "Sledgehammer" downsample with Karis-average, 3×3 tent upsample.
- **Lens Flare** — procedural directional-light flare composited pre-tonemap.
- **TAA** — Karis 2014 / Salvi 2016 hybrid: nearest-depth velocity dilation, 9-tap Catmull-Rom history,
  variance clipping with luma gamma (separate specular sigma), soft-edge disocclusion, anti-flicker cross
  blur, Halton jitter, outline-stencil-bit (0x80) history weakening; skinned meshes carry prev-pose velocity.
- **FXAA** — NVIDIA FXAA spatial pass, selectable independently or stacked after TAA (resolve priority
  FXAA > TAA > raw HDR).
- **XeGTAO** — verbatim port of Intel's XeGTAO main pass + 5-mip depth pre-filter; denoise runs **before**
  temporal accumulation (tightens the variance clip), with velocity + prev-linear-depth disocclusion rejection.
- **Tonemap** — final HDR → LDR with exposure & color-grading.
- **CAS** — AMD FidelityFX Contrast Adaptive Sharpening, LDS-tiled HDR-aware port.
- **Color Grading** parameter block.
- **Post-Process Volumes** — Unreal-style blendable volume system: Global / Box / Sphere SDF shapes with
  linear `blendDistance` falloff and per-stage `std::optional` overrides, priority-ordered weight-lerp
  blending (`VolumeSystem` slot registry + ECS `EntityVolumeSource` + `ParameterBlender`), plus a transient
  time-driven `ScriptedOverrideSystem` (damage-flash / flashbang fade curves). The staged effect chain
  (CAS → AutoExposure → Bloom → LensFlare → Tonemap) runs with zero-cost `IsEnabled` gates.

## Special Effects

- **Decal system** — clustered, screen-space-projected decals with material library + clustered cull.
- **Outline** — three-sub-pass system: inverted-hull silhouette + Object-ID + screen-space Roberts
  edge cross on normal/depth/ID.
- **Glass-shatter** — captures tonemap output, simulates shard physics, composites until duration elapses.
- **Particle system** — GPU emit/update compute pipeline, ring-buffer pool, indirect-draw render.
- **Trail system** — GPU-driven ribbon trails, control-point compute update.
- **Tracer system** — cylindrical-billboard "thin laser" tracers, GPU pool (4096 slots).
- **Beam system** — CS-generated procedural-tube heavy beams (`BeamTubeGen.cs`), parallel-transport frame,
  Rodrigues' rotation, optional perpendicular wobble; rendered through the **standard mesh path** via shared
  PVF vertex buffers + bindless mesh descriptors (no dedicated render pass). Inner-core + outer-glow shaders.
- **Afterimage / ghost** — snapshots skinned-mesh vertices into a bindless pool (`AfterimageCopy.cs`) and
  renders them as additive Fresnel-rim ghosts (`Afterimage_Ghost.ps`) with lifetime fade.
- **Skinned animation** — GPU skinning compute (`Skin.cs`), pose ring buffer, morph targets, CCD IK
  (incl. ground-aware foot IK), socket attachments, follow-entity / follow-socket components.
- **Video composite** — hardware-decoded NV12 → RGB (BT.709): fullscreen `VideoPass` and depth-tested
  world-space `VideoQuadPass` (see [Engine Systems](ENGINE.md#video)).
- **Chain physics** — CPU Verlet + PBD spring-bone simulation for hair / skirt / jiggle bones (no GPU cost).

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

The SSR sub-passes live under `DX12/include/Graphics/SSR/`; the rest under `DX12/include/RenderGraph/RenderPass/`.

| Category       | Passes                                                                                                |
| -------------- | ----------------------------------------------------------------------------------------------------- |
| **Geometry**   | GBuffer, Terrain, Skybox, Transparent, Picking, DebugWire                                             |
| **Shadows**    | Shadow (4-cascade CSM), SpotShadow                                                                    |
| **Lighting**   | Lighting, Decal, SkyIBL                                                                               |
| **GI**         | DDGI, DDGIProbeDebug, ReflectionProbeCapture, SceneVoxel                                              |
| **SSR**        | SSRTrace, SSRResolve, SSRTemporal, SSRUpsample, SSRComposite, SSRDepthHierarchy, SceneColorPyramid    |
| **Volumetric** | VolumetricFog (froxel + half-res raymarch, 6 compute + 2 apply), Cloud                                |
| **Culling**    | Culling, Cluster, HiZ                                                                                 |
| **Skinning**   | Skinning                                                                                              |
| **VFX**        | Particles, Trails, Tracers (sim+render), BeamSim, AfterimageCapture, GlassShatter                     |
| **Outline**    | Outline (3 sub-passes)                                                                                |
| **Post**       | TAA, FXAA, AutoExposure, Bloom, LensFlare, XeGTAO, CAS, ToneMap, ColorGrading                         |
| **Video**      | Video (screen-space), VideoQuad (world-space)                                                         |
| **UI**         | UI (screen), WorldUIBillboard                                                                         |
