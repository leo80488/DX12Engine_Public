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
- **プロシージャル草原**（`Grass.as / .ms / .ps`）— 頂点 / インスタンスバッファを **一切持たない**、完全に GPU 生成される二次 Bézier
  リボンブレード: amplification シェーダーがパッチをフラスタム & 距離カリング（ハッシュディザのディゾルブフェード、7/3/2 セグメント LOD）し、
  プレフィックス和でメッシュシェーダーグループを割り当てる。メッシュシェーダーはビット反転した層化グリッド上にブレードをハッシュ配置し（密度が変わっても位置が安定）、
  `TerrainComponent` のハイトマップに根元をアンカーし（バイキュービック）、Voronoi ライトなクランプと 2 オクターブのスクロール風（TAA ベロシティのため
  現在フレームと前フレームの **両方** で評価）を適用して、ディファード G-Buffer をすべて書き込む（ステンシル ref 1、PBR）。`GrassComponent` で駆動。
  メッシュシェーダー非対応時はグレースフルに自己無効化する。
- **平面水面**（`Water.vs / .ps`）— プロシージャルな `SV_VertexID` グリッド（バッファ不要）をスカイボックスの後に G-Buffer + HDR へ reverse-Z
  テスト/書き込みで描画するため、深度認識パス（クラウド / フォグ / SSR）からは実ジオメトリとして見える。ホワイトアウトブレンドされた 2 枚の逆スクロール
  フローノーマル、Fresnel スカイ反射（前フレーム SSR の信頼度で減衰）、CSM シャドウ付きの Blinn サンの輝き、地形ハイトマップからの解析的水深による
  deep/shallow 吸収 + clip-discard の岸際アルファフェード。モーションベクトルにより TAA/SSR 下でも安定する。`WaterComponent` で駆動。

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
- **ポイントライトシャドウキューブ**（`PointShadowPass`）— 最大 4 つの同時全方位キャスター。各々が `TextureCubeArray` の 1024²×6 面スライスを
  深度のみで描画し（`Shadow.vs/.ps` を再利用、フォリッジ用に ALPHA_TEST バケット）、面ごとに 90° の reverse-Z ビュープロジェクション（NearZ = ライト半径）を使う。
  `Lighting.ps` はワールド空間方向でサンプリングし（ハードウェアのキューブ面選択）、reverse-Z 参照深度を解析的に再構築する（ライトごとの VP 不要）。
  レシーバー側のノーマルオフセットバイアス付き。`LightData.castsShadow` でオプトインする。
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
- **ボリュメトリッククラウド** — Nubis / Frostbite 密度モデルによる球殻レイマーチ。3 つの一度だけベイクされるルックアップ —
  `CloudNoiseBake.cs`（128³ Perlin-Worley ベースシェイプ）、`CloudDetailNoiseBake.cs`（32³ Worley 侵食）、
  `CloudWeatherBake.cs`（512² のカバレッジ / 二次フィル / 雲タイプの気象マップ、風速の ¼ でスクロール）— が、
  クォーター解像度の適応的 coarse→fine レイマーチ（`CloudRaymarch.cs`）に供給される: 太陽方向への 5-tap コーン光学的深度、
  3 オクターブのエネルギー保存マルチスキャッター、デュアルローブ HG + シルバーライニング位相、雲タイプごとの高度勾配、IGN ジッター、ホライズンフェード。
  深度認識のバイラテラル 4-tap アップサンプル（`CloudComposite.ps`）が RGBA16F の結果をスカイボックスの後・フォグの前に alpha-over で HDR へコンポジットする。
  `CloudComponent`（高度帯、カバレッジ、密度、ディテール強度、気象スケール、風、マルチローブライティング、最大 192 レイステップ）で駆動される。

## ボリュメトリクス

- **Froxel ボリュメトリックフォグ** — density → light-injection → scatter → temporal-reproject のパイプライン
  （`FroxelDensity.cs`、`FroxelLightInject.cs`、`FroxelScatter.cs`、`FroxelTemporal.cs`）。太陽（CSM）+
  クラスタ化されたポイント/スポット + スクリーンスペース + ボクセル占有によるシャドウイング、カメラテレポート時の履歴リセット。
- **ボリュメトリックレイマーチ** はハーフ解像度の god-ray ティアで、独自のテンポラルパスを持つ（`VolumetricRaymarch.cs`、
  `VolumetricRaymarchTemporal.cs`、`VolumetricRaymarchApply.ps`）。加算的にコンポジットされ、その後 froxel
  の透過率が HDR 上にコンポジットされる（合計でコンピュート 6 ステージ + グラフィックス apply パス 2 つ）。
- **解析的ハイトフォグ**（`HeightFogApply.ps`）— UE スタイルの指数ハイトフォグ。各視線レイに沿った光学的深度を閉形式で積分し（`D.y→0` の Taylor 分岐と、
  空ピクセル用の `t→∞` 解析極限を持つ）、Henyey-Greenstein 位相によるサンインスキャッター、開始距離 / 最大不透明度のクランプを備える。深度は
  シルエットのハロを避けるため point-load（フィルタなし）。クラウドの **後**・froxel ボリュメトリックフォグの **前** にフルスクリーン（ONE / INV_SRC_ALPHA）で
  コンポジットされ、froxel の近距離スキャッタリングがその上に重なる。`HeightFogComponent` で駆動される。

