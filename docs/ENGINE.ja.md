[English](ENGINE.md) | **日本語**

# エンジンシステム

## ECS

データ指向の **コンポーネントごとプール (sparse-set)** ECS (`ECS::World`)。各コンポーネント型は `sparse[entity] → dense index` と並列の dense な `owner`/`value` 配列を持つプールを所有しており、`GetComponent` は 2 回の配列ロードで済み、`ForEach<T>` は `T` を持つエンティティのみを走査する。エンティティ ID はフリーリストで再利用され、生成・破棄は末尾とのスワップにより O(1) である。

- **`EntityHandle`** — 世代スタンプ付きの参照 (`IsHandleValid`) であり、長寿命の参照（フォローターゲット、イベントペイロード、アタッチメント）における再利用スロットのエイリアシングを防ぐ。
- **エンティティ破棄リスナー** はコンポーネント消去の*前*に発火するため、キャッシュ（レンダラのテクスチャ、`GuidRegistry`）を確定させられる。リスナーは `World::Clear()` を超えて存続する。
- コンポーネントには、シーングラフ (`Parent`, `Children`, `LocalTransform`, `GlobalTransform`, `Visibility`, `RenderLayer`, `WorldAabb`)、アニメーション／モーフ／ソケット／フォロー、物理＋キャラクターコントローラ、AI／ナビ／インテント、オーディオ、ビデオ、UI、デカール、地形、ビーム／パーティクル／トレイル、リフレクションプローブ／DDGI／クラウド／TOD が含まれる。

## システムスケジューラ

16 フェーズのティックパイプライン (`TickPhase`) がすべてのシステムを駆動する。フェーズ一覧と依存関係を考慮した並列バッチングモデルについては [ARCHITECTURE.ja.md → システムスケジューラ](ARCHITECTURE.ja.md) を参照。システムは `App::RegisterTickSystems` (`SystemRegistry`、フェーズごとに分割) で一度だけ登録され、`EngineSystems.cpp` で `ISystem` アダプタとしてラップされる。

- **`CommandBuffer`** — `AddComponent` / `RemoveComponent` / `DestroyEntity` / `Defer(lambda)` を遅延させ、フェーズ境界でフラッシュする。
- **`FrameContext`** — スケールなしの `deltaTime` と `scaledDeltaTime`（ヒットストップ／バレットタイム）、`fixedDeltaTime`、`physicsAlpha`、フレームインデックス、エディタの再生状態ゲート (`runUpdate`)。
- **`Command` API** — 高レベルのゲームプレイ編集 (`EquipToSocket` / `UnequipItem` / `AttachToEntity` / `DetachFromEntity`) であり、`EventBus` に装備／装備解除イベントを発行する。
- **`LifetimeSystem`** — *スケールなし*の dt で `LifetimeComponent` をカウントダウンし、自動破棄する（時限式 VFX のクリーンアップ）。
- **GUID システム** — RFC-4122 v4 の `Guid` と、プロセスグローバルな `GuidRegistry` Guid↔Entity 双方向マップ（破棄時に自動登録解除、シーンロード後に再構築）。`AttachmentRef` はスタンプ付き `GuidComponent` を O(1) のキャッシュ高速パスで解決する — セーブ安定なエンティティ間参照の基盤である。

## リソースシステム（オフラインクック＋ランタイムハンドル）

