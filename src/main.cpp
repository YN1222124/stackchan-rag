#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <vector>
#include "face.h"
#include "secrets.h" // WIFI_SSID, WIFI_PASS, SERVER_BASE("http://IP:8001"), API_KEY

// ===== 音声・通信設定 =====
constexpr uint32_t SAMPLE_RATE    = 16000;
constexpr size_t   CHUNK_SAMPLES  = 320;                 // 20ms分
constexpr size_t   MAX_REC_SEC    = 10;                   // 最大録音時間 (10秒)
constexpr size_t   MAX_REC_SAMPLES= SAMPLE_RATE * MAX_REC_SEC;
constexpr uint8_t  SPEAKER_VOLUME = 200;                 // 0〜255

const char* ENDPOINT_PATH = "/stackchan";
const char* AUTH_HEADER   = "X-API-Key";
constexpr uint32_t HTTP_TIMEOUT_MS = 60000;

// ---------- LED 定義 ----------
struct Color_t { uint8_t r = 0, g = 0, b = 0; };
const Color_t COLOR_OFF    = {0, 0, 0};
const Color_t COLOR_ORANGE = {120, 35, 0};   // 待機
const Color_t COLOR_RED    = {150, 0, 0};    // 録音中
const Color_t COLOR_PURPLE = {90, 0, 150};   // DGX推論中
const Color_t COLOR_GREEN  = {0, 150, 0};    // 発話再生中
const Color_t COLOR_YELLOW = {150, 100, 0};  // エラー

void setLedColor(const Color_t& c) {
    for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, c.r, c.g, c.b);
    M5StackChan.refreshRgb();
}

// ---------- 状態管理 ----------
enum class St { IDLE, REC, WAIT, PLAY };
St st = St::IDLE;

Face face;
uint32_t stateStartMs = 0;
uint32_t errUntil = 0;
uint32_t nextIdleAt = 0, lookResetAt = 0;
bool     lookActive = false, wasTouching = false;

// 録音・再生用バッファ（PSRAMに確保）
int16_t* recBuffer = nullptr;
size_t   recordedSamples = 0;

uint8_t* wavRxBuffer = nullptr;
size_t   wavRxSize = 0;

// ---------- 通信ジョブ ----------
struct NetJob {
    int  code = 0;
    volatile bool done = false;
    volatile bool cancel = false;
    String userQuery;
    String answerText;
};
NetJob job;
bool jobActive = false;

