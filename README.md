# Stack-chan LabRAG Voice Guide
M5Stack CoreS3（製品版スタックちゃん）を「研究室案内AIロボット」として動作させるためのファームウェアです。<br>
CoreS3 の内蔵マイクで録音した音声を LAN 内の GPU サーバーへ送信し、サーバー側で高速音声認識（faster-whisper）→ 特化型ドキュメント検索・回答生成（E5 + FAISS + Ollama / Gemma 4）→ 音声合成（VOICEVOX）を一括処理して生成された WAV 音声を受信・再生します。発話に合わせてフルカラー LED の発光、表情変化、首振り、リップシンクを行います。

## 1. システム構成と動作概要
``` txt
[CoreS3 (スタックちゃん)]
  1. 画面タッチで録音開始（LED: 赤 / カウントダウン表示）
  2. 録音終了（画面再タップ または 最大10秒到達）
  3. WAV データを HTTP POST (multipart/form-data) で送信
       │
       ▼ LAN (Wi-Fi) / ポート 8001
[AIサーバー (FastAPI)]
  4. STT: faster-whisper で文字起こし
  5. RAG: 類似度検索 (E5 + FAISS) ＋ プロンプト生成 ＋ LLM (Ollama)
  6. TTS: VOICEVOX Engine で回答音声 (24kHz WAV) を合成
       │
       ▼ レスポンス (audio/wav)
[CoreS3 (スタックちゃん)]
  7. スピーカーから回答音声を再生（LED: 緑 / 口パク＆首振り）
  8. 待機状態へ自動復帰（LED: オレンジ）
```

## 2.ファイル構成
|ファイル|役割|
|:--|:--|
|`main.cpp` (または `.ino`)	|本体側のメイン制御プログラム（録音・通信タスク・再生・サーボ・LED・状態遷移）|
|`face.h`|画面の表情・視線・下部字幕（電光掲示板）の描画制御|
|`secrets.h`|接続先 Wi-Fi 情報、AIサーバーのアドレス、FastAPI 認証キーの定義|
|`platformio.ini`|PlatformIO ビルド環境設定ファイル(ArduinoIDEであれば不要)|
※ Arduino IDE で開発する場合は、プロジェクトフォルダ名をファイル名と同一（例: `StackChanRAG/StackChanRAG.ino`）にリネームして配置してください。

## 3. 開発環境・ライブラリのセットアップ
ライブラリ管理やボードマネージャのバージョン選定が柔軟な *Arduino IDE で依存ライブラリをダウンロード・管理し、PlatformIO からそれを参照してビルド・書き込みを行うハイブリッド方式* を採用しています。<br>
そのため、ArduinoIDEのみでの開発も比較的容易なはずです。（未検証）
### 3-1. Arduino IDE 側での準備
1. *ボードパッケージの導入*
    * Arduino IDE のボードマネージャから `M5Stack` をインストールし、M5CoreS3 を選択します。  
2. *ライブラリマネージャからインストール*
    * `M5Unified` (`M5GFX` も自動で入ります)   
    * `ArduinoJson` (v7 推奨)
    * `IRremoteESP8266` (`StackChan-BSP` が内部で使用)   
    * `M5Unit-NFC` (`StackChan-BSP` が内部で使用)   
3. *StackChan-BSP のインストール（重要）*
    * ライブラリマネージャの公開版（1.1.0）は最新の M5Unified と競合エラーを起こすため、*GitHub の main ブランチからクローンまたは ZIP 導入*してください。   
    * GitHub リポジトリ: https://github.com/m5stack/StackChan-BSP   
    * ダウンロード後、Arduino のライブラリフォルダ（例: `~/Documents/Arduino/libraries/`）に配置します。

## 4. PlatformIO 設定 (`platformio.ini`)
Arduino 環境のライブラリ（`lib_extra_dirs`）を参照させつつ、ESP32 標準の `WiFi` ライブラリと競合する余分なネットワークライブラリ（`WiFiNINA` や `HttpClient` 等）を `lib_ignore` で確実に除外します。
``` ini
[env:m5stack-cores3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = m5stack-cores3
framework = arduino

monitor_speed = 115200
upload_speed = 921600

lib_deps =
    bblanchon/ArduinoJson@^7

; Arduino IDE でインストールしたライブラリ（StackChan-BSP, M5Unified 等）を参照
lib_extra_dirs = ~/Documents/Arduino/libraries

; ESP32 標準の WiFi と競合するライブラリを除外
lib_ignore =
    WiFiNINA
    WiFiNINA-master
    ArduinoHttpClient
    HttpClient
```

