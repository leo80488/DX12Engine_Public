[English](ARCHITECTURE.md) | **日本語**

# アーキテクチャ

| レイヤー         | 役割                                                                            |
| ---------------- | ------------------------------------------------------------------------------- |
| **ECS World**    | すべてのエンティティ／コンポーネントのデータ — 純粋なゲーム側の状態。            |
| **Scheduler**    | ECS World に対する 16 フェーズの依存関係を考慮した並列システムtick。             |
| **Renderer**     | ECS データを GPU 用の `DrawPacket` に変換する（DX12 型は含まない）。`Renderer.cpp` と `Renderer_Scene/_DDGI/_IBL/_Terrain/_Accessors.cpp` に分割。 |
| **RenderGraph**  | ラムダおよびクラスベースのパス DAG、トランジェントリソース、自動バリア挿入。     |
| **RenderPass**   | 個々のパス。RHI 型のみを扱う。                                                  |
| **RHI**          | 抽象的なコマンドリストおよびリソースハンドル（`RHICommandList`、`RGTextureHandle`）。 |
| **DX12 Backend** | `IGraphicsDevice` の背後に隠蔽された具体的な D3D12 実装（`GraphicsDX12.cpp` と `_Resources/_Translation/_Capture.cpp`）。 |

## System Scheduler

- **16 個の順序付き tick フェーズ**（`TickPhase`）: Input → Gameplay (Pre/Logic/Post) → AI → FixedPhysics (Pre/Step/Post) → PhysicsInterpolation → Animation → BoneAttachment → SecondaryPhysics → PreRender → Render → PostRender。
- 各フェーズディスクリプタは `isFixedTimestep` ／ `allowsParallel` ／ `requiresMainThread` をフラグで示す。フェーズ境界が**唯一**の同期点である。
- **依存関係を考慮した並列バッチ化**: システムは `Read<T>` ／ `Write<T>` ／ `ExclusiveResource(id)` のアクセスを宣言する。世代キャッシュ付きの線形貪欲プランナーが、競合しないシステムをバッチにまとめて `TaskSystem` 上でディスパッチする。
- **遅延構造編集** — `CommandBuffer` が `AddComponent` ／ `RemoveComponent` ／ `DestroyEntity` をキューに入れ、フェーズ境界でフラッシュするため、フェーズ N+1 は一貫したワールドを観測する。
- `FrameContext` はスケーリングされていない `deltaTime` とスケーリングされた `scaledDeltaTime`（ヒットストップ／バレットタイム）を分離し、`physicsAlpha`、フレームインデックス、エディタのプレイ状態ゲートを保持する。

## Frame & Resource Management

- トリプルバッファリングのスワップチェイン（`FrameCount = 3`）、エディタビューポート用に別個の HDR シーンレンダーターゲット。
- **4 個**のディスクリプタヒープアロケータ（RTV ／ DSV ／ CBV-SRV-UAV ／ Sampler）。それぞれ静的なフリーリストによる再利用に加え、フレームごとのディスクリプタテーブル用にフェンスで回収される動的リングリージョンを持つ。
- フレームごとのデータ（インスタンス、ライト、マテリアル、テレイン、シャドウ VP、インダイレクト引数）向けに、永続的にマップされた UPLOAD ヒープのリングバッファ。
- **マルチキューコマンドリストモデル**: `BeginCommandList(queue)` はスレッドセーフである。パスは `AddCommandListDependency` で順序を宣言する。`EndFrame` は記録されたリストを Kahn のトポロジカルソートで並べ替え、キュー境界（graphics ／ compute ／ copy）をまたぐ場合に**のみ**キューごとの GPU フェンス Wait/Signal を挿入する。
- **キュー間非同期コンピュート**: `SetExternalWait(pass, depCL)` は、消費側のパスのちょうどそこに、フレームごとに一度きりの GPU 待機を配置する — 例えば DDGI 非同期コンピュートリストは `LightingPass` で解決される一方、GBuffer ／ Shadow ／ SkyIBL はオーバーラップし続ける。
- スロットベースのフレームごとの遅延解放に**加えて**、3 フレームを超えて存続する非同期コンピュートリソース向けにフェンスをキーとした遅延解放。トランジェントな placed リソースのエイリアシング（`CreateTexturePlaced`）。
- ディスク上の PSO ライブラリキャッシュ（`pso_cache.bin`）と DXIL シェーダーブロブキャッシュ（`shaders/shader_cache_dxil/`）。
- エンジン全体にわたる Reverse-Z 深度。

## Shader Pipeline

- **すべての** HLSL に DXC（DXIL、SM 6.6、HV 2018。RT には `lib_6_5`）を使用 — `D3DCompile` は完全に廃止された。
- ソース＋パーミュテーション＋エントリをキーとする `.ishdr` ブロブキャッシュ。推移的な `#include` の mtime の陳腐化によって無効化される。
- `ReadDirectoryChangesW` ＋ パスごとの `ReloadShaders()` 仮想関数によるシェーダーのホットリロード。
- 失敗した DXC コンパイル＋失敗した PSO 作成のためのネガティブキャッシュ。これにより壊れたシェーダーが毎フレームログを埋め尽くすことを防ぐ。

## Frame Execution

`RenderGraph::Execute` は各フレームを 3 つのフェーズで実行する:

1. **シリアルセットアップ** — パスごとに 1 つのコマンドリストを開き、ビルトインターゲットを設定し、バリアを発行し（仮想テクスチャごとの状態トラッカーを介した `EmitBarriersBeforePass`）、同一キュー＋外部のキュー間依存をチェーンし、GPU タイムスタンプリージョンを開く。
2. **並列記録** — パスは `TaskSystem::ParallelFor` を介して並行に記録される（しきい値は 4 パス以上。それ未満ではシリアル）。
3. **シリアルクローズ** — タイムスタンプリージョンを閉じる。`GPUProfiler` がキューごとのタイミングと、クリティカルパスの「実効」フレーム ms（graphics ／ compute ／ copy の最大値）を報告する。

## Notable Engineering Details

- **すべてバインドレス** — メッシュ（16384 スロットの `g_Buffers[]` テーブルを介した属性ごとの `ByteAddressBuffer` SRV）、マテリアル（`MaterialGPUData` 内のテクスチャインデックス）、シャドウカスケード、リフレクションプローブ、DDGI ボリューム。
- **透過マテリアルでのカスタムシェーダーパス** — `TransparentPass` は `dp.customPSID` を尊重するため、任意の加算ブレンドマテリアルがカスタム PS を使用できる（エネルギーフィールド、ホログラム、ビームの外側グロー）。
- **マテリアルスキーマ** — 単一の `kMaterialPBRSchema` テーブルがシリアライザとインスペクタを駆動する。編集可能なフィールドの追加は 1 行の変更で済む。
- オンディスクの `ID3D12PipelineLibrary` 上の **PSO パーミュテーションキャッシュ**（シェーダー define マスク＋レンダーステートをキーとする `PermutationKey`）。
- VFX（トレーサー、ビーム、パーティクル、トレイル）向けの**事前割り当て GPU プール**。これにより割り当てコストは起動時に償却される。
- **アウトライン／トレーサー／ビームの手動バリアパターン** — 自動バリアトラッカーが曖昧さを解消できない depth-as-SRV-and-DSV のケースについてはグラフをバイパスする。
- **プラグマ駆動のリンク** — ほとんどのサードパーティライブラリは `#pragma comment(lib, ...)` を介してインラインでリンクされる。ビルドにはライブラリ検索パスのみが必要である（[BUILD.ja.md](BUILD.ja.md) を参照）。
