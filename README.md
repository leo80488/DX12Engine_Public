# DX12Engine

<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-20-54" src="https://github.com/user-attachments/assets/c2865b65-ad47-487d-9a8b-43042034a2f0" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-31-25" src="https://github.com/user-attachments/assets/3b6ea120-9fa1-4b4b-a96d-f1d7775dcb7a" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-55-46" src="https://github.com/user-attachments/assets/9c14df46-96ef-4418-9329-eca21c4c4903" />

A real-time rendering engine and editor written in C++20 / Direct3D 12 (Shader Model 6.6, DXIL via DXC).

## Features

**Rendering**
- Deferred G-Buffer with bindless per-vertex-format geometry and ExecuteIndirect culling
- Clustered Forward+ / Deferred lighting, CSM (3 cascades, Halton-jittered), spot shadow atlas
- DDGI (inline RayQuery), runtime-baked reflection probes, Hi-Z stochastic SSR
- Hillaire 2020 atmosphere, sky-SH IBL, IBL cube + BRDF LUT
- Froxel volumetric fog + volumetric raymarch god-rays
- TAA, XeGTAO, Bloom (Sledgehammer), CAS, Auto-Exposure, Lens Flare, Tonemap, Color Grading
- Mesh-shader terrain pipeline, decals, outline (3-pass), glass-shatter, GPU particles / trails / tracers / beams
- GPU skinning + morph targets + IK + sockets + chain physics

**Engine**
- Pool-per-component ECS, scene-graph hierarchy, post-process volume system
- Resource cooker (Mesh / Material / Texture / Animation / Skeleton / PMX / VMD / VRM / Audio)
- Async loading, descriptor-heap allocators, PSO + DXIL shader-blob caches, hot-reload
- Reverse-Z, bindless everything, material schema, reflection-driven inspector

**Tooling**
- ImGui-docking editor (Hierarchy / Viewport / Inspector / Asset Browser / Timeline)
- ShaderLab sub-second shader-iteration sandbox
- Lua 5.4 + sol2 scripting, Behavior Tree AI authored in Lua
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
