/**
 * 发射能力测试
 * ============================================================
 * 板子连不上任何 AP，且 AP 侧日志里连它的 MAC 都没有。
 * 怀疑：收得到（beacon），但发不出去（认证帧）。
 *
 * 本固件让板子开一个 SoftAP，然后由路由器主动扫描。
 *   路由器能扫到 C3-DIAG-TEST -> 发射正常，问题在别处
 *   路由器扫不到               -> 发射通路损坏，需换板
 *
 * 信道固定 1，与路由器 2.4G 同信道，便于捕获。
 */
#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println("\n\n============================================");
  Serial.println("  发射能力测试（SoftAP 模式）");
  Serial.println("============================================");
  Serial.printf("芯片      : %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("空闲堆    : %u\n", (unsigned)ESP.getFreeHeap());

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  delay(300);

  bool ok = WiFi.softAP("C3-DIAG-TEST", "", 1, 0, 4);
  delay(500);

  Serial.printf("softAP()  : %s\n", ok ? "成功" : "失败");
  Serial.printf("SSID      : %s\n", WiFi.softAPSSID().c_str());
  Serial.printf("信道      : %d\n", WiFi.channel());
  Serial.printf("AP IP     : %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("AP MAC    : %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("STA MAC   : %s\n", WiFi.macAddress().c_str());
  Serial.println();
  Serial.println("现在等路由器来扫。若路由器能扫到 C3-DIAG-TEST，");
  Serial.println("说明这块板子的发射通路是好的。");
  Serial.println("============================================");
}

void loop() {
  static unsigned long t = 0;
  if (millis() - t > 8000) {
    t = millis();
    Serial.printf("[%lus] 存活 已关联客户端=%d\n",
                  millis() / 1000, WiFi.softAPgetStationNum());
  }
  delay(100);
}