- **インポーター** — Mesh (OBJ) / Scene (Assimp FBX・glTF・OBJ・DAE) / PMX / VMD / VRM (MMD) / Skeleton / Animation / Texture / Shader / Material / Decal-Material / Audio。
- **クック済みフォーマット** — `.meshlib`、`.imsh`、`.imorph`、`.imat`、`.itex`、`.ishdr`、`.ianim`、`.iskel`、`.iscn`、`.iworld`、`.aclip`、`.inav`。すべてのブロブは `[24-byte AssetHeader][typed Metadata][payload]`（four-CC マジック＋バージョン）であり、`GetMetadata` / `GetPayload` 経由でコピーなしにアクセスする。
- **`.meshlib`**（正規のメッシュコンテナ） — 1 つの共有頂点プール＋1 つの共有 `uint32` インデックスプール＋安定した `meshId` でインデックスされる `MeshLibraryEntry` テーブル（メッシュごとの範囲、AABB、デフォルトマテリアル）。頂点レイアウトは 2 種類：32 バイトのレガシー (pos+normal+uv) と、`float4` の tangent を持つ 48 バイト (`MESHLIB_FLAG_HAS_TANGENT`)。レガシーのマージ済み `.imsh` プールと `.imshpack` アーカイブは P1–P6 のリライト中に廃止された。`.imsh` は現在、OBJ インポーターの出力およびレガシーのデコードパスとしてのみ存在する。
- **`AssetFS` / `.ipak`** — 読み取り専用の仮想ファイルシステム：単一の `.ipak` アーカイブ（`tools/pack_assets.py` でビルド）を正規化パスのインデックスとともに RAM に丸ごと読み込み、ロックフリーの `memcpy` で提供する。透過的なルーズファイル `std::ifstream` フォールバックを備えるため、pak なしでもエディタが動作する。
- **ランタイムシステム** — `TextureSystem`（path→GPU テクスチャの参照カウント、DXGI→RHI フォーマットテーブル、バッチ式遅延アップロード）、`MeshLibrary` / `MeshSystem`（DEFAULT ヒープの RAW VB/IB）、`MaterialSystem`（CBV リングバッファアップロード）、`AnimationClipSystem`、`DecalMaterialLibrary`。
- **テクスチャパイプライン** — `BCCompressor` は専用の D3D11 デバイス（WARP／CPU フォールバック）経由で GPU BC6H/BC7/BC4/BC5 を実行する。`TextureImporter` はファイル名のキーワードによって `TexRole` を分類し（Color→BC7 sRGB、Normal→BC5、SingleChannel→BC4、HDR→BC6H、加えて非圧縮の RawHeight / RawLUT2 / RawColor ロール）、ミップを生成する。
- **非同期ロード** — `ResourceManager` は世代付きの `Handle` を即座に返し、I/O ＋インポート＋クック済みキャッシュ書き込みを `TaskSystem` のワーカー上で実行し、GPU アップロードをメインスレッドでタイムバジェット内にポンプする。
- **コリジョンベイク** — `Tools/CollisionMeshBaker` は **meshoptimizer**（weld／simplify／vertex-fetch）を使用して、Jolt メッシュシェイプ用にマージ済みのコリジョン `.meshlib` ブロブをベイクする（＋デバッグワイヤーフレームオーバーレイ）。

## シーングラフ＆ゲームフロー

- **`GameModeStack`** (`IGameMode`) が現役のシーンフローモデルである：Title → Game → End（加えて Test、ShaderLab）。それぞれワールドを `Clear()` し、`.iscn` を `LoadScene` する（あるいはフォールバックのカメラ／ライト／スカイボックスをスポーンする）。遷移は `requestReplaceMode` 経由。`GameScene` は `game.json` から `startup_scene` を解決する。（旧来の `SceneManager` / `IScene` スタックは並行して存在するレガシー抽象であり、現役パスにはない。）
- **`SceneInstanceLoader`** — ランタイムで Assimp 非依存の `.iscn` インスタンサ（`SceneLoader` のランタイムパスは廃止）。
- **`TransformSystem::Propagate`** — BFS による階層更新であり、`Visibility::inherited_hidden` を伝播し、`WorldAabb` を再計算する。定常状態ではアロケーションフリー。

## 入力

集約されたポーリング型の入力 (`Input` シングルトン、`Input/InputSystem`)：フレームごとに 1 回、`GetAsyncKeyState` を 256 バイトの curr/prev テーブルにスナップショットし、`IsKeyDown`、`WasKeyPressed/Released` のエッジクエリ、`Axis(pos, neg)` を公開する。フォーカス喪失時にはリセットする。（マウスは `System/Mouse` に残る。）

## プレイヤー＆キャラクター制御

- **`PlayerControllerSystem`** — XZ 基底上のカメラ相対 WASD、正規化された斜め移動、空中制御スケーリング、歩行／走行、進行方向への旋回、`CharacterControllerComponent` を駆動するコヨーテタイム＋ジャンプバッファのジャンプパイプライン。
- **`CharacterControllerComponent`** — `JPH::CharacterVirtual` KCC モーター（collide-and-slide、ステップアップ、グラウンドスナップ、傾斜制限）であり、Jolt のワールド重力とは別個のゲームプレイ重力と `MovementMode`（Walking / Falling / Swimming / Climbing / LaunchedTraversal）を持つ。`RigidBody` とは相互排他。
- **`CharacterStateSystem`** — Lua で記述された状態ごとの**クロスフェードステートマシン**（遅延クリップの取得／バインド、`pendingIdx → currentIdx` ブレンド）。`Character.AddState/SetState/...` として公開される。

## カメラ

