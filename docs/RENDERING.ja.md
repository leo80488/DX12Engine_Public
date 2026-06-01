[English](RENDERING.md) | **日本語**

# レンダリングパイプライン

## ジオメトリ / 可視性

- **ディファード G-Buffer**（Albedo / Normal / Surface / Emissive / Depth / Velocity）。
- **PVF (Per-Vertex Format) バインドレスジオメトリ**: 属性ごとの `ByteAddressBuffer` SRV を
  バインドレスのメッシュディスクリプタヒープ経由で扱う（入力レイアウト不要、メッシュごとのルートシグネチャ変更も不要）。
- **GPU フラスタム & クラスタカリング**（`InstanceCull.cs`、`ClusterCull.cs`、`ClusterCullProbes.cs`）。
- **Hi-Z 深度ピラミッド**（reverse-Z、2 チャンネル min/max）— `HiZGenerate.cs`、`HiZReduce.cs`。
- GPU が描画数を構築する **ExecuteIndirect** による間接描画パス。
- CPU 側クエリ（フラスタムテスト、ピッキング）のためにフレームごとに再構築されるシーン **BVH**。
- 四分木 LOD と amplification シェーダー駆動の CSM キャスターパスを備えた、メッシュシェーダー **地形パイプライン**（`Terrain.as / .ms / .ps`）。

## ライティング

- **Clustered Forward+ / Clustered Deferred** ライトカリング — 16×9×24 (3456) froxel クラスタ、
  クラスタごとに固定のスライス割り当て（アトミックカウンタ不使用）、トリプルバッファ化された 64 バイトリング内に
  最大 4096 ライト（`ClusterBuild.cs`、`ClusterCull.cs`）。リフレクションプローブは並列にカリングされる（`ClusterCullProbes.cs`）。
- 点光源系ライト: ディレクショナル、ポイント、スポット。スポットライトは必要に応じてシャドウをオプトインする。
- **Cascaded Shadow Maps (CSM)** — 4 カスケード（近距離 3 つ + メッシュシェーダー地形パス経由でレンダリングされる
  超遠距離の **地形専用** カスケード 1 つ）を 4096²×4 D32_FLOAT 配列に格納、reversed-Z、実用的な log+uniform 分割（λ=0.85、
  near…400、最遠カスケードは 2000 まで）、タイトなバウンディングスフィアフィット、**テクセルスナップのみ** の安定化
  （サブテクセルの Halton ジッターは TAA のシマーとして見えたため削除。ソフト化は空間 PCF の Bayer 回転で行う）。
- マテリアルごとの `ShadowCullMode`（Back / Front / None + アルファテストバケット）に、グループごとの深度バイアスと
  プレフィックス和による `ExecuteIndirect` キャスターグループを備える。
- **スポットシャドウアトラス** — オプトインのスポットライトシャドウ用に 2048²×8 スライス配列。キャスターが 1 つもオプトインしない場合は完全にスキップされる。
- **Cook-Torrance マイクロファセット BRDF**（GGX + Smith + Schlick）を不透明パスと透明パスで共有。
  ディファードシェーディングは 3 つのステンシルゲート付き PSO バリアント（PBR / NPR / Unlit）を実行する。

## グローバルイルミネーション

- **DDGI (Dynamic Diffuse Global Illumination)** — DXR / インライン `RayQuery` コンピュートトレース（RTPSO/SBT 不使用）。
  プローブごとの **L1 SH イラディアンス** を `StructuredBuffer<DDGIProbeSH>`（48 B）に格納 — 八面体
  イラディアンスアトラスとその境界 CS は削除され、Chebyshev 可視性のための 16²-テクセル深度アトラス（R16G16F）のみが残る。
  最大 **4 つのスロットインデックス付きボリューム**、`ExecuteIndirect` トレースを駆動するプローブごとの適応的レイ数バケット、
  プローブの再配置 + 分類、マルチバウンス（前フレームの SH/深度を読み取る）、
  フレームごとの van der Corput 回転（振幅 ∝ √(π/raysPerProbe)）、closest hit でのバインドレス
  テクスチャサンプリングによる Emissive サーフェスのサポート。`DDGISceneAS` はフレームごとに静的 + プロシージャル
  ジオメトリ上に TLAS を構築し、予算化された BLAS ビルド（16/frame）+ コンパクションを行う。Sponza / Bistro でチューニング & 検証済み。
- **リフレクションプローブ** — ランタイムベイクされる `TextureCubeArray`（最大 64 キューブ、128²、7 ミップ）: reverse-Z のシーン
  6 面 + スカイボックスを一時キューブに描画し、その後プローブごとに 42 回の GGX プレフィルタディスパッチ。
  ライティングシェーダー内でのクラスタ化プローブ選択、`SceneBVH::QueryAABB` 駆動のキャプチャカリング。
