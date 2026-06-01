# Build

> Built against Visual Studio 2022, Windows 10 SDK (10.0.26100.0), MSVC v143, C++20.
> Windows + MSVC + x64 only.

## Prerequisites

| Tool                      | Version              | Notes                                                              |
| ------------------------- | -------------------- | ------------------------------------------------------------------ |
| **Visual Studio 2022**    | 17.x                 | "Desktop development with C++" + "Game development with C++"       |
| **Windows 10 SDK**        | 10.0.26100.0 or newer | Provides D3D12, DXGI, XAudio2 headers                              |
| **CMake**                 | ≥ 3.21               | Bundled with VS 2022                                               |
| **vcpkg**                 | manifest mode        | For Assimp (see `vcpkg.json`). Set `VCPKG_ROOT` env var             |

### Bundled dependencies

Everything else is vendored prebuilt under `DX12/external/` and needs no install: DirectXTex, Jolt, DXC,
FreeType, Dear ImGui (+ ImGuizmo), Lua 5.4, sol2, nlohmann/json, **meshoptimizer**, **Recast/Detour**,
**FFmpeg** (avcodec/avformat/avutil/swscale + version-suffixed runtime DLLs in `DX12/`), and the
DirectX-Headers. Most are linked inline via `#pragma comment(lib, ...)`; the CMake build just puts their
`external/<lib>/lib` folders on the linker search path. DXC + FFmpeg runtime DLLs are copied next to each
`.exe` at build time.

## One-time setup

```bat
:: Clone vcpkg somewhere (or reuse an existing checkout)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

:: Tell CMake where it lives
setx VCPKG_ROOT C:\vcpkg
```

## Configure & build

### Visual Studio 2022 (recommended)

```bat
cmake --preset vs-x64
cmake --build --preset vs-x64-release
```

The Release exes land in `build/bin/Release/`. Run `Editor.exe` from there.

### Ninja (faster incremental, command line)

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
```

### Manual configure (no preset)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Build options

| Option                     | Default | Description                                                  |
| -------------------------- | ------- | ------------------------------------------------------------ |
| `ENGINE_BUILD_EDITOR`      | `ON`    | Build `Editor.exe` (ImGui editor).                           |
| `ENGINE_BUILD_GAME`        | `ON`    | Build `Game.exe` (runtime only, no editor).                  |
| `ENGINE_BUILD_SHADERLAB`   | `ON`    | Build `ShaderLab.exe` (shader-iteration sandbox).            |
| `ENGINE_PACKAGE_ASSETS`    | `ON`    | Bundle `asset/` into the CPack ZIP (multi-GB).                |

Override with e.g. `-DENGINE_BUILD_GAME=OFF` on the configure line.

## Packaging

```bat
cmake --build build --config Release --target package
```

Produces `DX12Engine-<version>-win64.zip` containing the exes, runtime DLLs (DXC, Assimp, FFmpeg),
shaders, scripts, and (optionally) bundled assets.

## Runtime tips

- **F11** toggles between the ImGui dockspace and a fullscreen viewport.
- Output log: `DX12Log.txt` in the working directory.
- First run will populate `pso_cache.bin` and `shaders/shader_cache_dxil/`; subsequent launches start cold-cached.

## Troubleshooting

- **`Could not find package 'assimp'`** — `VCPKG_ROOT` is unset, or the toolchain file wasn't passed. Re-run with the preset.
- **Mesh-shader / DXR errors at startup** — needs a GPU + driver supporting D3D12 SM 6.6 + Mesh Shaders + DXR 1.1. Update GPU drivers.
- **Black viewport / no scene** — assets missing under `DX12/asset/`. Confirm the working directory matches `DX12/` (CMake sets it for VS launches).
- **`LNK1104: cannot open file '<lib>.lib'`** — a vendored `external/<lib>/lib` folder is missing from the linker search path. The build links most third-party libs via `#pragma comment(lib, ...)`; confirm `DX12/external/` is intact (Recast/Detour, meshoptimizer, FFmpeg, DirectXTex, Jolt, FreeType, DXC).
- **Video plays black / decode errors** — D3D12 video decode needs a GPU + driver supporting the clip's codec (H.264 / HEVC). The FFmpeg path falls back to software decode; ensure `avcodec-*.dll` / `avformat-*.dll` / `avutil-*.dll` / `swscale-*.dll` (+ `swresample-*.dll`) sit next to the exe.