- **`CameraSystem`** — FPS／フリーフライカメラに加え、`PhysicsSystem` に対するスプリングアームのコリジョン球スイープを伴う一人称／三人称フォロー。
- **`CameraStackSystem`** — 優先度／重み付きの**バーチャルカメラスタック**（Cinemachine 風）：チャンネルごとのハッシュ ID、VCam ごとのブレンドステートマシン、`tan(fov/2)` の FOV ブレンド、最短経路の nlerp 回転、重み付きの複数寄与解決を `LiveCameraComponent` へ、テンポラルパス向けのハードカットシグナリング、trauma² 駆動のシェイク。`Camera.PushVCam/PopVCam/HardCutTo/AddShake/...` として公開される。

## アニメーション

- スケルタル — Assimp / VMD / VRM のクリップインポート、主↔副の**クロスフェード**ブレンド、モーフターゲット、CCD IK、ソケット、Follow-entity / Follow-socket。（注：クロスフェード FSM であり、汎用のブレンドツリーではない。）
- `SkinnedMeshSubsystem` ＋ `SkinningPass`（コンピュート `Skin.cs`）、トリプルバッファリングされたスキニング用の `PoseRingBuffer`。前フレームのスキン済み位置が TAA の速度に供給される。
- **MMD サポート** — 回転付与（付与／D-bone）ボーンを pre-IK / post-IK に分割。CCD-IK ソルバは *saba* から移植（平面モード、角度制限、オイラー分解）。
- **フット IK** — `FootIKTargetSystem` はボーン名によって左右の足の IK チェーンを自動検出し、物理経由で地面をレイキャストし、IK 解決の前に加算的な地形 Y 差分を適用する。
- `.ianim` は SOA ボーンクリップ＋モーフクリップ＋アニメイベント＋オプションの末尾 AnimNotify JSON セクションを保持する。

## 物理

- **Jolt Physics**（60 Hz 固定ステップ）を `PhysicsSystem` 経由で統合し、分割されたスケジューラエントリポイント (`PreAllSteps` / `StepOnce` / `PostAllSteps`) により固定フェーズの処理をサブステップ間で実行する。
- 8 カテゴリのオブジェクトレイヤーマトリクス（StaticEnv / DynamicProp / Character / Ragdoll / HurtBox / AttackBox / Projectile / Sensing）を、カスタムブロードフェーズ＋レイヤーペアフィルタ、およびクエリ用のランタイム `LayerMask` とともに備える。
- `RigidBodyComponent` ＋ `ColliderComponent` / `CapsuleColliderComponent`。遅延ボディ生成。シェイプキャスト＋レイクエリ (`CastRay/Capsule/SphereClosest`)。バッファされた接触は `ContactBeganEvent` として `EventBus` へドレインされる。`physicsAlpha` によるレンダーポーズ補間。並列メッシュシェイプのプリウォーム＋ブロブキャッシュ。
- **チェーン物理** — `ChainPhysicsSystem` は **CPU** の副ボーンソルバ（髪／スカートチェーン向けに 6 カラーグループの Verlet ＋ PBD 距離拘束、Chest/Butt の揺れボーン向けにスプリングダンパ ODE、カプセルコライダ衝突）であり、`SecondaryPhysics` フェーズで実行される。コンピュートシェーダーでは*ない*。

## ナビゲーション

- **Recast / Detour** ラッパー (`Nav::NavMeshSystem`)：ワールドメッシュコライダから収集した三角形スープからの単一タイルおよびタイル分割ナビメッシュベイク（エディタの「Build NavMesh」）、`.inav` シリアライゼーション、`FindPath`（直線パス）、レイキャスト、点投影。デバッグラインは `DebugWirePass` 経由。
- **`NavAgentSystem`** — パスファインディング＋ステアリング：到達／減速半径を伴うウェイポイント追従、リパスゲーティング、nlerp slerp による向きモード（FaceMovement / FaceTarget / Manual）、CCC ブロック型のスタック検出＋リプラン、オフメッシュリンク踏破のハンドシェイク。KCC の `desiredHorizontalVelocity` を書き込む。

## AI

