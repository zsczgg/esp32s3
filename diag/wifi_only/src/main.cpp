/**
 * 纯 WiFi 诊断固件
 * ============================================================
 * 这个草图里没有一行蓝牙代码（不 include BLEDevice.h，不链接 BLE 库）。
 * 目的：判断 "C3 连不上路由器" 是不是由 WiFi/BLE 共存引起的。
 *
 *   连上了   -> 蓝牙共存是元凶，换双芯片桥接有意义
 *   没连上   -> 与蓝牙无关，问题在路由器侧（MAC 过滤 / 密码 / WPA3）
 *
 * 依次尝试三种安全配置，每次都打印底层 reason code。
 */
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ---- 你要改的只有这两行 ----
static const char* SSID = "你的WiFi名称";   // 自己填
static const char* PASS = "你的WiFi密码";   // 自己填
// ----------------------------

static volatile int  g_reason = 0;
static volatile bool g_gotIp  = false;

static void onEvt(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("  [事件] 已关联 AP —— 认证过了，开始握手");
  } else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_gotIp = true;
    Serial.println("  [事件] 拿到 IP");
  } else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_reason = (int)info.wifi_sta_disconnected.reason;
    Serial.printf("  [事件] 断开 reason=%d\n", g_reason);
  }
}

static bool tryConnect(const char* label, wifi_auth_mode_t minSec,
                       bool useSae, unsigned long ms) {
  g_reason = 0;
  g_gotIp  = false;

  WiFi.disconnect(true, false);
  delay(250);

  WiFi.setMinSecurity(minSec);
  WiFi.begin(SSID, PASS);          // begin() 会重置整个 sta 配置
  delay(150);

  if (useSae) {
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
      c.sta.sae_pwe_h2e      = WPA3_SAE_PWE_BOTH;
      c.sta.pmf_cfg.capable  = true;
      c.sta.pmf_cfg.required = false;
      esp_wifi_set_config(WIFI_IF_STA, &c);
    }
    esp_wifi_disconnect();
    delay(120);
    esp_wifi_connect();
  }

  Serial.printf("\n>>> 尝试 %s", label);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  结果: 成功  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("  结果: 失败  最后 reason=%d\n", g_reason);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n\n============================================");
  Serial.println("  纯 WiFi 诊断固件（不含任何蓝牙代码）");
  Serial.println("============================================");
  Serial.printf("芯片     : %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("Flash    : %u MB\n", (unsigned)(ESP.getFlashChipSize() / 1048576));
  Serial.printf("空闲堆   : %u 字节  <- 没有蓝牙占用\n", (unsigned)ESP.getFreeHeap());

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onEvt);

  Serial.printf("WiFi MAC : %s\n", WiFi.macAddress().c_str());
  Serial.printf("目标 AP  : \"%s\"\n", SSID);

  // ---------- 1. 扫描，确认 AP 真实参数 ----------
  Serial.println("\n扫描中...");
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("扫到 %d 个网络\n", n);
  int idx = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == SSID) { idx = i; break; }
  }
  if (idx < 0) {
    Serial.println("!! 没扫到目标 AP —— 后面的连接必然失败");
  } else {
    int enc = (int)WiFi.encryptionType(idx);
    const char* encName =
        enc == WIFI_AUTH_OPEN            ? "开放" :
        enc == WIFI_AUTH_WEP             ? "WEP" :
        enc == WIFI_AUTH_WPA_PSK         ? "WPA-PSK" :
        enc == WIFI_AUTH_WPA2_PSK        ? "WPA2-PSK" :
        enc == WIFI_AUTH_WPA_WPA2_PSK    ? "WPA/WPA2" :
        enc == WIFI_AUTH_WPA3_PSK        ? "WPA3-SAE" :
        enc == WIFI_AUTH_WPA2_WPA3_PSK   ? "WPA2/WPA3 混合" : "其他";
    Serial.printf("  目标: ch=%d  rssi=%d  authmode=%d (%s)  bssid=%s\n",
                  WiFi.channel(idx), WiFi.RSSI(idx), enc, encName,
                  WiFi.BSSIDstr(idx).c_str());
  }
  WiFi.scanDelete();

  // ---------- 2. 三种安全配置依次试 ----------
  bool ok = tryConnect("① WPA2-PSK（最低安全等级 WPA2）",
                       WIFI_AUTH_WPA2_PSK, false, 12000);
  if (!ok) {
    ok = tryConnect("② WPA3-SAE（H2E + hunting-and-pecking，PMF 可选）",
                    WIFI_AUTH_WPA3_PSK, true, 14000);
  }
  if (!ok) {
    ok = tryConnect("③ 开放最低等级（WPA_PSK）",
                    WIFI_AUTH_WPA_PSK, false, 10000);
  }

  Serial.println("\n============================================");
  if (ok) {
    Serial.println("  结论: 【连上了】蓝牙共存是元凶");
    Serial.println("  -> 双芯片分工 / 换 ESP32 都有意义");
  } else {
    Serial.println("  结论: 【没连上】与蓝牙无关，问题在路由器侧");
    Serial.println("  -> 换任何板子都白搭，需查：2.4G 密码是否与 5G 不同、");
    Serial.println("     是否开了 MAC 过滤、是否只允许 WPA3");
  }
  Serial.println("============================================");
}

// 循环重试：配合路由器侧抓 hostapd 日志，判定"帧到底有没有到达 AP"
void loop() {
  static unsigned long t = 0;
  if (millis() - t > 6000) {
    t = millis();
    Serial.printf("\n[%lus] 重试 WPA2 ...\n", millis() / 1000);
    tryConnect("WPA2", WIFI_AUTH_WPA2_PSK, false, 6000);
    Serial.printf("[%lus] 重试 SAE ...\n", millis() / 1000);
    tryConnect("SAE", WIFI_AUTH_WPA3_PSK, true, 6000);
  }
  delay(200);
}