- **Screen-Space Reflections (SSR)** — FidelityFX-SSSR / Wicked Engine から移植された専用サブシステム
  （`Graphics/SSR/`、独自のコマンドリスト上で `SSRSubsystem` がオーケストレーションする）: DepthHierarchy（Hi-Z min/max）→
  Karis シーンカラーピラミッド → 確率的 GGX トレース → 空間 BRDF リゾルブ → デュアルリプロジェクションのテンポラル →
  バイラテラルアップサンプル → Fresnel×envBRDF コンポジット。**フルレンダリング解像度** で動作。LightingPass は
  1 フレームのレイテンシで IBL を (1−ssrConf) だけ減衰させる。任意の transient scratch-alias ヒープ。
- **Sky Spherical Harmonics** — 動的な空のアトモスフィアキューブを IBL ディフューズ用に 9 係数（L1）SH へ投影する。
  太陽が動いた / 強度が変化したときのみ再実行される。
- **ボクセルシーン** — 128³ R8_UINT Tier-2 三角形ボクセル化（`SceneVoxelize.cs`、`SceneVoxelClear.cs`）。
  ボリュメトリックフォグのオクルージョンソースとして使用。コンシューマがアクティブでないときはスキップされる。

## アトモスフィア & スカイ

- Hillaire 2020 スカイモデル: `AtmosphereTransmittance.cs`、`AtmosphereMultiScatter.cs`、
  `AtmosphereSkyView.cs`、`AerialPerspective.cs`（32³ 3D LUT）、`SkyAtmosphere.cs`。
  Transmittance + MultiScatter LUT は一度だけベイクし、フレームごとの処理は太陽の方向/強度に対してダーティチェックされる。
- 事前ベイクされた星（`StarsBake.cs`）、解析的な太陽 + 月のディスク、HDRI スカイボックスのビジュアルオーバーライド。
- IBL パイプライン — イラディアンス（9 係数の空 SH）+ ラディアンスキューブ（フレームごとに 1 面のテンポラル GGX プレフィルタ）+
  事前積分された BRDF LUT（`SpecularPrefilter.cs`、`GenerateLUT.cs`）。
- **ボリュメトリッククラウド** — 一度だけベイクされる 128³ ノイズボリューム、クォーター解像度の深度認識レイマーチ（`CloudRaymarch.cs`）
  と alpha-over HDR コンポジット。ECS の `CloudComponent`（高度帯、カバレッジ、密度、風）で駆動される。

## ボリュメトリクス

- **Froxel ボリュメトリックフォグ** — density → light-injection → scatter → temporal-reproject のパイプライン
  （`FroxelDensity.cs`、`FroxelLightInject.cs`、`FroxelScatter.cs`、`FroxelTemporal.cs`）。太陽（CSM）+
  クラスタ化されたポイント/スポット + スクリーンスペース + ボクセル占有によるシャドウイング、カメラテレポート時の履歴リセット。
- **ボリュメトリックレイマーチ** はハーフ解像度の god-ray ティアで、独自のテンポラルパスを持つ（`VolumetricRaymarch.cs`、
  `VolumetricRaymarchTemporal.cs`、`VolumetricRaymarchApply.ps`）。加算的にコンポジットされ、その後 froxel
  の透過率が HDR 上にコンポジットされる（合計でコンピュート 6 ステージ + グラフィックス apply パス 2 つ）。

## ポストプロセススタック

- **オートエクスポージャー** — 対数輝度ヒストグラム、目の順応。
- **ブルーム** — Karis 平均を用いた 13-tap "Sledgehammer" ダウンサンプル、3×3 テントアップサンプル。
- **レンズフレア** — プロシージャルなディレクショナルライトのフレアをトーンマップ前にコンポジットする。
- **TAA** — Karis 2014 / Salvi 2016 ハイブリッド: 最近接深度のベロシティ拡張、9-tap Catmull-Rom 履歴、
  ルマガンマ付き分散クリッピング（スペキュラ用に別シグマ）、ソフトエッジのディスオクルージョン、アンチフリッカークロス
  ブラー、Halton ジッター、アウトラインステンシルビット (0x80) による履歴の弱化。スキンメッシュは前ポーズのベロシティを保持する。
- **FXAA** — NVIDIA FXAA 空間パス。独立して選択するか、TAA の後にスタックできる（リゾルブの優先順位は
  FXAA > TAA > 生 HDR）。
- **XeGTAO** — Intel の XeGTAO メインパス + 5-mip 深度プレフィルタの忠実な移植。デノイズはテンポラル蓄積の **前** に実行され
  （分散クリップを引き締める）、ベロシティ + 前フレームのリニア深度によるディスオクルージョン棄却を伴う。