- **ビヘイビアツリー**ランタイム（`BTAsset` 共有ツリー＋エンティティごとの `BTInstance`）：コンポジット（Sequence / Selector / Parallel）、デコレータ（Inverter / Repeater / Cooldown / BlackboardCondition）、リーフ（Action / Condition）、加えて `UtilitySelector` と `SubTree`。ツリーは **Lua** で記述され、C++ ノードツリーへパースされ、mtime ポーリングによりホットリロードされる。ティックごとにエディタの BT ビジュアライザ用のトレースを記録する。
- **`ActionRegistry`** — ネイティブ C++ を先に試し次に Lua という (`Actions[]` / `Conditions[]`) ディスパッチ（現状はすべて Lua）。
- **レイヤー化された移動** — `AIIntentComponent`（戦略的ゴール）→ `AITacticalSystem`（Idle / Patrol / Investigate / Attack / Follow / Flee → ナビフィールド）→ `NavAgentComponent` → `CharacterControllerComponent`。
- **`AILODSystem`** — 距離ティアによるティックレートのスロットリング（boss / combat / distant / offscreen）。
- 文字列キー付きの `BlackboardComponent`（8 型バリアント）が Lua との間でラウンドトリップされる。

## スクリプティング

- **Lua 5.4** ＋ **sol2**、1 つの共有 VM。スクリプトカテゴリ：Logic（エンティティごとの `OnSpawn/OnUpdate/OnDestroy`）、System（グローバルな `OnInit/OnUpdate/OnShutdown`）、Service（ステートレス）、UI、データとしての Lua である Config。
- `LuaBus` イベントキュー (`Engine.Subscribe/Publish`) を C++ の `EventBus`（`ContactBeganEvent` を含む）へブリッジ。
- `FileWatcher` 経由のホットリロード。World のエンティティ破棄リスナー経由の同期 `OnDestroy`。
- ヒットストップ／バレットタイム向けのタイムスケール (`SetTimeScale`)。`Engine.AfterDelay` タイマーは実時間（スケールなし）の dt でティックする。
- **エディタ公開のスクリプト変数**（スキーマ＋エンティティごとのオーバーライドを `OnSpawn` の前に注入）。
- バインディングモジュール：数学型、`Input`、`Time`、`Camera`、`Animation`、`Physics.Raycast`、`Nav.*`、`Character.*` / `Player.*`、`Intent.*` / `AI.*`、BT アクション、`ui` ウィジェット、ECS コマンド。

## オーディオ

- **XAudio2 ＋ X3DAudio** (`AudioEngine`)、マスタリングボイス＋5 つのサブミックスバス（Music / SFX / Voice / Ambient / UI）。固定スロットテーブル上の世代スタンプ付きボイスハンドル。
- `AudioSystem` はイベント駆動 (`PlaySound` / `StopSound` / `SetAudioParam` / `BusVolumeChanged`)。`AudioClipSystem` はクリップをパスで重複排除する。`Audio3DSystem` は有限差分速度からドップラーを導出する。
- `AudioImporter` は RIFF/WAVE（PCM のみ v1、cue/label テーブル）を **`.aclip`** にクックする（`AudioClipLoader` がそれを読む）。

## ビデオ

ECS の `VideoComponent` と `VideoSystem` によって駆動されるハードウェアビデオ再生：

- **デコード** — FFmpeg ベースの `Mp4FrameSource`（`WITH_FFMPEG` 下でビルド）は **D3D12VA** ハードウェアデコードのためにエンジン自身の `ID3D12Device` を FFmpeg に渡す。透過的なソフトウェア (`sws_scale`) フォールバックを備える。または、手書きの `VideoDecoderDX12`（`ID3D12VideoDecoder`、DXVA H.264 / HEVC Main / Main10）を、クロスキューのグラフィックス Wait フェンス付きで専用の VIDEO_DECODE キュー上で実行する。いずれも NV12 テクスチャを生成する。
- **コンポジット** — `VideoPass`（フルスクリーン）と `VideoQuadPass`（深度テスト付きのワールド空間クアッド）が NV12 → RGB (BT.709) 変換を実行する。作者主導の再生のための組み込み SMPTE カラーバーテストパターンが存在する。

## AnimNotify / タイムライン

- **`TimelineSystem`**（Animation フェーズ）は 2 つの加算的なソースから `AnimNotify` / `NotifyState` イベントをディスパッチする：クリップで記述された `notifyTracks` と、エンティティごとの `TimelineComponent` オーバーライドトラックであり、Begin/Tick/End エッジを伴うループ折り返し型の `(prev, curr]` 時間窓を使用する。
- イベントは Animation フェーズで書き込まれる 6 つの `Pending*` **メールボックスコンポーネント**（Hitbox / VFX / Camera / Audio / StateToggle / Generic）を経由し、同一フレーム内の BoneAttachment フェーズで各コンシューマシステムによってドレインされる — 最も作り込まれているのは `VFXSpawnSystem`（純 ECS の Particle/Trail/Beam/Mesh エミッタ＋Tracer/Decal/Afterimage 向けの Renderer メールボックスリクエスト、アタッチモードと `VFXPrefab` キャッシュ付き）である。
- `NotifyIO` は notify トラックを JSON との間でラウンドトリップする（Timeline エディタで使用）。