// 16bit PCM データを WAV フォーマットヘッダ (44 bytes) でラップして送信
int postAudioWav(const int16_t* pcmData, size_t sampleCount, uint8_t*& outWav, size_t& outSize, String& userQuery, String& answerText, volatile bool* cancel)
{
    String hostPort = SERVER_BASE;
    if (hostPort.startsWith("http://")) hostPort.remove(0, 7);
    int slash = hostPort.indexOf('/');
    if (slash >= 0) hostPort = hostPort.substring(0, slash);

    String host = hostPort;
    uint16_t port = 80;
    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        port = hostPort.substring(colon + 1).toInt();
    }

    WiFiClient client;
    if (!client.connect(host.c_str(), port, 5000)) return -1;

    // WAV ヘッダ (44バイト)
    uint32_t pcmBytes = sampleCount * sizeof(int16_t);
    uint32_t totalDataLen = pcmBytes + 36;
    uint8_t wavHeader[44] = {
        'R', 'I', 'F', 'F',
        (uint8_t)(totalDataLen & 0xff), (uint8_t)((totalDataLen >> 8) & 0xff), (uint8_t)((totalDataLen >> 16) & 0xff), (uint8_t)((totalDataLen >> 24) & 0xff),
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0,             // Subchunk1Size (16 for PCM)
        1, 0,                    // AudioFormat (1: PCM)
        1, 0,                    // NumChannels (1: Mono)
        (uint8_t)(SAMPLE_RATE & 0xff), (uint8_t)((SAMPLE_RATE >> 8) & 0xff), (uint8_t)((SAMPLE_RATE >> 16) & 0xff), (uint8_t)((SAMPLE_RATE >> 24) & 0xff),
        (uint8_t)((SAMPLE_RATE * 2) & 0xff), (uint8_t)(((SAMPLE_RATE * 2) >> 8) & 0xff), (uint8_t)(((SAMPLE_RATE * 2) >> 16) & 0xff), (uint8_t)(((SAMPLE_RATE * 2) >> 24) & 0xff),
        2, 0,                    // BlockAlign (2)
        16, 0,                   // BitsPerSample (16)
        'd', 'a', 't', 'a',
        (uint8_t)(pcmBytes & 0xff), (uint8_t)((pcmBytes >> 8) & 0xff), (uint8_t)((pcmBytes >> 16) & 0xff), (uint8_t)((pcmBytes >> 24) & 0xff)
    };

    String boundary = "----WebKitFormBoundaryStackChanCoreS3";
    String head = "--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
                + "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t fullContentLength = head.length() + 44 + pcmBytes + tail.length();

    client.print(String("POST ") + ENDPOINT_PATH + " HTTP/1.1\r\n");
    client.print(String("Host: ") + hostPort + "\r\n");
    client.print(String(AUTH_HEADER) + ": " + API_KEY + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print(String("Content-Length: ") + fullContentLength + "\r\n");
    client.print("Connection: close\r\n\r\n");

    // バイナリ送信
    client.print(head);
    client.write(wavHeader, 44);
    client.write((const uint8_t*)pcmData, pcmBytes);
    client.print(tail);

    // レスポンス受信
    uint32_t t0 = millis();
    String headerText = "";
    while (client.connected() && millis() - t0 < HTTP_TIMEOUT_MS) {
        if (cancel && *cancel) { client.stop(); return -2; }
        if (client.available()) {
            char c = client.read();
            headerText += c;
            if (headerText.endsWith("\r\n\r\n")) break;
        }
        delay(1);
    }

    if (!headerText.endsWith("\r\n\r\n")) { client.stop(); return -11; }

    int code = 0;
    if (headerText.startsWith("HTTP/1.1 ") || headerText.startsWith("HTTP/1.0 ")) {
        code = headerText.substring(9, 12).toInt();
    }

    // ヘッダーから Content-Length と認識・回答テキストを取得
    int clIdx = headerText.indexOf("content-length:");
    if (clIdx < 0) clIdx = headerText.indexOf("Content-Length:");
    size_t expectedLen = 0;
    if (clIdx >= 0) {
        int endCl = headerText.indexOf("\r\n", clIdx);
        expectedLen = headerText.substring(clIdx + 15, endCl).toInt();
    }

    if (code == 200 && expectedLen > 0) {
        Serial.printf("[NET] Allocating PSRAM for Rx WAV: %d bytes\n", expectedLen);
        if (outWav) {
            free(outWav);
            outWav = nullptr;
        }
        outWav = (uint8_t*)ps_malloc(expectedLen);
        if (!outWav) {
            Serial.println("[NET ERROR] Failed to allocate PSRAM for WAV!");
            client.stop();
            return -101;
        }

        size_t bytesRead = 0;
        t0 = millis();
        uint8_t tempChunk[1024];

        // 1KB単位で一気に読み込んで受信時間を劇的に短縮（画面フリーズ防止）
        while (bytesRead < expectedLen && millis() - t0 < 30000) {
            if (cancel && *cancel) { client.stop(); return -2; }
            
            size_t availableBytes = client.available();
            if (availableBytes > 0) {
                size_t toRead = (availableBytes > sizeof(tempChunk)) ? sizeof(tempChunk) : availableBytes;
                if (bytesRead + toRead > expectedLen) {
                    toRead = expectedLen - bytesRead;
                }
                int r = client.read(tempChunk, toRead);
                if (r > 0) {
                    memcpy(outWav + bytesRead, tempChunk, r);
                    bytesRead += r;
                }
            } else {
                delay(2); // パケット到着待ち & WDTリセット防止
            }
        }
        outSize = bytesRead;
        Serial.printf("[NET] Download complete: %d / %d bytes\n", bytesRead, expectedLen);
    } else {
        outSize = 0;
    }

    client.stop();
    return code;
}

void netTask(void* p)
{
    NetJob* j = (NetJob*)p;
    Serial.printf("[NET] Uploading %d samples to DGX...\n", recordedSamples);
    j->code = postAudioWav(recBuffer, recordedSamples, wavRxBuffer, wavRxSize, j->userQuery, j->answerText, &j->cancel);
    Serial.printf("[NET] Finished. Code=%d, RxBytes=%d\n", j->code, wavRxSize);
    j->done = true;
    vTaskDelete(NULL);
}

// ---------- 状態遷移処理 ----------
void enterIdle()
{
    st = St::IDLE;
    stateStartMs = millis();
    setLedColor(COLOR_ORANGE);
    face.setMouthOpen(-1);
    face.setMood(Mood::Neutral);
    face.setLook(0, 0);
    face.setCaption("画面タッチでお話し");
    M5StackChan.Motion.move(0, 0, 300);
}

void fail(const String& msg)
{
    Serial.println("[ERROR] " + msg);
    enterIdle();
    setLedColor(COLOR_YELLOW);
    face.setCaption(msg);
    face.flashMood(Mood::Surprised, 700);
    errUntil = millis() + 4000;
}

void enterRec()
{
    st = St::REC;
    stateStartMs = millis();
    recordedSamples = 0;
    setLedColor(COLOR_RED);
    face.setMood(Mood::Neutral);
    face.setCaption("お話し中 (残り10秒) [タップで完了]");
    face.setLook(0, 0.3f);
    M5StackChan.Motion.move(0, 150, 300);

    // CoreS3 のマイクを起動
    M5.Speaker.end();
    M5.Mic.begin();
}

