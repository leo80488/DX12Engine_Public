[English](CREDITS.md) | **日本語**

# クレジットと参考文献

## 謝辞

本プロジェクトは、Anthropic のAIコーディングアシスタントである [Claude Code](https://claude.com/claude-code) の支援を受けて開発された。

## `DX12/external/` 以下に同梱されているライブラリ

| ライブラリ                          | 用途                                                       | ライセンス（上流） |
| ---------------------------------- | --------------------------------------------------------- | ------------------ |
| **Assimp**                         | メッシュ／シーン／スケルトン／アニメーションのインポート（オフラインクック） | BSD-3-Clause       |
| **DirectX-Headers**                | 最新の `d3d12.h` + Agility SDK ヘッダー                    | MIT                |
| **DirectXTex**                     | テクスチャ読み込み、BC圧縮、WIC保存（PNGキャプチャ）       | MIT                |
| **DXC (dxcompiler.dll, dxil.dll)** | HLSL → DXIL コンパイル、リフレクション                     | LLVM (Apache 2.0)  |
| **Dear ImGui** (+ ImGuizmo)        | エディタUI、ギズモ                                         | MIT                |
| **Jolt Physics**                   | 剛体物理                                                   | MIT                |
| **Lua 5.4**                        | スクリプティングVM                                         | MIT                |
| **sol2**                           | C++ ↔ Lua バインディング                                  | MIT                |
| **FreeType**                       | UI用フォントラスタライズ                                   | FTL / GPL          |
| **nlohmann/json**                  | シーンシリアライズ、ポストプロセスボリュームのblob         | MIT                |
| **meshoptimizer**                  | メッシュ簡略化／最適化（コリジョンメッシュベイカー）       | MIT                |
| **Recast & Detour**                | ナビメッシュ生成（Recast）+ 経路探索（Detour）             | zlib               |
| **FFmpeg** (avcodec/format/util/swscale) | 動画のデマックス + ハードウェア（D3D12VA）デコード + スケーリング | LGPL-2.1+          |
| **XAudio2 / X3DAudio**             | オーディオミキサー + 3D空間化                              | Windows SDK        |

> DXC バイナリは Windows SDK 10.0.26100.0 からコピーされている。

## アルゴリズム／技術

| サブシステム               | 参考文献                                                                                                                                     |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| **PBR / BRDF**             | Cook-Torrance 1982、Disney BRDF (Burley 2012)、Epic UE4 PBR (Karis 2013)、Schlick フレネル近似                                               |
| **CSM**                    | Practical-Split (NVIDIA)、バウンディングスフィア安定化 (Valient 2012)、テクセルスナッピング、Halton ジッター                                  |
| **DDGI**                   | RTXGI / NVIDIA *Dynamic Diffuse Global Illumination* (Majercik et al. 2019)、L1 SH プローブ、Wicked Engine 適応的レイバケット方式            |
| **SSR (Hi-Z)**             | FidelityFX-SSSR (AMD) → Wicked Engine 移植 (`ssr_raytraceCS.hlsl`)                                                                           |
| **TAA**                    | Karis 2014 (*"High Quality Temporal Supersampling"*) + Salvi 2016 アンチフリッカークロスブラー                                               |
| **XeGTAO**                 | Intel XeGTAO (Filip Strugar 2016-2021) — `github.com/GameTechDev/XeGTAO` — 逐語的移植                                                         |
| **CAS**                    | AMD FidelityFX SDK 1.0、`ffx_cas.h` — LDSタイル化HDR適応移植                                                                                 |
| **Bloom**                  | 「Sledgehammer」13タップダウンサンプル + 3×3 テントアップサンプル、初回ダウンサンプルにKaris平均 (Jorge Jiménez / Call of Duty Advanced Warfare 2014) |
| **Atmosphere**             | Hillaire 2020 — *"A Scalable and Production Ready Sky and Atmosphere Rendering Technique"*                                                   |
| **Volumetric Fog**         | Bart Wronski 2014 (*"Volumetric Fog: Unified Compute Shader Based Solution"*) — froxel グリッド + テンポラルリプロジェクション               |
| **Volumetric Clouds**      | Schneider 2015 / 2017 (*"The Real-time Volumetric Cloudscapes of Horizon Zero Dawn"* / Nubis) — Perlin-Worley + 気象マップ；Hillaire/Frostbite エネルギー保存マルチスキャッター；Wrenninge マルチスキャッター近似 |
| **Height Fog**             | 解析的指数ハイトフォグ（Unreal スタイル）— 視線レイ光学的深度の閉形式積分 + Henyey-Greenstein サンインスキャッター                          |
| **Clustered Shading**      | Olsson / Billeter / Assarsson 2012、Doom 2016 実装                                                                                           |
| **Outline**                | 反転ハル (Genshin / NPR スタイル) + オブジェクトID／法線／深度に対するスクリーンスペース Roberts エッジ                                       |
| **Glass Shatter**          | Voronoi 破片プリカット + 破片ごとの2D剛体シミュレーション                                                                                    |
| **Procedural Grass**       | GPU メッシュシェーダーによる二次 Bézier リボンブレード、*Ghost of Tsushima* のクランプ／風モデル (Wohllaib, GDC 2021)                       |
| **Depth of Field**         | フォーカス距離の錯乱円、scatter-as-gather の 2 リングギャザー                                                                                |
| **NPR Stylize**            | Kuwahara フィルタ (Kuwahara 1976; 異方性 Kyprianidis et al. 2009)、4×4 Bayer 順序ディザ、輝度ハーフトーン、複数角度クロスハッチ              |
| **Decals**                 | ボリューム投影（キューブ）クラスタ化デカール、Doom 2016 / Wicked Engine のデカールクラスタパスと類似                                          |
| **Tracer (cyl. billboard)** | `cross(beamAxis, toCamera)` による円柱ビルボード、平行縮退フォールバック付き                                                                 |
| **TAA Velocity Dilation**  | Drobot / Lottes の最近傍深度ダイレーション                                                                                                   |
| **Reverse-Z Depth**        | Reed/Persson — D32_FLOAT 深度精度向けに推奨                                                                                                  |
| **GPU Skinning**           | 標準的なリニアブレンドスキニング + モーフターゲット（コンピュート）                                                                          |
| **Navmesh / Pathfinding**  | Recast（ボクセル化 → リージョン → 輪郭 → ポリメッシュ）+ Detour A* クエリ (Mikko Mononen)                                                    |
| **Video Decode**           | FFmpeg デマックス + D3D12 Video (`ID3D12VideoDecoder`、D3D12VA ハードウェアデコード)、`swscale` 色変換                                        |

## アーキテクチャの着想元

- **Wicked Engine** (Turánszki, MIT) — RHI 構造体の規約、DDGI レイバケットアロケータ、SSR 移植の参照。
- **Granite / FrameGraph** (Themaister; Yuri O'Donnell GDC 2017 *"FrameGraph: Extensible Rendering Architecture in Frostbite"*) — レンダーグラフの設計思想。
- **EnTT / flecs** — コンポーネントごとのプール方式の ECS データレイアウト。
- **Unreal Engine** — ポストプロセスボリュームのブレンディングモデル、マテリアルスキーマの概念。

## テストアセット

`DX12/asset/` 以下に同梱:

- **Sponza** (Crytek / Frank Meinl / Intel) — 古典的な GI／ライティングの参照シーン。
- **Bistro** (Amazon Lumberyard) — DDGI 検証シーン。