## 時刻（Time of Day）

`TODSystems`（`BuildRenderScene` から駆動される単一関心の 5 システム）：`timeOfDay` を進める／折り返す、時角＋緯度から太陽方向を評価する、昼夜の smoothstep で太陽／月の色をブレンドする、結果を `SunLightTag` / `MoonLightTag` ライトエンティティへ同期する。

## リフレクション

- `Reflect::Descriptor<T>` ＋ `REFLECT_BEGIN/END` マクロ — 宣言的でヘッダオンリーの構造体フィールドメタデータ（約 80 個のコンポーネントデスクリプタが `ComponentReflection.h` に存在する）。
- エディタのコンポーネントインスペクタ（自動生成された ImGui ウィジェット、手書きブロックを排除）を駆動する。
- フィールド種別：scalar / color3+4 / enum / bool / string / quaternion-as-Euler / direction-as-AzEl / angle / slider / `std::vector` ＋固定配列（add/remove）／drag-drop string / 条件付き (`REFLECT_IF`) ＋折りたたみヘッダグループ／info-text / カスタム描画フック。
- `MaterialSchema` は同じアイデアをマテリアルパラメータに適用する（スキーマ駆動の `.imat`）。`MaterialReflectionSync` は DXC シェーダーリフレクションからカスタムシェーダーのパラメータ／テクスチャを自動投入し、エンジン予約のバインディングをフィルタリングする。

## エディタ (`Editor.exe`)

- **ImGui ドッキング**レイアウト：Hierarchy / Viewport / Inspector / Resource / Log。**F11** でビューポートをフルスクリーン化。
- **アセットブラウザ** — ファイルシステムスキャン、ライブな BC テクスチャサムネイル（フレームごとのロードバジェット）、FA アイコン、シーン／インスペクタへのドラッグドロップ。
- **インスペクタ** — スキーマ駆動のマテリアル（PBR / NPR / Unlit ＋カスタムシェーダー）と、ハイブリッドな `postDraw` フック付きのリフレクション駆動コンポーネントインスペクタ。カテゴリ別の Add-Component メニュー。
- **ビューポート** — GPU ピッキング、カメラ向き平面上でのドラッグ移動、ImGuizmo の translate / rotate / scale、Play / Pause / Step-Frame コントロール。
- **フローティングウィンドウ** — Timeline（アンドゥ／リドゥ付きの AnimNotify 記述）、SSR Debug（ステージごとのプレビュータイル）、UI Font Editor（CJK 範囲対応のライブ FreeType 再ベイク）、Camera Switcher（バーチャルカメラスタック）、Animation Debug（ボーンマトリクスの NaN/Inf 診断）、Profiler、Phase Debug、Post-Process、Decal Materials。
- **ツール／ビルド** — Bake Collision Meshes、Bake NavMesh、Bake Reflection Probes、PNG へのフレームキャプチャ、Glass-Shatter トリガー、パスごとのデバッグトグル、Set Startup Scene（`game.json` を書き込む）、Package Game。
- **`EntityRefPicker`** — `GuidComponent` の自動スタンプを伴うドラッグドロップ式 `AttachmentRef` バインディング。

## ShaderLab (`ShaderLab.exe`)

- `EngineCore.lib` 全体＋`EditorLayer` を再利用し、シェーダーイテレーション向けに調整されたメッシュ＋ライトプリセット＋IBL シーンへ起動する。メッシュ切り替え（Cube / Sphere / Cone / Plane / Torus）、ライトプリセット（Three-Point / Key-Only / Pure-Black）、`asset/IBL/<name>/` からの HDRI 切り替え、ターンテーブルオービット、フレームキャプチャ。
- `.ps.hlsl` を Shader Path フィールドにドラッグ → `MaterialReflectionSync` がインスペクタを自動投入する。
- **NPR ランプエディタ** — 行ごとに 2 ストップのグラデーションを持つ 512×25 RGBA アトラスであり、バインドレスインデックス経由でマテリアルの RAMPMAP スロットへライブバインドされる。

## ツール

- **アセットパッカー** (`tools/pack_assets.py`) — アセットをクック＋バンドルして `.ipak` にする。
- **ゲームパッケージャー** (`tools/package_game.py`) — 配布用に `Game.exe` ＋クック済みアセットをパッケージングする。
