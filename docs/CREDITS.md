# Credits & References

## Acknowledgments

This project was developed with the assistance of [Claude Code](https://claude.com/claude-code), Anthropic's AI coding assistant.

## Libraries shipped under `DX12/external/`

| Library                            | Use                                                       | License (upstream) |
| ---------------------------------- | --------------------------------------------------------- | ------------------ |
| **DirectX-Headers**                | Up-to-date `d3d12.h` + Agility SDK headers                | MIT                |
| **DirectXTex**                     | Texture loading, BC compression, WIC save (PNG capture)   | MIT                |
| **DXC (dxcompiler.dll, dxil.dll)** | HLSL → DXIL compilation, reflection                       | LLVM (Apache 2.0)  |
| **Dear ImGui** (+ ImGuizmo)        | Editor UI, gizmo                                          | MIT                |
| **Jolt Physics**                   | Rigid-body physics                                        | MIT                |
| **Lua 5.4**                        | Scripting VM                                              | MIT                |
| **sol2**                           | C++ ↔ Lua binding                                         | MIT                |
| **FreeType**                       | Font rasterization for the UI                             | FTL / GPL          |
| **nlohmann/json**                  | Scene serialization, post-process volume blobs            | MIT                |
| **meshoptimizer**                  | Mesh simplification / optimization (collision-mesh baker) | MIT                |
| **Recast & Detour**                | Navmesh generation (Recast) + pathfinding (Detour)        | zlib               |
| **FFmpeg** (avcodec/format/util/swscale) | Video demux + hardware (D3D12VA) decode + scaling   | LGPL-2.1+          |
| **XAudio2 / X3DAudio**             | Audio mixer + 3D spatialisation                           | Windows SDK        |

## Libraries via vcpkg

| Library    | Use                                                     | License      |
| ---------- | ------------------------------------------------------- | ------------ |
| **Assimp** | Mesh / scene / skeleton / animation import (offline cook) | BSD-3-Clause |

> DXC binaries are copied from Windows SDK 10.0.26100.0.

## Algorithms / Techniques

| Subsystem                  | Reference                                                                                                                                    |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| **PBR / BRDF**             | Cook-Torrance 1982, Disney BRDF (Burley 2012), Epic UE4 PBR (Karis 2013), Schlick Fresnel approximation                                      |
| **CSM**                    | Practical-Split (NVIDIA), bounding-sphere stabilisation (Valient 2012), texel snapping, Halton jitter                                        |
| **DDGI**                   | RTXGI / NVIDIA *Dynamic Diffuse Global Illumination* (Majercik et al. 2019), L1 SH probes, Wicked Engine adaptive ray-bucket scheme          |
| **SSR (Hi-Z)**             | FidelityFX-SSSR (AMD) → Wicked Engine port (`ssr_raytraceCS.hlsl`)                                                                           |
| **TAA**                    | Karis 2014 (*"High Quality Temporal Supersampling"*) + Salvi 2016 anti-flicker cross blur                                                    |
| **XeGTAO**                 | Intel XeGTAO (Filip Strugar 2016-2021) — `github.com/GameTechDev/XeGTAO` — verbatim port                                                     |
| **CAS**                    | AMD FidelityFX SDK 1.0, `ffx_cas.h` — LDS-tiled HDR-adapted port                                                                             |
| **Bloom**                  | "Sledgehammer" 13-tap downsample + 3×3 tent upsample, Karis-average for first downsample (Jorge Jiménez / Call of Duty Advanced Warfare 2014) |
| **Atmosphere**             | Hillaire 2020 — *"A Scalable and Production Ready Sky and Atmosphere Rendering Technique"*                                                   |
| **Volumetric Fog**         | Bart Wronski 2014 (*"Volumetric Fog: Unified Compute Shader Based Solution"*) — froxel grid + temporal reprojection                          |
| **Clustered Shading**      | Olsson / Billeter / Assarsson 2012, Doom 2016 implementation                                                                                 |
| **Outline**                | Inverted-hull (Genshin / NPR style) + screen-space Roberts edge on object-ID/normal/depth                                                    |
| **Glass Shatter**          | Voronoi shard pre-cut + per-shard 2D rigid-body sim                                                                                          |
| **Decals**                 | Volume-projected (cube) clustered decals, similar to Doom 2016 / Wicked Engine decal cluster path                                            |
| **Beam (procedural tube)** | Parallel-transport frame, Rodrigues' rotation, per-segment hash noise wobble                                                                 |
| **Tracer (cyl. billboard)** | `cross(beamAxis, toCamera)` cylindrical billboard with parallel-degenerate fallback                                                         |
| **TAA Velocity Dilation**  | Drobot / Lottes nearest-depth dilation                                                                                                       |
| **Reverse-Z Depth**        | Reed/Persson — recommended for D32_FLOAT depth precision                                                                                     |
| **GPU Skinning**           | Standard linear-blend skinning + morph targets (compute)                                                                                     |
| **Navmesh / Pathfinding**  | Recast (voxelization → region → contour → polymesh) + Detour A* query (Mikko Mononen)                                                       |
| **Video Decode**           | FFmpeg demux + D3D12 Video (`ID3D12VideoDecoder`, D3D12VA hardware decode), `swscale` colour convert                                          |

## Architecture Inspirations

- **Wicked Engine** (Turánszki, MIT) — RHI struct conventions, DDGI ray-bucket allocator, SSR port reference.
- **Granite / FrameGraph** (Themaister; Yuri O'Donnell GDC 2017 *"FrameGraph: Extensible Rendering Architecture in Frostbite"*) — render-graph design philosophy.
- **EnTT / flecs** — pool-per-component ECS data layout.
- **Unreal Engine** — post-process volume blending model, material schema concept.

## Test Assets

Bundled under `DX12/asset/`:

- **Sponza** (Crytek / Frank Meinl / Intel) — classic GI / lighting reference scene.
- **Bistro** (Amazon Lumberyard) — DDGI verification scene.
- **soldier2** — skinned animation sample.
- **PMX/VMD test rig** — MMD pipeline verification.
- **IBL** — multiple HDRI cubemaps with pre-baked irradiance + radiance.