## ポストプロセススタック

- **オートエクスポージャー** — 対数輝度ヒストグラム、目の順応。
- **ブルーム** — Karis 平均を用いた 13-tap "Sledgehammer" ダウンサンプル、3×3 テントアップサンプル。
- **被写界深度**（`DepthOfField.cs`）— フォーカス距離の錯乱円（CoC）ブラー: reverse-Z 深度を線形化し、トランジション帯で CoC をランプさせ、
  2 つの同心リング（8 + 16 タップ）を各タップ自身の CoC で重み付けしてギャザーする（scatter-as-gather のため、シャープな前景がボケた背景へにじまない）。
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
- **NPR スタイライズ**（`Stylize.cs`）— 独立してトグルできるモードを持つシングルディスパッチのノンフォトリアルスタック: Kuwahara
  （最小分散の象限）、Posterize、4×4 Bayer の順序ディザ、輝度駆動の Halftone ドット、複数角度のインク Crosshatch、Pixelate。
- **水中**（`Underwater.cs`）— 2 層のアニメーション正弦波スクリーン歪み + 水色ティント。カメラが水中にあるときトーンマップ前に適用される。
- **カラーグレーディング** パラメータブロック。
- **ポストプロセスボリューム & プロファイル** — プロパティごとの `Overridable<T>` 値を中心に再構築された、Unreal スタイルのブレンド可能なルックシステム。
  `PostProcessVolumeComponent` が共有のアセットベース `.ppprofile` をアタッチし（Global / Box-OBB / Sphere SDF 境界、線形 `blendDistance` 減衰、
  優先度、32 ビットのビューごと `layerMask`）、`PostProcessResolveSystem`（PreRender、カメラスタックのブレンド後なので視点位置が減衰をジッターさせない）が
  エンジンデフォルトをフラット化し、空間ボリューム + 一時的なゲームプレイオーバーライドを集め、優先度昇順でソートし、オーバーライドするプロパティ **のみ** を
  逐次 lerp してフラットな `ResolvedPostProcessSettings` に解決する。単一の X-マクロ（`PostProcessProperties.inl`）が唯一の真実の源として、各プロパティを
  プロファイル構造体・解決済み構造体・シリアライザ・エディタウィジェット・Lua バインディング（`PostProcess.spawnVolume / setOverride / pushTransient /
  getResolved`、fade-in/hold/fade-out のオーバーライドエンベロープ付き）へ自動的に配線する。`PostProcess::Stack` は疎結合のコンシューマで、解決済み設定に対して
  ゼロコストの `IsEnabled` ゲートとともにステージ化チェーン（DepthOfField → CAS → AutoExposure → Bloom → LensFlare → Underwater → Stylize → Tonemap）を実行する。

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
- **スプライトビルボード**（`BillboardFX.vs/.ps`）— アニメーションするスプライトシートビルボード（爆発 / ヒット / グロー）: CPU 展開のカメラ対向クアッド
  （球状 or 円筒状）、フリップブック再生（Loop / Once / PingPong）、加算 or アルファブレンド。トリプルバッファのアップロードリングから頂点プルされ、
  Bloom 越しに光るよう読み取り専用深度で HDR に描画される。距離で背面から前面へペインターソートし、テクスチャはオンデマンドでバインドレススロットへ解決される。`BillboardFXComponent` で駆動。
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
| **Geometry**   | GBuffer, Terrain, Grass, Skybox, Water, Transparent, Picking, DebugWire                              |
| **Shadows**    | Shadow (4-cascade CSM), SpotShadow, PointShadow (cube)                                                |
| **Lighting**   | Lighting, Decal, SkyIBL                                                                               |
| **GI**         | DDGI, DDGIProbeDebug, ReflectionProbeCapture, SceneVoxel                                              |
| **SSR**        | SSRTrace, SSRResolve, SSRTemporal, SSRUpsample, SSRComposite, SSRDepthHierarchy, SceneColorPyramid    |
| **Volumetric** | VolumetricFog (froxel + half-res raymarch, 6 compute + 2 apply), Cloud (3 bake + raymarch + composite), HeightFog |
| **Culling**    | Culling, Cluster, HiZ                                                                                 |
| **Skinning**   | Skinning                                                                                              |
| **VFX**        | Particles, Trails, Tracers (sim+render), BeamSim, AfterimageCapture, BillboardFX, GlassShatter        |
| **Outline**    | Outline (3 sub-passes)                                                                                |
| **Post**       | TAA, FXAA, AutoExposure, Bloom, DepthOfField, LensFlare, XeGTAO, CAS, Stylize, Underwater, ToneMap, ColorGrading |
| **Video**      | Video (screen-space), VideoQuad (world-space)                                                         |
| **UI**         | UI (screen), WorldUIBillboard                                                                         |
