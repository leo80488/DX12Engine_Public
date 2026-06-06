[English](README.md) | **日本語**

# DX12Engine

<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-20-54" src="https://github.com/user-attachments/assets/c2865b65-ad47-487d-9a8b-43042034a2f0" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-31-25" src="https://github.com/user-attachments/assets/3b6ea120-9fa1-4b4b-a96d-f1d7775dcb7a" />
<img width="1920" height="1080" alt="Screenshot 2026-05-10 03-55-46" src="https://github.com/user-attachments/assets/9c14df46-96ef-4418-9329-eca21c4c4903" />

C++20 / Direct3D 12（Shader Model 6.6、DXC 経由の DXIL）で記述されたリアルタイムレンダリングエンジンおよびエディタ。

## Demo

> サムネイルをクリックすると YouTube で再生される。

<table>
  <tr>
    <td><a href="https://youtu.be/6CPXLe-J1sw"><img width="400" src="https://img.youtube.com/vi/6CPXLe-J1sw/hqdefault.jpg" alt="DX12Engine demo 1"></a></td>
    <td><a href="https://youtu.be/mi7Asi6O87Y"><img width="400" src="https://img.youtube.com/vi/mi7Asi6O87Y/hqdefault.jpg" alt="DX12Engine demo 2"></a></td>
  </tr>
  <tr>
    <td><a href="https://youtu.be/0zyT00WBlOE"><img width="400" src="https://img.youtube.com/vi/0zyT00WBlOE/hqdefault.jpg" alt="DX12Engine demo 3"></a></td>
    <td><a href="https://youtu.be/juu7YkzafQs"><img width="400" src="https://img.youtube.com/vi/juu7YkzafQs/hqdefault.jpg" alt="DX12Engine demo 4"></a></td>
  </tr>
</table>

## Features

**Rendering**
- バインドレスな per-vertex-format（PVF）ジオメトリと ExecuteIndirect による GPU カリングを備えたディファード G-Buffer
- クラスタード Forward+ / ディファードライティング（16×9×24 フロクセル）、4 カスケード CSM（近距離 3 + 超遠距離地形 1）+ スポットシャドウアトラス
- DDGI（インライン RayQuery、プローブごとの SH イラディアンス、最大 4 ボリューム）、ランタイムベイクのリフレクションプローブ、Hi-Z 確率的 SSR サブシステム
- Hillaire 2020 大気、sky-SH IBL、IBL キューブ + BRDF LUT、ボリュメトリッククラウド
- フロクセルボリュメトリックフォグ + ボリュメトリックレイマーチによる god-ray
- TAA + FXAA、XeGTAO、Bloom（Sledgehammer）、CAS、Auto-Exposure、Lens Flare、Tonemap、Color Grading
- メッシュシェーダー地形パイプライン、デカール、アウトライン（3 パス）、glass-shatter、GPU パーティクル / トレイル / トレーサー / ビーム / アフターイメージ
- GPU スキニング + モーフターゲット + CCD IK（地形対応のフット IK を含む）+ ソケット + チェーン / スプリングボーン物理
- ハードウェアビデオデコード（FFmpeg D3D12VA → NV12 YUV→RGB コンポジット、スクリーン空間およびワールド空間）

**Engine**
- コンポーネントごとのプール（sparse-set）ECS、16 フェーズの依存関係を考慮した並列スケジューラ、シーングラフ階層
- ポストプロセスボリュームシステム、AnimNotify / Timeline ランタイム、time-of-day、GUID 安定なエンティティ参照
- リソースクッカー（Mesh / `.meshlib` / Material / Texture / Animation / Skeleton / PMX / VMD / VRM / Audio）+ `.ipak` 仮想ファイルシステム
- 非同期ロード、GPU BC 圧縮、ディスクリプタヒープアロケータ、PSO + DXIL シェーダーブロブキャッシュ、ホットリロード
- Jolt Physics + キネマティックキャラクターコントローラ、Recast/Detour ナビゲーション、ビヘイビアツリー AI（Intent / Tactical / LOD レイヤ）
- Reverse-Z、すべてをバインドレス化、マテリアルスキーマ、リフレクション駆動のインスペクタ

**Tooling**
- ImGui ドッキングエディタ（Hierarchy / Viewport / Inspector / Asset Browser / Timeline + SSR / Font / Camera / Profiler デバッグウィンドウ）
- ShaderLab サブ秒のシェーダーイテレーションサンドボックス
- Lua 5.4 + sol2 スクリプティング（ゲームプレイ、UI、AI ビヘイビアツリー）、GameModeStack シーンフロー
- Jolt Physics、XAudio2 + X3DAudio

## Build

```bat
cmake --preset vs-x64
cmake --build --preset vs-x64-release
```

完全な手順、オプション、トラブルシューティング → [docs/BUILD.ja.md](docs/BUILD.ja.md)。

## Documentation

| Document                                  | Contents                                          |
| ----------------------------------------- | ------------------------------------------------- |
| [Architecture](docs/ARCHITECTURE.ja.md)   | レイヤリング、フレーム管理、シェーダーパイプライン |
| [Rendering](docs/RENDERING.ja.md)         | レンダリングパイプラインの詳細 + レンダーパスカタログ |
| [Engine Systems](docs/ENGINE.ja.md)       | ECS、リソース、アニメーション、物理、オーディオ、AI、スクリプティング、エディタ、ShaderLab |
| [Build](docs/BUILD.ja.md)                 | CMake プリセット、オプション、パッケージング、トラブルシューティング |
| [Credits & References](docs/CREDITS.ja.md) | サードパーティライブラリ、論文、インスピレーション |
| [Status & Limitations](docs/STATUS.ja.md) | 既知の不足点                                       |