void enterWait()
{
    M5.Mic.end(); // 録音終了

    st = St::WAIT;
    stateStartMs = millis();
    setLedColor(COLOR_PURPLE);
    face.setCaption("考え中です...");
    face.setLook(-0.6f, 0.6f);
    M5StackChan.Motion.move(150, 250, 300);

    job = NetJob();
    jobActive = true;
    xTaskCreatePinnedToCore(netTask, "net_voice", 16384, &job, 1, nullptr, 0);
}

void enterPlay()
{
    st = St::PLAY;
    stateStartMs = millis();
    setLedColor(COLOR_GREEN);
    face.setMood(Mood::Happy);
    face.setCaption("回答中...");
    M5StackChan.Motion.move(0, 0, 300);

    // I2Sバスの切り替えマージン
    delay(20);

    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    constexpr uint32_t VOICEVOX_SAMPLE_RATE = 24000;

    if (wavRxSize > 44 && wavRxBuffer != nullptr) {
        Serial.printf("[PLAY] Playing WAV: %d bytes @ %d Hz\n", wavRxSize - 44, VOICEVOX_SAMPLE_RATE);
        M5.Speaker.playRaw(
            (const int16_t*)(wavRxBuffer + 44), 
            (wavRxSize - 44) / sizeof(int16_t), 
            VOICEVOX_SAMPLE_RATE,
            false, 1, 0
        );
    } else {
        Serial.println("[PLAY ERROR] Invalid WAV buffer or size!");
        enterIdle();
    }
}

// ---------- Arduino setup / loop ----------
void setup()
{
    Serial.begin(115200);

    M5StackChan.begin();
    M5StackChan.Motion.setAutoAngleSyncEnabled(false);
    face.begin();
    M5StackChan.Motion.goHome(300);
    setLedColor(COLOR_OFF);

    // PSRAMバッファ確保
    recBuffer = (int16_t*)ps_malloc(MAX_REC_SAMPLES * sizeof(int16_t));
    if (!recBuffer) Serial.println("[ERR] PSRAM malloc failed for recBuffer");

    face.setCaption("Wi-Fi接続中...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        face.update();
        delay(50);
    }
    Serial.printf("Connected IP: %s\n", WiFi.localIP().toString().c_str());

    enterIdle();
}

void loop()
{
    M5StackChan.update();
    uint32_t now = millis();

    int16_t tx, ty;
    bool touching = M5StackChan.Display().getTouch(&tx, &ty);
    bool screenTap = touching && !wasTouching;
    wasTouching = touching;

    if (jobActive && job.done && st != St::WAIT) jobActive = false;

    switch (st) {
    case St::IDLE:
        if (errUntil && now > errUntil) { errUntil = 0; face.setCaption("画面タッチでお話し"); setLedColor(COLOR_ORANGE); }
        if (screenTap) {
            if (WiFi.status() != WL_CONNECTED) fail("Wi-Fi切断中");
            else enterRec();
        }
        break;

    case St::REC: {  // ← 先頭に波括弧を追加
        // 録音処理 (20msごと)
        if (M5.Mic.record(recBuffer + recordedSamples, CHUNK_SAMPLES, SAMPLE_RATE)) {
            while (M5.Mic.isRecording()) delay(1);
            recordedSamples += CHUNK_SAMPLES;
        }

        // 残り秒数の計算と表示更新
        uint32_t elapsedSec = (millis() - stateStartMs) / 1000;
        int remainingSec = (int)MAX_REC_SEC - (int)elapsedSec;
        if (remainingSec < 0) remainingSec = 0;

        static int lastRemaining = -1;
        if (remainingSec != lastRemaining) {
            lastRemaining = remainingSec;
            String msg = "お話し中 (残り" + String(remainingSec) + "秒) [タップで完了]";
            face.setCaption(msg.c_str());
        }

        // 終了条件: 画面再タップ または 最大時間到達
        if (screenTap || (recordedSamples >= MAX_REC_SAMPLES)) {
            lastRemaining = -1;
            enterWait();
        }
        break;
    } 

    case St::WAIT:
        if (screenTap) {
            job.cancel = true;
            enterIdle();
            break;
        }
        if (job.done) {
            jobActive = false;
            if (job.code == 200 && wavRxSize > 44) {
                enterPlay();
            } else if (job.code == 401) {
                fail("認証エラー (401)");
            } else {
                fail("エラー: " + String(job.code));
            }
        }
        break;

    case St::PLAY: {
        if (screenTap) {
            M5.Speaker.stop();
            enterIdle();
            break;
        }

        // 再生が終了したら待機へ戻す
        if (!M5.Speaker.isPlaying()) {
            enterIdle();
            face.flashMood(Mood::Happy, 800);
            break;
        }

        // 再生中の口パクと首振りモーション（リップシンク）
        float m = fabsf(sinf(now / 80.0f)) * 0.8f;
        face.setMouthOpen(m);
        if (now % 300 < 20) {
            M5StackChan.Motion.move(0, (int)(m * 100), 500);
        }
        break;
    }
    }

    face.update();
    delay(5);
}