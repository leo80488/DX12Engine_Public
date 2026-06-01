**English** | [日本語](README.ja.md)

# DX12Engine

<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-20-54" src="https://github.com/user-attachments/assets/c2865b65-ad47-487d-9a8b-43042034a2f0" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-31-25" src="https://github.com/user-attachments/assets/3b6ea120-9fa1-4b4b-a96d-f1d7775dcb7a" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-55-46" src="https://github.com/user-attachments/assets/9c14df46-96ef-4418-9329-eca21c4c4903" />

A real-time rendering engine and editor written in C++20 / Direct3D 12 (Shader Model 6.6, DXIL via DXC).

## Features

**Rendering**
- Deferred G-Buffer with bindless per-vertex-format (PVF) geometry and ExecuteIndirect GPU culling
- Clustered Forward+ / Deferred lighting (16×9×24 froxels), 4-cascade CSM (3 near + 1 ultra-far terrain) + spot shadow atlas
- DDGI (inline RayQuery, per-probe SH irradiance, up to 4 volumes), runtime-baked reflection probes, Hi-Z stochastic SSR subsystem
- Hillaire 2020 atmosphere, sky-SH IBL, IBL cube + BRDF LUT, volumetric clouds
- Froxel volumetric fog + volumetric raymarch god-rays
- TAA + FXAA, XeGTAO, Bloom (Sledgehammer), CAS, Auto-Exposure, Lens Flare, Tonemap, Color Grading
- Mesh-shader terrain pipeline, decals, outline (3-pass), glass-shatter, GPU particles / trails / tracers / beams / afterimages
- GPU skinning + morph targets + CCD IK (incl. ground-aware foot IK) + sockets + chain / spring-bone physics
- Hardware video decode (FFmpeg D3D12VA → NV12 YUV→RGB composite, screen- and world-space)

**Engine**
- Pool-per-component (sparse-set) ECS, 16-phase dependency-aware parallel scheduler, scene-graph hierarchy
- Post-process volume system, AnimNotify / Timeline runtime, time-of-day, GUID-stable entity references
- Resource cooker (Mesh / `.meshlib` / Material / Texture / Animation / Skeleton / PMX / VMD / VRM / Audio) + `.ipak` virtual filesystem
- Async loading, GPU BC compression, descriptor-heap allocators, PSO + DXIL shader-blob caches, hot-reload
- Jolt Physics + kinematic character controller, Recast/Detour navigation, behavior-tree AI (Intent / Tactical / LOD layers)
- Reverse-Z, bindless everything, material schema, reflection-driven inspector

**Tooling**
- ImGui-docking editor (Hierarchy / Viewport / Inspector / Asset Browser / Timeline + SSR / Font / Camera / Profiler debug windows)
- ShaderLab sub-second shader-iteration sandbox
- Lua 5.4 + sol2 scripting (gameplay, UI, AI behavior trees), GameModeStack scene flow
- Jolt Physics, XAudio2 + X3DAudio

## Build

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
cmake --preset vs-x64
cmake --build --preset vs-x64-release
```

Full instructions, options, and troubleshooting → [docs/BUILD.md](docs/BUILD.md).

## Documentation

| Document                                  | Contents                                          |
| ----------------------------------------- | ------------------------------------------------- |
| [Architecture](docs/ARCHITECTURE.md)      | Layering, frame management, shader pipeline       |
| [Rendering](docs/RENDERING.md)            | Rendering pipeline detail + render-pass catalog   |
| [Engine Systems](docs/ENGINE.md)          | ECS, resources, animation, physics, audio, AI, scripting, editor, ShaderLab |
| [Build](docs/BUILD.md)                    | CMake presets, options, packaging, troubleshooting |
| [Credits & References](docs/CREDITS.md)   | Third-party libraries, papers, inspirations       |
| [Status & Limitations](docs/STATUS.md)    | Known gaps                                        |
