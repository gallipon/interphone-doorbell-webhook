#include <Arduino.h>
#include <M5StickC.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"  // SSID / PASSWORD / WEBHOOK_URL（config.h.example をコピーして作成）

// ── 設定 ────────────────────────────────────────────

// GPIO36 = 入力専用ピン（内部プルアップなし → 外部10kΩプルアップ必須）
#define CALL_PIN 36

// チャタリング防止・再通知抑制（ミリ秒）
const unsigned long DOORBELL_DEBOUNCE_MS = 200;
const unsigned long DOORBELL_COOLDOWN_MS = 10000;  // 10秒間は再発火しない

// 画面更新間隔
const unsigned long DISPLAY_UPDATE_MS = 5000;
// ────────────────────────────────────────────────────

bool     lastState       = HIGH;
uint32_t lastEdgeTime    = 0;
uint32_t lastTriggerTime = 0;
uint32_t lastDisplayTime = 0;
bool     muted           = false;
bool     fullChargeMode  = false;
bool     longPressFired  = false;

// ────────────────────────────────────────────────────
uint8_t axpReadReg(uint8_t reg) {
    Wire1.beginTransmission(0x34);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)0x34, (uint8_t)1);
    return Wire1.read();
}

void axpWriteReg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(0x34);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

// AXP192 reg 0x33 bits[5:4]: 00=4.10V(~80%), 10=4.20V(100%)
void setChargeVoltage(bool full) {
    uint8_t val = axpReadReg(0x33);
    val = full ? (val & 0xCF) | 0x20 : (val & 0xCF);
    axpWriteReg(0x33, val);
}

// ────────────────────────────────────────────────────
void connectWiFi() {
    M5.Lcd.println("WiFi connecting...");
    WiFi.begin(SSID, PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500);
        M5.Lcd.print(".");
        retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        M5.Lcd.println("\nConnected!");
        M5.Lcd.println(WiFi.localIP());
    } else {
        M5.Lcd.println("\nWiFi FAILED");
    }
}

// ────────────────────────────────────────────────────
void updateDisplay() {
    float bat = M5.Axp.GetBatVoltage();
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Watching...");
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.printf("Bat: %.2fV", bat);
    M5.Lcd.setCursor(10, 50);
    M5.Lcd.printf("WiFi: %s", WiFi.status() == WL_CONNECTED ? "OK" : "NG");
    M5.Lcd.setCursor(10, 70);
    M5.Lcd.setTextColor(fullChargeMode ? TFT_YELLOW : TFT_DARKGREY);
    M5.Lcd.printf("Chg: %s", fullChargeMode ? "FULL" : "80%");
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

    // 画面にフィードバック
    M5.Axp.SetLDO2(true);  // 画面オン
    M5.Lcd.fillScreen(TFT_RED);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.printf("CALL! %d", code);
    delay(2000);
    if (muted) {
        M5.Axp.SetLDO2(false);  // ミュート中は再度オフ
    } else {
        updateDisplay();
    }
}

// ────────────────────────────────────────────────────
void setup() {
    M5.begin();
    Serial.begin(115200);

    M5.Lcd.setRotation(3);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 0);

    pinMode(CALL_PIN, INPUT);

    connectWiFi();
    setChargeVoltage(false);  // 起動時は80%モード

    updateDisplay();
    Serial.println("Ready. Monitoring TD/TC output...");
}

// ────────────────────────────────────────────────────
void loop() {
    M5.update();

    // 長押し（2秒）: フルチャージモードトグル
    if (M5.BtnA.pressedFor(2000) && !longPressFired) {
        longPressFired = true;
        fullChargeMode = !fullChargeMode;
        setChargeVoltage(fullChargeMode);
        M5.Axp.SetLDO2(true);
        M5.Lcd.fillScreen(fullChargeMode ? TFT_YELLOW : TFT_BLUE);
        M5.Lcd.setTextColor(TFT_BLACK);
        M5.Lcd.setCursor(10, 25);
        M5.Lcd.println(fullChargeMode ? "FULL CHARGE" : "80% MODE");
        delay(1500);
        if (muted) M5.Axp.SetLDO2(false);
        else updateDisplay();
        Serial.printf("[CHARGE] %s\n", fullChargeMode ? "FULL(4.2V)" : "80%(4.1V)");
    }

    // 短押し（リリース時）: ミュートトグル
    if (M5.BtnA.wasReleased()) {
        if (!longPressFired) {
            muted = !muted;
            if (muted) {
                M5.Axp.SetLDO2(false);
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                M5.Axp.SetCharge(false);
                Serial.println("[MUTE] ON");
            } else {
                M5.Axp.SetCharge(true);
                M5.Axp.SetLDO2(true);
                WiFi.mode(WIFI_STA);
                connectWiFi();
                updateDisplay();
                Serial.println("[MUTE] OFF");
            }
        }
        longPressFired = false;
    }

    // 定期的に画面更新（バッテリー電圧など）
    uint32_t now = millis();
    if (!muted && now - lastDisplayTime > DISPLAY_UPDATE_MS) {
        lastDisplayTime = now;
        updateDisplay();
    }

    // 立下りエッジ（HIGH→LOW）= 呼出開始（ミュート中はスキップ）
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