- **トーンマップ** — 露出 & カラーグレーディングを伴う最終的な HDR → LDR。
- **CAS** — AMD FidelityFX Contrast Adaptive Sharpening、LDS タイル化された HDR 対応の移植。
- **カラーグレーディング** パラメータブロック。
- **ポストプロセスボリューム** — Unreal スタイルのブレンド可能なボリュームシステム: Global / Box / Sphere SDF 形状を
  線形の `blendDistance` 減衰と、ステージごとの `std::optional` オーバーライド付きで備え、優先順位順の重み lerp による
  ブレンドを行う（`VolumeSystem` のスロットレジストリ + ECS の `EntityVolumeSource` + `ParameterBlender`）。加えて transient で
  時間駆動の `ScriptedOverrideSystem`（ダメージフラッシュ / フラッシュバンのフェードカーブ）を備える。ステージ化されたエフェクトチェーン
  （CAS → AutoExposure → Bloom → LensFlare → Tonemap）はゼロコストの `IsEnabled` ゲートとともに動作する。

## 特殊エフェクト

- **デカールシステム** — マテリアルライブラリ + クラスタカリングを備えた、クラスタ化されたスクリーンスペース投影デカール。
- **アウトライン** — 3 サブパス構成のシステム: 反転ハルのシルエット + Object-ID + normal/depth/ID 上の
  スクリーンスペース Roberts エッジクロス。
- **ガラス破砕** — トーンマップ出力をキャプチャし、破片の物理をシミュレートし、持続時間が経過するまでコンポジットする。
- **パーティクルシステム** — GPU の emit/update コンピュートパイプライン、リングバッファプール、間接描画レンダリング。
- **トレイルシステム** — GPU 駆動のリボントレイル、制御点のコンピュート更新。
- **トレーサーシステム** — 円筒ビルボードの "細いレーザー" トレーサー、GPU プール（4096 スロット）。
- **ビームシステム** — CS 生成のプロシージャルチューブによる重量級ビーム（`BeamTubeGen.cs`）、平行移動フレーム、
  Rodrigues 回転、任意の垂直ウォブル。共有 PVF 頂点バッファ + バインドレスメッシュディスクリプタ経由で **標準メッシュパス** を通してレンダリングされる
  （専用レンダーパスなし）。内側コア + 外側グローのシェーダー。
- **アフターイメージ / ゴースト** — スキンメッシュの頂点をバインドレスプールにスナップショットし（`AfterimageCopy.cs`）、
  ライフタイムフェード付きの加算 Fresnel リムゴーストとしてレンダリングする（`Afterimage_Ghost.ps`）。
- **スキンアニメーション** — GPU スキニングコンピュート（`Skin.cs`）、ポーズリングバッファ、モーフターゲット、CCD IK
  （地面認識の足 IK を含む）、ソケットアタッチメント、follow-entity / follow-socket コンポーネント。
- **ビデオコンポジット** — ハードウェアデコードされた NV12 → RGB (BT.709): フルスクリーンの `VideoPass` と深度テスト付き
  ワールドスペースの `VideoQuadPass`（[Engine Systems](ENGINE.ja.md) を参照）。
- **チェーン物理** — 髪 / スカート / 揺れボーン用の CPU Verlet + PBD スプリングボーンシミュレーション（GPU コストなし）。

## ワールドスペース & スクリーンスペース UI

- コンポーネントを **一切** 共有しない、厳密に分離されたサブシステム。
- **スクリーンスペース**: `UISystem` のウィジェットツリー（`UIRootComponent`）に加えてフラット ECS のアイテム
  （`UIScreenSpace/Image/Text/Bar`）。テクスチャ/マテリアル/クリップによるコマンドマージを行う `UIPass` を通してレンダリングする。
- **ワールドスペース**: `WorldSpaceUIComponent` + コンテンツコンポーネント（`WorldUIBar`、`WorldUIText`、
  `WorldUIImage`、`DamageNumberComponent`）。`WorldUIBillboardPass` でビルボード化される。`LocalTransform` +
  任意の `FollowEntity` によりエンティティ駆動される。

## その他

- **ピッキング**（`PickingPass`、`PickingID.vs/.ps`）— GPU リードバック → エンティティ ID 解決。
- **デバッグワイヤフレーム** — AABB / フラスタム / スプラインのオーバーレイ用 `DebugWirePass`。
- **DDGI プローブのデバッグ可視化** — プローブのトレース原点に配置されたインスタンス化された球。

---

## レンダーパスカタログ

`DX12/include/RenderGraph/RenderPass/` 配下に配置されている。

SSR サブパスは `DX12/include/Graphics/SSR/` 配下に、残りは `DX12/include/RenderGraph/RenderPass/` 配下に置かれている。

| カテゴリ        | パス                                                                                                  |
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
