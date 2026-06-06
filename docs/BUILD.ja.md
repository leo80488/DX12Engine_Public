[English](BUILD.md) | **日本語**

# Build

> Visual Studio 2022、Windows 10 SDK (10.0.26100.0)、MSVC v143、C++20 でビルドを検証している。
> Windows + MSVC + x64 のみ対応。

## Prerequisites

| ツール                    | バージョン           | 備考                                                               |
| ------------------------- | -------------------- | ------------------------------------------------------------------ |
| **Visual Studio 2022**    | 17.x                 | 「C++ によるデスクトップ開発」+「C++ によるゲーム開発」            |
| **Windows 10 SDK**        | 10.0.26100.0 以降    | D3D12、DXGI、XAudio2 のヘッダーを提供                              |
| **CMake**                 | ≥ 3.21               | VS 2022 に同梱                                                     |

> パッケージマネージャは不要 —— サードパーティ依存はすべて `DX12/external/` 配下にプリビルド済みで同梱されている。

### Bundled dependencies

サードパーティ依存はすべて `DX12/external/` 配下にプリビルド済みで同梱されており、インストールは不要 —— パッケージ
マネージャ（vcpkg / Conan）も不要: **Assimp**、DirectXTex、Jolt、DXC、FreeType、Dear ImGui (+ ImGuizmo)、Lua 5.4、
sol2、nlohmann/json、**meshoptimizer**、**Recast/Detour**、**FFmpeg**（avcodec/avformat/avutil/swscale + バージョン
サフィックス付きランタイム DLL を `DX12/` に配置）、および DirectX-Headers。ほとんどは `#pragma comment(lib, ...)` 経由で
インラインリンクされ、CMake ビルドは単にそれらの `external/<lib>/lib` フォルダをリンカの検索パスに追加するだけである。
DXC、FFmpeg、Assimp のランタイム DLL はビルド時に各 `.exe` の隣にコピーされる。

## Configure & build

### Visual Studio 2022 (recommended)

```bat
cmake --preset vs-x64
cmake --build --preset vs-x64-release
```

Release の exe は `build/bin/Release/` に生成される。そこから `Editor.exe` を実行する。

### Ninja (faster incremental, command line)

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
```

### Manual configure (no preset)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Build options

| オプション                 | デフォルト | 説明                                                         |
| -------------------------- | ------- | ------------------------------------------------------------ |
| `ENGINE_BUILD_EDITOR`      | `ON`    | `Editor.exe`（ImGui エディタ）をビルドする。                  |
| `ENGINE_BUILD_GAME`        | `ON`    | `Game.exe`（ランタイムのみ、エディタなし）をビルドする。      |
| `ENGINE_BUILD_SHADERLAB`   | `ON`    | `ShaderLab.exe`（シェーダー反復用サンドボックス）をビルドする。|
| `ENGINE_PACKAGE_ASSETS`    | `ON`    | `asset/` を CPack の ZIP に同梱する（数 GB）。                |

設定行で例えば `-DENGINE_BUILD_GAME=OFF` のように指定して上書きする。

## Packaging

```bat
cmake --build build --config Release --target package
```

exe、ランタイム DLL（DXC、Assimp、FFmpeg）、シェーダー、スクリプト、および（任意で）同梱アセットを含む
`DX12Engine-<version>-win64.zip` を生成する。

## Runtime tips

- **F11** で ImGui のドックスペースとフルスクリーンビューポートを切り替える。
- 出力ログ: 作業ディレクトリ内の `DX12Log.txt`。
- 初回実行時に `pso_cache.bin` と `shaders/shader_cache_dxil/` が生成される。以降の起動はコールドキャッシュから開始する。

## Troubleshooting

- **起動時のメッシュシェーダー / DXR エラー** — D3D12 SM 6.6 + Mesh Shaders + DXR 1.1 をサポートする GPU + ドライバが必要。GPU ドライバを更新する。
- **ビューポートが黒い / シーンが表示されない** — `DX12/asset/` 配下のアセットが欠落している。作業ディレクトリが `DX12/` と一致していることを確認する（VS からの起動では CMake が設定する）。
- **`LNK1104: cannot open file '<lib>.lib'`** — 同梱の `external/<lib>/lib` フォルダがリンカの検索パスから欠落している。ビルドはほとんどのサードパーティライブラリを `#pragma comment(lib, ...)` 経由でリンクする。`DX12/external/`（Assimp、Recast/Detour、meshoptimizer、FFmpeg、DirectXTex、Jolt、FreeType、DXC）が無傷であることを確認する。
- **動画が黒く再生される / デコードエラー** — D3D12 のビデオデコードには、クリップのコーデック（H.264 / HEVC）をサポートする GPU + ドライバが必要。FFmpeg パスはソフトウェアデコードにフォールバックする。`avcodec-*.dll` / `avformat-*.dll` / `avutil-*.dll` / `swscale-*.dll`（+ `swresample-*.dll`）が exe の隣にあることを確認する。
