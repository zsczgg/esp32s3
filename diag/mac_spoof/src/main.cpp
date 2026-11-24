/**
 * MAC 仿冒诊断
 * ============================================================
 * 已知事实：
 *   手机能连上 JCG-Q20-2.4G（密码正确、2.4G 无线服务正常）
 *   本板子连不上，reason=2（AP 不回认证帧），且扫描完全正常
 *
 * 只剩两种可能：
 *   A. 路由器有 MAC 白名单，只放行登记过的设备
 *   B. 本板发射通路异常（收得到 beacon，但认证帧发不出去）
 *
 * 判定方法：把 STA MAC 改成"这台笔记本的 MAC"。
 *   笔记本此刻正挂在同一个路由器上，必然在白名单里（若真的有白名单）。
 *   -> 改完能连上   = A 成立，问题在路由器，把真实 MAC 加白名单即可
 *   -> 改完仍是 2  = B 成立，板子发射坏，换板
 *
 * 注意：同 MAC 双设备会让路由器短暂混乱，笔记本 WiFi 可能闪一下。
 */
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

static const char* SSID = "你的WiFi名称";   // 自己填
static const char* PASS = "你的WiFi密码";   // 自己填

// 这台笔记本的 WLAN MAC（Intel BE200），当前正连着同一个路由器
static uint8_t SPOOF[6] = { 0xBC, 0xCD, 0x99, 0x9E, 0xA4, 0xE9 };

static volatile int g_reason = 0;

static void onEvt(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("  [事件] 已关联 AP —— 认证过了！");
  } else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("  [事件] 拿到 IP");
  } else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_reason = (int)info.wifi_sta_disconnected.reason;
    Serial.printf("  [事件] 断开 reason=%d\n", g_reason);
  }
}

static bool connectTest(unsigned long ms) {
  g_reason = 0;
  WiFi.disconnect(true, false);
  delay(200);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.begin(SSID, PASS);
  Serial.print("  连接中");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  >>> 成功  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("  >>> 失败  最后 reason=%d\n", g_reason);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n============================================");
  Serial.println("  MAC 仿冒诊断");
  Serial.println("============================================");

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onEvt);

  Serial.printf("原始 STA MAC : %s\n", WiFi.macAddress().c_str());
  Serial.printf("目标 STA MAC : %02X:%02X:%02X:%02X:%02X:%02X\n",
                SPOOF[0], SPOOF[1], SPOOF[2], SPOOF[3], SPOOF[4], SPOOF[5]);

  // 必须在 WiFi 启动、但未连接时改 MAC
  esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, SPOOF);
  Serial.printf("改 MAC 结果  : %s\n", e == ESP_OK ? "成功" : "失败");
  delay(200);
  Serial.printf("当前生效 MAC : %s\n", WiFi.macAddress().c_str());

  Serial.println("\n>>> 用仿冒 MAC 连接 2.4G");
  bool ok = connectTest(15000);

  Serial.println("\n============================================");
  if (ok) {
    Serial.println("  结论: 【连上了】=> 路由器有 MAC 白名单");
    Serial.println("  处理: 把真实 MAC 30:ED:A0:5A:52:30 加进白名单，");
    Serial.println("        或关掉路由器的接入控制。板子没问题。");
  } else {
    Serial.println("  结论: 【仍然失败】=> 白名单不成立");
    Serial.println("  说明: 伪装成已被放行的 MAC 依旧被拒，");
    Serial.println("        指向本板发射通路异常（能收不能发），需换板。");
  }
  Serial.println("============================================");
}

void loop() {
  delay(1000);
}
