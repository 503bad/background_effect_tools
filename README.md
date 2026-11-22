# Background Effect Tools

> **📥 最新版のダウンロード：[release.zip（Windows x64）](https://github.com/503bad/background_effect_tools/releases/latest/download/release.zip)**
> ／ [リリースページ（更新内容）](https://github.com/503bad/background_effect_tools/releases/latest)
>
> 展開した `install.bat` をダブルクリックするとインストールできます（詳しくは「[OBS へのインストール](#obs-へのインストール)」）。

OBS Studio 用の背景エフェクト・ソースプラグインです。1つのソース「背景エフェクト
(Background Effects)」を追加し、プロパティでエフェクト種別を切り替えて使用します。


## OBS へのインストール

### インストーラーを使う（推奨）

1. OBS を完全に終了します。
2. ZIP をすべて展開し、`install.bat` を**ダブルクリック**します。管理者権限の確認（UAC）が出たら「はい」を選ぶと、新しいウィンドウでインストールが進みます。

`install.bat` は次のことを自動で行います。

- `C:\ProgramData\obs-studio\plugins\background-effect-tools\` に、DLL を
  **OBS 33 以降の置き方と OBS 32 以前の置き方の両方**で配置します。OBS のバージョンに
  関係なく読み込まれ、32 から 33 へ更新したあともそのまま使えます
  （OBS 33 は新しい置き方を先に読み込み、もう一方は読み飛ばします）。
  - `...\background-effect-tools\background-effect-tools.dll`（OBS 33 以降）
  - `...\background-effect-tools\bin\64bit\background-effect-tools.dll`（OBS 32 以前）
  - `...\background-effect-tools\data\`
- OBS の場所（レジストリ → `C:\Program Files\obs-studio`）は、古いコピーの削除と
  ポータブル版の判定にだけ使います。見つからなくてもインストールは続けます。
  - **ポータブル版 OBS 33 以降**は `install.bat "D:\Apps\obs-studio"` のように
    OBS のフォルダを指定してください（`<OBS のフォルダ>\plugins\background-effect-tools\` に入ります。
    ポータブル版 OBS 32 以前はインストーラーの対象外です）。
- 以前のバージョンが旧方式の場所（`<OBS>\obs-plugins\64bit\` と
  `<OBS>\data\obs-plugins\background-effect-tools\`）にあれば削除します。
  旧方式の場所は OBS 34 以降で読み込まれなくなる予定です。
- 以前のバージョンの不要なファイル（古いエフェクトなど）も削除します。

インストールのたびに、`install.bat` と同じフォルダへ `install-log.txt`（見つけた OBS・
インストール先・PC にあるプラグインの DLL の一覧）を書き出します。うまく動かないときは
このファイルと OBS のログを添えてお知らせください。`install.bat /check` を実行すると、
何も変更せずにインストール先などを画面に表示します（管理者権限は不要です）。

### 手動でインストールする

OBS を終了し、ZIP の中の `background-effect-tools` フォルダを次の場所にフォルダごと
コピーします。フォルダ名は必ず `background-effect-tools`（DLL のベース名と一致）にします。

- `C:\ProgramData\obs-studio\plugins\` の中へそのままコピーし、さらに
  `background-effect-tools.dll` と `background-effect-tools.pdb` を `background-effect-tools\bin\64bit\`
  にもコピーします（両方に置くと OBS 32 / 33 のどちらでも読み込まれます）

以前に `C:\Program Files\obs-studio\obs-plugins\64bit\background-effect-tools.dll` と
`C:\Program Files\obs-studio\data\obs-plugins\background-effect-tools\` に入れていた
場合は、それらを削除してください。

## エフェクト一覧

| エフェクト | 概要 |
|---|---|
| 煙 (Smoke) | 下から立ち上り、渦を巻きながら漂う煙パーティクル |
| 火の粉 (Embers) | 下から舞い上がり明滅する光点。風・光彩系対応 |
| 雲 (Clouds) | ノイズベースの2色雲模様。モーフィング・スクロール・グラデーション乗算 |
| キラキラ (Sparkles) | 画面全体に発生し滞留して明滅する光点（円/十字/星形） |
| 重力パーティクル (Gravity) | 任意角度の重力で加速する汎用パーティクル（雨・雪など） |
| 渦巻き上昇 (Vortex)【ベータ版】 | 中心軸を巻きながら上昇するビジュアライザー。4つの挙動（カールノイズ気流 / 螺旋トルネード / 複数渦の干渉 / フィボナッチ噴水）と高さグラデーション色 |
| オーディオスペクトラム (Spectrum)【ベータ版】 | FFT＋ビート検出による音楽ビジュアライザー。円形(Trap Nation型) / バー(Monstercat型) / 波形リング / ビート連動パーティクルの4タイプ。要・音声リアクティブ有効化 |
| 音声反応パーティクル (Audio particles)【ベータ版】 | パーティクル・ビジュアライザー。スペクトラムバー / デジタルレイン球 / 3Dスペクトログラム / 波形フィールド / タービュランス・コメット / アンビエント・トゥインクルの6プリセット。音声オフ・無音時は内蔵Seedジェネレーター（LFO / ノイズ / オートビート）が自律駆動するため、音声ソースが無くても動き続ける |

## 共通機能

- アルファ付き出力。他ソースの上下に自由に重ねられます
- すべてのパラメータは OBS プロパティから調整でき、即時反映されます
- パーティクル更新はデルタタイム基準（30fps/60fps で同じ見た目・速度）
- ソース解像度（幅・高さ）指定
- 乱数シード固定オプション（録画テイク間の再現性確保）
- 表示/非表示切り替え時のリセット/継続を選択可能
- 風（強さ・向き 0〜360°・乱流）: 煙・火の粉
- ポストエフェクト（グロー / ブルーム / 発光 / レンズフレア）: 対象エフェクトで個別強度調整
- 音声リアクティブ: 指定した音声ソースを FFT 解析（スペクトル / 低中高域 / ビート検出）
  - 「オーディオスペクトラム」エフェクトの土台。さらにパーティクルの「サイズ」「色」「バウンス」へも個別にオン/オフ・量調整で反応（併用可能）
  - 音声ソース・入力ゲイン・アタック/リリース（反応の速さ）を調整可
  - ※スペクトラム表示や音声反応を使うには「音声リアクティブを使う」を有効化し音声ソースを選択
- UI は日本語 / 英語対応

## ビルド (Windows)

Visual Studio 2022 + CMake 3.28 以降が必要です。

```bat
build.bat        REM インクリメンタルビルド + package\ へステージ + OBS へ配置
build.bat clean  REM フルリビルド
```

成果物は `package\` 以下に出力されます:

```
package/
├─ background-effect-tools/
│   ├─ background-effect-tools.dll
│   ├─ background-effect-tools.pdb
│   └─ data/
│       ├─ effects/*.effect
│       └─ locale/*.ini
├─ install.bat
├─ README.md / LICENSE / SOURCE-NOTICE.txt
```

OBS への配置は最後に `package\install.bat` が行います（配布物と同じインストーラー）。
`C:\ProgramData\obs-studio\plugins\` に書き込むため、管理者権限のコマンドプロンプトから
実行してください。

## ライセンス

GPL-2.0（LICENSE を参照）


## クレジット表記のお願い

配信や動画で利用する際は、概要欄などに以下のクレジットをお願いします。

```
■Background Effect Tools
開発：ヘヴィメタルバンド 503 bad gateway
Youtube：https://youtube.com/@503badgateway
X（伊達五十嵐）：https://x.com/503_bad
```