## 5. 設定と書き込み
### 5-1. `secrets.h` の設定
プロジェクト内に `secrets.h` を作成し、接続先環境に合わせて書き換えます。
``` cpp
#pragma once

// Wi-Fi 設定 (ESP32 のため 2.4GHz 帯必須)
const char* WIFI_SSID   = "YOUR_LAB_WIFI;
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";

// LLMを動かしているFastAPIのアドレスとポート
const char* SERVER_BASE = "http://XXX.XXX.XX.XX:xxxx";

// 共通の認証APIキー
const char* API_KEY     = "your_api_key";
```
### 5-2. ビルドと書き込み
1. CoreS3(StackChan) を USB ケーブルで PC に接続します。
2. PlatformIO の*Build*を実行し、エラーがないことを確認します。
3. *Upload* を実行してファームウェアを書き込みます。
4. シリアルモニタ（`115200 bps`）を開き、`Connected IP: XXX.XXX.X.X` と表示されれば起動完了です。

## 6. 使い方と状態遷移

| 状態 (State) | LED 色 | 表情 / 画面字幕 | スタックチャンの動作 |
|---|---|---|---|
| **待機 (`IDLE`)** | オレンジ (`COLOR_ORANGE`) | 通常・正面視線<br>「画面タッチでお話し」 | ランダムな視線・まばたき動作 |
| **録音中 (`REC`)** | 赤 (`COLOR_RED`) | 見上げ表情<br>「お話し中 (残りN秒) [タップで完了]」 | **画面タップ** で開始。内蔵マイクで録音（16kHz Mono）[cite: 5, 7]。再度画面タップまたは10秒経過で自動終了 |
| **考え中 (`WAIT`)** | 紫 (`COLOR_PURPLE`) | 首をかしげる<br>「考え中です...」 | バックグラウンド通信タスクで DGX の `/stackchan` へ WAV をアップロードし、回答 WAV を受信[cite: 1, 7] |
| **回答中 (`PLAY`)** | 緑 (`COLOR_GREEN`) | 笑顔 (`Happy`)<br>「回答中...」 | 受信した WAV（24kHz）を直接スピーカー再生しながら、リップシンクと首振り相槌を実行 |
| **エラー (`FAIL`)** | 黄 (`COLOR_YELLOW`) | 驚き顔 (`Surprised`)<br>エラー内容表示 | 認証エラー (401)、Wi-Fi 切断、サーバー応答なしなどを表示して数秒後に復帰[cite: 6, 7] |

---

## 7. トラブルシューティング

| 症状 | 原因と対策 |
|---|---|
| **Wi-Fi に接続できない** | ESP32-S3 は **2.4GHz 帯** にのみ対応しています。5GHz 専用の SSID を指定していないか確認してください。研究室ルーターのクライアント間通信遮断にも注意してください。 |
| **LED が点灯しない・サーボが途中で落ちる** | CoreS3 本体＋サーボ2基＋RGB LED は瞬間的に大電流を消費します。PC の USB ポート給電では不足するため、**5V / 2.0A〜3.0A 以上の給電が可能な AC アダプタ** を接続してください。また、`setup()` 内で `CoreS3.Power.setExtOutput(true);` が呼ばれていることを確認してください。 |
| **エラー 401 が表示される** | `secrets.h` の `API_KEY` が DGX Spark 側の `.env` (`LAB_RAG_API_KEY`) と一致しているか確認してください。 |
| **音声受信時にスタックチャンが落ちる / 固まる** | 回答 WAV の容量が大きく、PSRAM へのブロック展開が詰まっていた可能性があります。最新のコードでは `client.readBytes()` による 1KB 単位のチャンク受信と PSRAM バッファ保護が組み込まれています。 |
| **回答音声のピッチが低く、ゆっくり再生される** | VOICEVOX の生成レート（24000Hz）と CoreS3 の再生レート設定がズレています。`enterPlay()` 内のサンプリングレート指定が `24000` になっているか確認してください。 |
