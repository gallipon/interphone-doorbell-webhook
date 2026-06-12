#include <Arduino.h>
#include <M5Atom.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include <math.h>
#include "config.h"  // SSID / PASSWORD / WEBHOOK_URL（config.h.example をコピーして作成）

// ── 設定 ────────────────────────────────────────────

// GPIO21 = ボトム拡張ピン（外部10kΩプルアップ必須）
#define CALL_PIN 21

// チャタリング防止・再通知抑制（ミリ秒）
const unsigned long DOORBELL_DEBOUNCE_MS = 200;
const unsigned long DOORBELL_COOLDOWN_MS = 10000;  // 10秒間は再発火しない

// WiFi状態LED更新間隔
const unsigned long LED_UPDATE_MS = 5000;

// M5Atom Echo スピーカー I2S ピン（NS4168 アンプ）
#define SPK_BCK_PIN  19
#define SPK_WS_PIN   33
#define SPK_DAT_PIN  22
#define I2S_SAMPLE_RATE 16000
// ────────────────────────────────────────────────────

bool     lastState       = HIGH;
uint32_t lastEdgeTime    = 0;
uint32_t lastTriggerTime = 0;
uint32_t lastLEDUpdate   = 0;
bool     muted           = false;

// ────────────────────────────────────────────────────
void setLED(uint8_t r, uint8_t g, uint8_t b) {
    M5.dis.drawpix(0, CRGB(r, g, b));
    M5.dis.show();
}

// ────────────────────────────────────────────────────
void setupSpeaker() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = I2S_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = 0,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
    };
    i2s_pin_config_t pin_cfg = {
        .bck_io_num   = SPK_BCK_PIN,
        .ws_io_num    = SPK_WS_PIN,
        .data_out_num = SPK_DAT_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_cfg);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

void playBeep(int freqHz, int durationMs) {
    int totalSamples = I2S_SAMPLE_RATE * durationMs / 1000;
    const int CHUNK = 256;
    int16_t buf[CHUNK];
    int written_total = 0;
    size_t written;

    while (written_total < totalSamples) {
        int n = min(CHUNK, totalSamples - written_total);
        for (int i = 0; i < n; i++) {
            buf[i] = (int16_t)(8000 * sin(2.0 * M_PI * freqHz * (written_total + i) / I2S_SAMPLE_RATE));
        }
        i2s_write(I2S_NUM_0, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
        written_total += n;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
}

// ────────────────────────────────────────────────────
// WiFi状態をLEDに反映（ミュート中は呼ばない）
void updateLED() {
    if (WiFi.status() == WL_CONNECTED) {
        setLED(0, 20, 0);   // 緑：正常待機中
        Serial.println("[LED] WiFi OK");
    } else {
        setLED(20, 0, 0);   // 赤：WiFi切断中
        Serial.println("[LED] WiFi NG");
    }
}

// ────────────────────────────────────────────────────
void connectWiFi() {
    setLED(0, 0, 40);  // 青：接続中
    WiFi.begin(SSID, PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500);
        retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        setLED(0, 20, 0);  // 緑：接続済み・待機中
    } else {
        Serial.println("[WiFi] FAILED");
        setLED(20, 0, 0);  // 赤：接続失敗
    }
}

// ────────────────────────────────────────────────────
void sendWebhook(const char* reason) {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        delay(2000);
    }
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(WEBHOOK_URL);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.addHeader("Title", "インターホン");
    http.addHeader("Priority", "urgent");
    http.addHeader("Tags", "bell");

    int code = http.POST("呼出がありました");
    Serial.printf("[Webhook] HTTP %d\n", code);
    http.end();

    // スピーカーフィードバック（ピンポン風ビープ）
    playBeep(880, 150);
    delay(80);
    playBeep(660, 200);

    // LED赤点滅フィードバック
    for (int i = 0; i < 3; i++) {
        setLED(50, 0, 0);
        delay(200);
        setLED(0, 0, 0);
        delay(200);
    }
    updateLED();  // Webhook後のWiFi状態をLEDに反映
}

// ────────────────────────────────────────────────────
void setup() {
    M5.begin(true, false, true);  // Serial有効, I2C無効, LED有効
    Serial.begin(115200);

    setupSpeaker();
    pinMode(CALL_PIN, INPUT);

    connectWiFi();
    Serial.println("Ready. Monitoring TD/TC output...");
}

// ────────────────────────────────────────────────────
void loop() {
    M5.update();

    // ボタン（G39）でミュートトグル
    if (M5.Btn.wasPressed()) {
        muted = !muted;
        if (muted) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            setLED(20, 8, 0);  // オレンジ：ミュート中
            Serial.println("[MUTE] ON");
        } else {
            WiFi.mode(WIFI_STA);
            connectWiFi();
            Serial.println("[MUTE] OFF");
        }
    }

    // 定期的にWiFi状態をLEDに反映
    uint32_t now = millis();
    if (!muted && now - lastLEDUpdate > LED_UPDATE_MS) {
        lastLEDUpdate = now;
        updateLED();
    }

    // 立下りエッジ（HIGH→LOW）= 呼出開始
    bool current = digitalRead(CALL_PIN);
    if (!muted && lastState == HIGH && current == LOW) {
        if (now - lastEdgeTime > DOORBELL_DEBOUNCE_MS) {
            lastEdgeTime = now;
            if (now - lastTriggerTime > DOORBELL_COOLDOWN_MS) {
                lastTriggerTime = now;
                Serial.println("[CALL DETECTED] Sending webhook...");
                sendWebhook("call");
            } else {
                Serial.println("[CALL DETECTED] Cooldown, skipped.");
            }
        }
    }

    lastState = current;
    delay(20);
}
