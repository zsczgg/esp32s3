/**
 * ============================================================
 *  云打印盒 · ESP32-C3 固件  v0.2
 * ============================================================
 *  角色：冒充快递小程序（顺丰/丰声/快递管家等）认可的 BLE 打印机，
 *        把收到的打印数据分段转发给电脑上的打印代理。
 *
 *  v0.2 相比 v0.1 的变化：
 *    - 设备名可串口切换（不用重烧就能试不同型号）
 *    - 大位图分段转发（面单位图可能上百 KB，C3 存不下整票）
 *    - BLE 断开时强制收尾，避免最后一段丢在缓冲里
 *
 *  串口命令（115200 8N1）：
 *    status          查看状态
 *    names           列出预置机型名
 *    name <名字>     改广播名（存 NVS，自动重启）
 *    fwd on|off      开/关转发（off = 纯抓包）
 *    dump            立刻打印当前缓冲 hex
 *    clear           清空缓冲
 *    reboot          重启
 * ============================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>   // esp_ble_gap_set_device_name：运行时改广播名，不重启
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <esp_wifi.h>     // set_config 改 SAE PWE / PMF 用

#include "config.h"
#include "app_state.h"

#if defined(HAS_DISPLAY) && HAS_DISPLAY
  #include "board_profile.h"
  #include "display_port.h"
  #include "ui.h"
  #include <lvgl.h>
  #define UI_ON 1
#else
  #define UI_ON 0
#endif

#define FW_VERSION "1.1.0"

// 分段大小：一段 4KB，一票 124KB 的面单约 31 段
#define CHUNK_SIZE 4096

// ---------------- 缓冲 ----------------
static uint8_t  s_buf[CHUNK_SIZE];
static uint8_t  s_send[CHUNK_SIZE];
static size_t   s_len = 0;
static unsigned long s_firstMs = 0;
static unsigned long s_lastMs  = 0;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- 任务状态 ----------------
static volatile bool s_jobActive = false;
static volatile bool s_forceFlush = false;
static uint32_t      s_jobId = 0;
static uint16_t      s_chunkSeq = 0;
static uint32_t      s_rxTotal = 0;   // 本票累计收到的字节数（跨分段累加）

// ---------------- 运行状态 ----------------
static volatile bool s_bleConn = false;
static bool          s_forward = true;
static uint32_t      s_okJobs = 0, s_failJobs = 0;
static uint32_t      s_okChunks = 0, s_failChunks = 0;
static unsigned long s_ledUntil = 0;
static unsigned long s_lastWifiTry = 0;
static bool          s_scanning = false;   // 扫描期间暂停自动重连，否则 scan_start 会失败(-2)
static bool          s_advOn = false;      // BLE 广播当前是否开着
static String        g_ssid, g_pass;       // 实际使用的 WiFi 账号（可被串口改写）
static uint8_t       g_apBssid[6] = {0};    // 扫描到的目标 AP 地址（定向连接用）
static int           g_apCh = 0;
static bool          g_apFound = false;

static void loadWifiCred();
static void saveWifiCred(const String& s, const String& w);
static bool          s_wifiTrying = false; // 是否正在一轮重连窗口内
static unsigned long s_retryStart = 0;

static BLECharacteristic* s_txCh = nullptr;
static String g_devName;

// ---- 通知通道：小程序订阅了哪个就向哪个发状态 ----
static BLECharacteristic* s_ntfChs[8];
static size_t             s_ntfCount = 0;
static volatile bool      s_sendHello = false;   // 连上后主动报一次"就绪"

static void ntfSend(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < s_ntfCount; i++) {
    s_ntfChs[i]->setValue((uint8_t*)d, n);
    s_ntfChs[i]->notify();
  }
}

// 顺丰系小程序常见兼容机型
static const char* PRESET_NAMES[] = {
  "HM-A300",        // 汉印
  "HM-A300-F616",   // 汉印（橙版）
  "HM-303Q",        // 汉印
  "XT-423",         // 芝柯
  "CC3",            // 芝柯（顺丰采购版）
  "BTP-P33",        // 新北洋
  "BTP-P39",        // 新北洋
  "RPP322N",        // 容大
  "Printer_58HB",   // 通用小票机名
};
static const int PRESET_COUNT = sizeof(PRESET_NAMES) / sizeof(PRESET_NAMES[0]);

// 运行时替换 BLE 广播名（定义在后面，这里先声明，串口命令也要用）
static bool ble_apply_name(const String& name);

// ---------------- LED ----------------
static inline void ledSet(bool on) {
#if LED_PIN >= 0
  bool lv = LED_ACTIVE_LOW ? !on : on;
  digitalWrite(LED_PIN, lv ? HIGH : LOW);
#endif
}
static inline void ledPulse(uint16_t ms) {
#if LED_PIN >= 0
  ledSet(true);
  s_ledUntil = millis() + ms;
#endif
}

// ---------------- hex 输出 ----------------
static void hexDump(const uint8_t* d, size_t n, uint32_t job, uint16_t seq) {
#if DUMP_HEX
  Serial.printf("  --- job %lu chunk %u (%u B) ---\n", (unsigned long)job, (unsigned)seq, (unsigned)n);
  for (size_t i = 0; i < n; i += 32) {
    Serial.printf("  %04X  ", (unsigned)i);
    size_t end = (i + 32 < n) ? i + 32 : n;
    for (size_t j = i; j < end; j++) {
      Serial.printf("%02X ", d[j]);
      if (j == i + 15) Serial.print(' ');
    }
    Serial.print("  |");
    for (size_t j = i; j < end; j++) {
      char c = (d[j] >= 0x20 && d[j] < 0x7f) ? (char)d[j] : '.';
      Serial.print(c);
    }
    Serial.println('|');
  }
#else
  (void)d; (void)n; (void)job; (void)seq;
#endif
}

// ---------------- 转发一段 ----------------
static bool postChunk(const uint8_t* d, size_t n, uint32_t job, uint16_t seq, bool final) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[160];
  snprintf(url, sizeof(url), "http://%s:%u%s", AGENT_HOST, (unsigned)AGENT_PORT, AGENT_PATH);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(10000);
  if (!http.begin(url)) { Serial.println("[NET] begin 失败"); return false; }

  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Job-Id", String((unsigned long)job));
  http.addHeader("X-Chunk-Seq", String((unsigned)seq));
  http.addHeader("X-Final", final ? "1" : "0");
  http.addHeader("X-Device", g_devName);

  int code = http.POST(const_cast<uint8_t*>(d), n);
  if (code > 0) {
    Serial.printf("[NET] job %lu seg %u %s -> HTTP %d\n",
                  (unsigned long)job, (unsigned)seq, final ? "(final)" : "", code);
  } else {
    Serial.printf("[NET] job %lu seg %u 失败: %s\n",
                  (unsigned long)job, (unsigned)seq, http.errorToString(code).c_str());
  }
  http.end();
  return (code >= 200 && code < 300);
}

// ---------------- 冲出一段 ----------------
static void flushChunk(bool final) {
  size_t n;
  uint32_t job;
  uint16_t seq;

  portENTER_CRITICAL(&s_mux);
  n    = s_len;
  job  = s_jobId;
  seq  = s_chunkSeq;
  if (n) memcpy(s_send, s_buf, n);
  s_len = 0;
  portEXIT_CRITICAL(&s_mux);

  if (n == 0 && !final) {
    portENTER_CRITICAL(&s_mux);
    if (s_jobActive) s_jobActive = false;
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  Serial.printf("\n[RX] job %lu seg %u  %u 字节 %s\n",
                (unsigned long)job, (unsigned)seq, (unsigned)n, final ? "[最后一节]" : "");
  hexDump(s_send, n, job, seq);

  bool ok = true;
  if (s_forward) {
    state_jobPhase(PH_TX);
    ok = postChunk(s_send, n, job, seq, final);
    ok ? s_okChunks++ : s_failChunks++;
  }

  portENTER_CRITICAL(&s_mux);
  s_chunkSeq++;
  if (final) s_jobActive = false;
  portEXIT_CRITICAL(&s_mux);

  if (final) {
    if (ok) s_okJobs++; else s_failJobs++;
    Serial.printf("[RX] job %lu 结束，共 %u 段\n", (unsigned long)job, (unsigned)seq + 1);
    if (s_rxTotal > 0) {
      state_jobEnd(ok ? JR_OK : JR_FAIL, s_rxTotal, nullptr);
    }
    if (s_txCh && s_bleConn) {
      char msg[40];
      int len = snprintf(msg, sizeof(msg), ok ? "OK %lu\n" : "ERR %lu\n", (unsigned long)job);
      s_txCh->setValue((uint8_t*)msg, len);
      s_txCh->notify();
    }
  }
}

// ---------------- 收到 BLE 数据 ----------------
static void onBleData(const uint8_t* d, size_t n) {
  if (!n) return;
  bool overflow = false;
  bool newJob   = false;

  portENTER_CRITICAL(&s_mux);
  if (!s_jobActive) {
    s_jobActive = true;
    s_jobId++;
    s_chunkSeq = 0;
    s_firstMs = millis();
    newJob = true;
  }
  size_t room = CHUNK_SIZE - s_len;
  size_t take = (n < room) ? n : room;
  if (take) memcpy(s_buf + s_len, d, take);
  s_len += take;
  s_lastMs = millis();
  if (take < n) overflow = true;
  portEXIT_CRITICAL(&s_mux);

  // 界面：新的一票就清零重来，否则累加
  if (newJob) { s_rxTotal = 0; state_jobBegin(s_jobId); }
  s_rxTotal += n;
  state_jobProgress(s_rxTotal);

  if (overflow) Serial.println("[WARN] 本段已满，余下字节丢弃（下一段补齐）");
  ledPulse(20);
}

// ---------------- 状态查询应答 ----------------
// 小程序连上后不急着发打印数据，先发一条状态查询（1~3 字节），
// 等打印机通过 notify 回一个状态字节。回不上就直接报"打印机暂不可用"。
// 这些查询本身**不能转发给电脑**，否则会被当成打印内容打出一串乱码。
static bool autoReplyStatus(const uint8_t* d, size_t n, const char* src) {
  if (n == 0 || n > 8) return false;   // 长包一定是打印数据，不拦

  if (n >= 3 && d[0] == 0x10 && d[1] == 0x04) {
    // DLE EOT n —— ESC/POS 实时状态查询
    //   n=1 打印机状态  n=2 离线状态  n=3 错误状态  n=4 纸张状态
    // 0x16 = 在线 / 无错误 / 有纸（市面小票机最通用的"一切正常"值）
    uint8_t st = 0x16;
    Serial.printf("[TX] 回应 DLE EOT %u -> %02X  (%s)\n", d[2], st, src);
    ntfSend(&st, 1);
    return true;
  }
  if (n >= 3 && d[0] == 0x1D && d[1] == 0x72) {
    // GS r n —— 状态传输
    uint8_t st = 0x16;
    Serial.printf("[TX] 回应 GS r %u -> %02X  (%s)\n", d[2], st, src);
    ntfSend(&st, 1);
    return true;
  }
  if (n >= 2 && d[0] == 0x10 && d[1] == 0x04) {
    uint8_t st = 0x16;
    Serial.printf("[TX] 回应 DLE EOT(短包) -> %02X  (%s)\n", st, src);
    ntfSend(&st, 1);
    return true;
  }
  return false;
}

class RxCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    auto v = c->getValue();
    if (v.empty()) return;

    // 收到就立刻打印，不等 300ms idle —— 状态查询只有 2~3 字节，
    // 等 idle 才 dump 会让人误判成"小程序根本没发数据"
    String u(c->getUUID().toString().c_str());
    Serial.printf("\n[RX] %u 字节 <- %s : ", (unsigned)v.size(), u.c_str());
    size_t show = v.size() < 48 ? v.size() : 48;
    for (size_t i = 0; i < show; i++) Serial.printf("%02X ", (uint8_t)v.data()[i]);
    if (v.size() > show) Serial.printf("...(+%u)", (unsigned)(v.size() - show));
    Serial.println();

    if (autoReplyStatus((const uint8_t*)v.data(), v.size(), u.c_str())) return;
    onBleData((const uint8_t*)v.data(), v.size());
  }

  void onStatus(BLECharacteristic* c, Status s, uint32_t code) override {
    // 客户端写 CCCD(订阅/退订) 也会走到这里 —— 能直接看出小程序订阅了哪个特征
    Serial.printf("[BLE] 特征 %s 状态=%d code=%u\n",
                  c->getUUID().toString().c_str(), (int)s, (unsigned)code);
  }
};

class SrvCallback : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    (void)s;
    s_bleConn = true;
    s_sendHello = true;      // 连上后主动报一次"就绪"，模拟真机上电上报
    state_setBle(true);
    Serial.println("\n[BLE] 手机已连接");
    ledSet(true);
  }
  void onDisconnect(BLEServer* s) override {
    (void)s;
    s_bleConn = false;
    state_setBle(false);
    Serial.println("\n[BLE] 连接断开，重新广播");
    // 不要在 BLE 回调里做 HTTP，交给 loop
    if (s_jobActive) s_forceFlush = true;
    ledSet(false);
    BLEDevice::startAdvertising();
  }
};

// ---------------- 建服务 ----------------
// 按真机 GATT 表建：每个服务 = 「写特征」+「通知特征」两个。
// 之前只建了写特征、没有可订阅的通知通道，小程序订阅不到就判"打印机不可用"，
// 连一个字节都不往下发 —— 这是"能连上但提示暂不可用"的真正原因。
static void registerNtf(BLECharacteristic* c) {
  if (s_ntfCount < 8) s_ntfChs[s_ntfCount++] = c;
}

static void addPrinterService(BLEServer* srv, const char* svcUuid,
                              const char* wrUuid, const char* ntfUuid,
                              const char* label) {
  BLEService* svc = srv->createService(svcUuid);
  if (!svc) { Serial.printf("[BLE] %s 创建失败（内存不足）\n", label); return; }

  const bool same = (ntfUuid && strcmp(wrUuid, ntfUuid) == 0);

  uint32_t wrProps = BLECharacteristic::PROPERTY_WRITE |
                     BLECharacteristic::PROPERTY_WRITE_NR;
  bool wrIsNtf = (!ntfUuid || same);          // 写特征自己兼任通知（HM-10 风格）
  if (wrIsNtf) wrProps |= BLECharacteristic::PROPERTY_NOTIFY |
                           BLECharacteristic::PROPERTY_READ;

  BLECharacteristic* wr = svc->createCharacteristic(wrUuid, wrProps);
  wr->setCallbacks(new RxCallback());
  if (wrIsNtf) {
    wr->addDescriptor(new BLE2902());
    registerNtf(wr);
  }

  if (ntfUuid && !same) {
    BLECharacteristic* nt = svc->createCharacteristic(
        ntfUuid, BLECharacteristic::PROPERTY_NOTIFY |
                 BLECharacteristic::PROPERTY_READ |
                 BLECharacteristic::PROPERTY_WRITE |
                 BLECharacteristic::PROPERTY_WRITE_NR);
    nt->setCallbacks(new RxCallback());   // 少数实现把数据写进"通知"特征
    nt->addDescriptor(new BLE2902());
    registerNtf(nt);
  }

  svc->start();
  Serial.printf("[BLE] 服务 %-6s wr=%s  ntf=%s\n",
                label, wrUuid, (ntfUuid && !same) ? ntfUuid : "同wr");
}

// ---------------- 串口命令 ----------------
static void printNames() {
  Serial.println("预置机型名（顺丰系小程序常见兼容机型）：");
  for (int i = 0; i < PRESET_COUNT; i++) {
    Serial.printf("  %2d) %s\n", i + 1, PRESET_NAMES[i]);
  }
  Serial.printf("当前: %s\n", g_devName.c_str());
  Serial.println("用法: name <名字>   例: name HM-A300");
}

// ============================================================
// 板上彩色灯（WS2812 类）自测
//   官方文档写 RGB 灯在 GPIO42，但没实测过；不同批次可能改到 48/38/47。
//   这条命令就是用来把它验成事实的：哪个脚出颜色，就记到档案里。
//   neopixelWrite 是 Arduino 3.x 自带（走 RMT，不占 CPU、不影响 WiFi）。
//   注意 GPIO26~37 被 Flash/PSRAM 占用，代码里直接拒绝，绝不碰。
// ============================================================
static const int LED_CANDIDATES[] = {42, 48, 38, 47, 21, 14};
static const int LED_CAND_N = sizeof(LED_CANDIDATES) / sizeof(LED_CANDIDATES[0]);

static bool led_pin_ok(int pin) {
  if (pin < 0) return false;
  if (pin >= 26 && pin <= 37) return false;    // Flash / Octal PSRAM
  if (pin >= 43 && pin <= 46) return false;    // 本板：43/44 未引出，45 背光，46 DC
  if (pin == 19 || pin == 20) return false;    // USB D- / D+
  return true;
}

static void led_set(int pin, int r, int g, int b) {
  if (!led_pin_ok(pin)) {
    Serial.printf("GPIO%d 不可用（Flash/PSRAM/USB 占用，或本板已用于 LCD）\n", pin);
    return;
  }
  neopixelWrite((uint8_t)pin, (uint8_t)r, (uint8_t)g, (uint8_t)b);
  Serial.printf("GPIO%d -> RGB(%d,%d,%d)\n", pin, r, g, b);
}

static void led_sweep(int ms) {
  Serial.println("[LED] 逐个试候选脚，每脚先红后绿再蓝。看到板上有灯变色，记下当前脚号");
  for (int i = 0; i < LED_CAND_N; i++) {
    int p = LED_CANDIDATES[i];
    Serial.printf("[LED] 现在试 GPIO%d\n", p);
    const uint8_t seq[3][3] = {{40, 0, 0}, {0, 40, 0}, {0, 0, 40}};
    for (int k = 0; k < 3; k++) {
      neopixelWrite((uint8_t)p, seq[k][0], seq[k][1], seq[k][2]);
      delay(ms);
    }
    neopixelWrite((uint8_t)p, 0, 0, 0);
    delay(150);
  }
  Serial.println("[LED] 扫完。全程没亮 → 这板没有 WS2812，或灯挂在别的脚");
}

static void handleSerial() {
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (!cmd.length()) return;

  if (cmd == "status") {
    Serial.printf("固件     : v%s\n", FW_VERSION);
    Serial.printf("设备名   : %s\n", g_devName.c_str());
    Serial.printf("WiFi     : %s  IP=%s  RSSI=%d  (SSID=%s)\n",
                  WiFi.status() == WL_CONNECTED ? "已连接" : "未连接",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), g_ssid.c_str());
    Serial.printf("BLE      : %s\n", s_bleConn ? "已连接" : "广播中");
    Serial.printf("代理     : http://%s:%u%s\n", AGENT_HOST, (unsigned)AGENT_PORT, AGENT_PATH);
    Serial.printf("转发     : %s\n", s_forward ? "开" : "关");
    Serial.printf("任务     : 成功 %lu / 失败 %lu   分段 %lu / %lu\n",
                  (unsigned long)s_okJobs, (unsigned long)s_failJobs,
                  (unsigned long)s_okChunks, (unsigned long)s_failChunks);
    Serial.printf("缓冲     : %u / %u\n", (unsigned)s_len, (unsigned)CHUNK_SIZE);
  } else if (cmd.startsWith("wifi ")) {
    // wifi <SSID> <密码>  —— 存进 NVS，重启后生效，不用重烧固件
    int sp = cmd.indexOf(' ', 5);
    if (sp < 0) { Serial.println("用法: wifi <SSID> <密码>"); return; }
    String s = cmd.substring(5, sp);  s.trim();
    String w = cmd.substring(sp + 1); w.trim();
    if (!s.length()) { Serial.println("SSID 不能为空"); return; }
    saveWifiCred(s, w);
    Serial.printf("已保存 WiFi: %s / %s\n重启后生效...\n", s.c_str(), w.c_str());
    delay(400);
    ESP.restart();
  } else if (cmd == "names") {
    printNames();
  } else if (cmd.startsWith("name ")) {
    String v = cmd.substring(5);
    v.trim();
    if (!v.length()) { Serial.println("名字不能为空"); return; }
    Preferences p;
    p.begin("cpb", false);
    p.putString("name", v);
    p.end();
    g_devName = v;
    state_setDevName(g_devName.c_str());
    Serial.printf("设备名已改为 \"%s\"（已存 NVS，立即生效，不重启）\n", v.c_str());
    ble_apply_name(g_devName);
  } else if (cmd == "fwd on")  { s_forward = true;  Serial.println("转发已开"); }
  else if (cmd == "fwd off")   { s_forward = false; Serial.println("转发已关（纯抓包）"); }
  else if (cmd == "dump") {
    portENTER_CRITICAL(&s_mux);
    size_t n = s_len;
    if (n) memcpy(s_send, s_buf, n);
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("当前缓冲 %u 字节\n", (unsigned)n);
    if (n) hexDump(s_send, n, s_jobId, s_chunkSeq);
  } else if (cmd == "clear") {
    portENTER_CRITICAL(&s_mux);
    s_len = 0; s_jobActive = false;
    portEXIT_CRITICAL(&s_mux);
    Serial.println("缓冲已清空");
#if UI_ON
  } else if (cmd.startsWith("blsweep")) {
    int a = 0, b = 0;
    if (cmd.length() > 8) sscanf(cmd.c_str() + 8, "%d %d", &a, &b);
    display_bl_sweep(a, b);
  } else if (cmd.startsWith("bl ")) {
    int sp = cmd.indexOf(' ', 3);
    if (sp < 0) { Serial.println("用法: bl <脚号> on|off"); return; }
    int p = cmd.substring(3, sp).toInt();
    bool on = cmd.substring(sp + 1) != "off";
    display_bl_pin(p, on);
    Serial.printf("GPIO%d -> %s\n", p, on ? "高" : "低");
  } else if (cmd.startsWith("fill ")) {
    int r = 0, g = 0, b = 0;
    sscanf(cmd.c_str() + 5, "%d %d %d", &r, &g, &b);
    display_fill(r, g, b);
  } else if (cmd == "mem") {
    ui_mem_report();
  } else if (cmd == "ledsweep") {
    led_sweep(350);
  } else if (cmd.startsWith("led ")) {
    // led <pin> <r> <g> <b>   例: led 42 40 0 0
    // led off                 关掉所有候选脚
    if (cmd.substring(4) == "off") {
      for (int i = 0; i < LED_CAND_N; i++) neopixelWrite((uint8_t)LED_CANDIDATES[i], 0, 0, 0);
      Serial.println("[LED] 全部熄灭");
    } else {
      int p = -1, r = 0, g = 0, b = 0;
      if (sscanf(cmd.c_str() + 4, "%d %d %d %d", &p, &r, &g, &b) >= 1) led_set(p, r, g, b);
      else Serial.println("用法: led <脚> <r> <g> <b>   /   led off   /   ledsweep");
    }
  } else if (cmd == "colortest") {
    ui_show_color_page();
  } else if (cmd == "colors") {
    display_color_cycle();
  } else if (cmd == "dcprobe") {
    lcd_raw_probe();
  } else if (cmd.startsWith("rawp")) {
    // rawp <dc> <r> <g> <b> [边框] [cs] [rst]
    int dc = 10, r = 255, g = 255, b = 255, f = 0, cs = -1, rst = -1;
    if (cmd.length() > 4) sscanf(cmd.c_str() + 4, "%d %d %d %d %d %d %d",
                                 &dc, &r, &g, &b, &f, &cs, &rst);
    lcd_raw_paint(dc, cs, (uint8_t)r, (uint8_t)g, (uint8_t)b, f != 0, rst);
  } else if (cmd == "uistop") {
    display_pause(true);
  } else if (cmd == "uiresume") {
    display_pause(false);
  } else if (cmd.startsWith("bltest")) {
    int p = -1;
    if (cmd.length() > 7) p = cmd.substring(7).toInt();
    lcd_bl_test(p);
  } else if (cmd.startsWith("i2cscan")) {
    int sda = PIN_TP_SDA, scl = PIN_TP_SCL;
    if (cmd.length() > 9) sscanf(cmd.c_str() + 9, "%d %d", &sda, &scl);
    i2c_scan(sda, scl);
  } else if (cmd.startsWith("i2cdump")) {
    int a = 0x18, st = 0;
    if (cmd.length() > 8) {
      char* endp = nullptr;
      a = (int)strtol(cmd.c_str() + 8, &endp, 0);
      if (endp && *endp) st = (int)strtol(endp, nullptr, 0);
    }
    i2c_dump(a, st);
  } else if (cmd.startsWith("i2cset")) {
    int a = 0x18, r = 0, v = 0;
    if (cmd.length() > 7) sscanf(cmd.c_str() + 7, "%i %i %i", &a, &r, &v);
    i2c_write(a, r, v);
  } else if (cmd.startsWith("tprot")) {
    int r = 1;
    if (cmd.length() > 5) r = cmd.substring(5).toInt();
    touch_set_rotation(r);
  } else if (cmd.startsWith("tpraw")) {
    int sec = 15;
    if (cmd.length() > 5) sec = cmd.substring(5).toInt();
    tp_raw_test(sec);
  } else if (cmd.startsWith("tplive")) {
    int sec = 15;
    if (cmd.length() > 6) sec = cmd.substring(6).toInt();
    tp_live_test(sec);
  } else if (cmd == "lcdinfo") {
    Serial.printf("屏 : %dx%d  旋转=%d  SPI=%dHz\n",
                  SCREEN_W, SCREEN_H, LCD_ROTATION, (int)LCD_SPI_HZ);
    Serial.printf("引脚 SCLK=%d MOSI=%d MISO=%d CS=%d DC=%d RST=%d BL=%d\n",
                  PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO,
                  PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST, PIN_LCD_BL);
    Serial.printf("init结果=%s\n", display_ok() ? "成功" : "失败");
#endif
  } else if (cmd == "scan") {
    s_scanning = true;
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (adv) adv->stop();
    WiFi.disconnect(false, false);        // 停掉正在进行的连接，把射频空出来
    delay(600);
    Serial.printf("扫描前: 模式=%d 状态=%d 空闲堆=%u\n",
                  (int)WiFi.getMode(), (int)WiFi.status(),
                  (unsigned)ESP.getFreeHeap());

    int n = -99;
    for (int attempt = 1; attempt <= 3; attempt++) {
      Serial.printf("第 %d 次扫描（约 4 秒）...\n", attempt);
      n = WiFi.scanNetworks(false, true);   // 同步扫描，含隐藏 SSID
      Serial.printf("  返回 %d\n", n);
      if (n >= 0) break;
      delay(1000);
    }

    if (n <= 0) {
      Serial.println("扫不到 —— -1=超时 -2=扫描失败 0=确实没信号");
    } else {
      Serial.printf("发现 %d 个 2.4G 网络：\n", n);
      for (int i = 0; i < n; i++) {
        Serial.printf("  %2d) [%s] %-24s ch=%-3d rssi=%-4d %s\n",
                      i + 1,
                      WiFi.BSSIDstr(i).c_str(),
                      WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "<隐藏>",
                      WiFi.channel(i), WiFi.RSSI(i),
                      WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "开放" : "加密");
      }
    }
    WiFi.scanDelete();
    s_advOn      = false;   // 广播已在扫描前停掉
    s_scanning   = false;   // 交回主循环：它会按“抢 12 秒窗口”的节奏重连并恢复广播
    s_wifiTrying = false;
    s_lastWifiTry = 0;
    Serial.println("扫描结束，交回主循环恢复连接与广播");
  } else if (cmd == "reboot") {
    Serial.println("重启...");
    delay(100);
    ESP.restart();
  } else {
    Serial.println("命令: status | scan | wifi <SSID> <密码> | names | name <名字> | fwd on|off | dump | clear | reboot");
  }
}

// ---------------- WiFi 凭证（可持久化） ----------------
static void loadWifiCred() {
  Preferences p;
  p.begin("cpb", true);
  g_ssid = p.getString("ssid", WIFI_SSID);
  g_pass = p.getString("pass", WIFI_PASS);
  p.end();
}

static void saveWifiCred(const String& s, const String& w) {
  Preferences p;
  p.begin("cpb", false);
  p.putString("ssid", s);
  p.putString("pass", w);
  p.end();
}

// ---------------- WiFi 连接 ----------------
// 为什么不用一行 WiFi.begin：
//   这个路由器 2.4G 是 WPA2/WPA3 过渡模式(authmode=7)。
//   Arduino 的 begin() 把最低安全等级写死成 WPA2、SAE 用 hunting-and-pecking、
//   PMF 设成"可选"，跟过渡模式 AP 协商时在认证阶段就挂（reason=2/202）。
// 这里改成：扫描时记住 AP 的 BSSID 和信道 → 定向连接（不再让协议栈自己挑），
// 并把 SAE 的两种 PWE 方法都打开、PMF 只 capable 不 required。
static bool wifiConnectDirected(wifi_auth_mode_t minAuth, unsigned long timeoutMs) {
  wifi_config_t c = {};
  strncpy((char*)c.sta.ssid,     g_ssid.c_str(), sizeof(c.sta.ssid) - 1);
  strncpy((char*)c.sta.password, g_pass.c_str(), sizeof(c.sta.password) - 1);
  c.sta.scan_method        = WIFI_FAST_SCAN;
  c.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
  c.sta.threshold.rssi     = -127;
  c.sta.threshold.authmode = minAuth;
  c.sta.pmf_cfg.capable    = true;
  c.sta.pmf_cfg.required   = false;
  c.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
  if (g_apFound) {                       // 定向：锁 BSSID + 信道，跳过重扫
    c.sta.bssid_set = 1;
    memcpy(c.sta.bssid, g_apBssid, 6);
    c.sta.channel = g_apCh;
  }

  // 顺序很重要：先断开，再写配置，最后连。
  // 之前先 begin 再 set_config 会撞上 "sta is connecting, return error"
  esp_wifi_disconnect();
  delay(150);
  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &c);
  if (e != ESP_OK) {
    Serial.printf("[NET] set_config 失败: %s\n", esp_err_to_name(e));
  }
  delay(100);
  esp_wifi_connect();

  Serial.printf("[NET] 尝试 authmode>=%d%s ...", (int)minAuth,
                g_apFound ? "(锁BSSID)" : "");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
    Serial.print('.');
    ledSet((millis() / 150) % 2);
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.println(ok ? " 成功" : " 失败");
  return ok;
}

// ---------------- setup ----------------
// ---------------- 界面回调（ui.cpp 调用） ----------------
// 运行时替换 BLE 广播名，不重启。
//
//   原来这里是「写 NVS → delay(500) → ESP.restart()」，
//   所以屏幕上点一下「切设备名」等于主动重启，手感和崩溃一模一样。
//
//   现在改成：停广播 → 改 GAP 设备名 → 重开广播。
//   广播包由协议栈按结构化字段（Flags + TX Power + Name）自动生成，
//   名字直接取自 GAP 名，所以重开广播就带上了新名字。
//   已连接的客户端不受影响（GATT 服务没动）。
static bool ble_apply_name(const String& rawName) {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if (!adv) { Serial.println("[BLE] 取不到广播对象，改名失败"); return false; }

  // 库里有句警告：设备名超过 29 字符会在配置扫描响应时崩，这里先截断
  String name = rawName;
  if (name.length() > 20) name = name.substring(0, 20);

  adv->stop();
  delay(80);                                   // 给射频和 BTC 任务收尾时间

  esp_ble_gap_set_device_name(name.c_str());   // 协议栈内部拷贝，安全
  delay(40);

  adv->start();                                // 非 custom 模式：用新 GAP 名重新生成广播数据
  Serial.printf("[BLE] 广播名 -> \"%s\"  已重新开始广播\n", name.c_str());
  return true;
}

void app_cycle_devname() {
  int cur = -1;
  for (int i = 0; i < PRESET_COUNT; i++) {
    if (g_devName == PRESET_NAMES[i]) { cur = i; break; }
  }
  int nxt = (cur + 1) % PRESET_COUNT;

  g_devName = PRESET_NAMES[nxt];

  Preferences p;
  p.begin("cpb", false);
  p.putString("name", g_devName);
  p.end();

  state_setDevName(g_devName.c_str());
  Serial.printf("[UI] 设备名 -> %s（已存 NVS，立即生效，不重启）\n", g_devName.c_str());

  ble_apply_name(g_devName);

#if UI_ON
  lv_refr_now(nullptr);      // 让屏上马上看到新名字
#endif
}

void app_reconnect_wifi() {
  Serial.println("[UI] 手动重连 WiFi");
  WiFi.disconnect(false, false);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  s_lastWifiTry = millis();
  s_wifiTrying  = true;
  s_retryStart  = millis();
}

void app_reboot() {
  Serial.println("[UI] 重启");
#if UI_ON
  lv_refr_now(nullptr);
#endif
  delay(300);
  ESP.restart();
}

void app_set_forward(bool on) {
  s_forward = on;
  Serial.printf("[UI] 自动转发 %s\n", on ? "开" : "关");
}

bool app_get_forward() { return s_forward; }

String app_agent_str() {
  return String(AGENT_HOST) + ":" + String((unsigned)AGENT_PORT);
}

// 上次为什么重启 —— 这是判断「点着点着重启」的唯一硬证据：
//   PANIC / TASK_WDT / INT_WDT → 软件崩了或被看门狗掐了
//   SW   → 代码里主动 ESP.restart()（比如点了重启按钮）
//   BROWNOUT → 供电掉压（USB 线/接口带不动背光+WiFi 发射）
//   POWERON  → 真的断电重来了
static void printResetReason() {
  const char* cn = "未知";
  const char* hint = "";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   cn = "上电/拔插";      break;
    case ESP_RST_EXT:       cn = "外部复位脚";     break;
    case ESP_RST_SW:        cn = "软件主动重启";   hint = "（点到了重启按钮，或代码里调了 ESP.restart）"; break;
    case ESP_RST_PANIC:     cn = "程序崩溃(PANIC)"; hint = "（有 bug 或内存越界，看上面的 Guru Meditation 回溯）"; break;
    case ESP_RST_INT_WDT:   cn = "中断看门狗";     hint = "（中断里卡太久）"; break;
    case ESP_RST_TASK_WDT:  cn = "任务看门狗";     hint = "（某个任务卡住超过 5 秒）"; break;
    case ESP_RST_WDT:       cn = "看门狗";         break;
    case ESP_RST_BROWNOUT:  cn = "供电掉压(BROWNOUT)"; hint = "（USB 供电不足，换口/换线，别用扩展坞）"; break;
    case ESP_RST_SDIO:      cn = "SDIO";           break;
#ifdef ESP_RST_USB
    case ESP_RST_USB:       cn = "USB 复位";       hint = "（串口被打开/烧录触发，不是真故障）"; break;
#endif
    default:                cn = "其它";           hint = "（0x15/USB_UART_CHIP_RESET 属串口打开触发，可忽略）"; break;
  }
  Serial.printf("[SYS] 上次重启原因: %s(%d) %s\n", cn, (int)esp_reset_reason(), hint);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  printResetReason();

  state_init();

  // 屏最先初始化：出问题时屏幕上能直接看到原因
#if UI_ON
  if (display_init()) {
    display_lvgl_init();
    ui_init();
    Serial.println("[UI] LVGL 已启动");
  } else {
    Serial.println("[UI] 屏初始化失败（白屏/黑屏请核对 board_profile.h 的引脚和驱动型号）");
  }
#endif

  loadWifiCred();

  // 读 NVS 里的设备名
  Preferences prefs;
  prefs.begin("cpb", true);
  g_devName = prefs.getString("name", DEV_NAME);
  prefs.end();
  if (!g_devName.length()) g_devName = DEV_NAME;
  state_setDevName(g_devName.c_str());

  Serial.printf("\n\n=== 云打印盒 v%s ===\n", FW_VERSION);
  Serial.printf("冒充设备名: \"%s\"\n", g_devName.c_str());
  Serial.println("串口输入 names 可看候选机型，name <名字> 可切换\n");

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
#endif

  // ---- 先起 BLE ----
  // C3 上 WiFi/BLE 共存（coex）要求 BLE 先注册。反过来的话
  // 连上 WiFi 再 BLEDevice::init() 会触发 coex_core_enable 断言 -> panic 重启。
  Serial.printf("[SYS] BLE 前空闲堆: %u\n", (unsigned)ESP.getFreeHeap());
  BLEDevice::init(g_devName.c_str());
  Serial.printf("[SYS] BLE init 后空闲堆: %u\n", (unsigned)ESP.getFreeHeap());

  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new SrvCallback());

  addPrinterService(srv, SVC_FF00, CHR_FF02, CHR_FF01, "FF00");
  addPrinterService(srv, SVC_18F0, CHR_2AF0, CHR_2AF1, "18F0");
  addPrinterService(srv, SVC_FFE0, CHR_FFE1, CHR_FFE1, "FFE0");
  addPrinterService(srv, SVC_NUS,  CHR_NUS_RX, CHR_NUS_TX, "NUS");
  Serial.printf("[SYS] 服务建完空闲堆: %u\n", (unsigned)ESP.getFreeHeap());

  // 广播数据交给协议栈按结构化字段自动拼：Flags + TX Power + 设备名。
  // 名字取自 GAP 设备名，所以改名只要 set_device_name + 重开广播就能生效。
  //
  // ⚠ 绝对不要在这里调 adv->setAdvertisementData() / setScanResponseData()：
  //   BLEAdvertisementData::getPayload() 的签名是 `std::string getPayload();`（按值返回），
  //   库内实现是 esp_ble_gap_config_adv_data_raw(advData.getPayload().data(), ...)。
  //   临时字符串在语句结束就析构，而 BTC 任务异步处理这条命令时那块内存已被回收，
  //   实测结果：CORRUPT HEAP: Bad head + assert failed: multi_heap_free + PANIC 重启。
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setScanResponse(false);   // 名字放广告包，扫描响应留给将来扩展
  adv->setMinPreferred(0x06);

  // 广播故意延后到 WiFi 有结果之后再开：
  // BLE 广播占着射频时，WiFi 关联阶段会失败(reason=201)。
  // 但 WiFi 失败时也必须开广播 —— 那种情况下 BLE 是唯一可用出口。

  // ---- 再起 WiFi ----
  // 注意：这里绝对不能 WiFi.setSleep(false)。
  // C3 上 Wi-Fi 与 Bluetooth 共存时 WiFi 必须保持 modem sleep，
  // 关掉会直接 abort（日志: Should enable WiFi modem sleep when both enabled）
  WiFi.persistent(false);
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    if (e == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      Serial.println("\n[WiFi] 已关联 AP，开始握手");
    } else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.println("[WiFi] 拿到 IP");
    } else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      // reason 常见值：15=握手超时(密码错) 204=握手超时 2=认证过期
      //               3=AP踢开 201=找不到AP 205=连接失败
      Serial.printf("\n[WiFi] 断开 reason=%u\n",
                    (unsigned)info.wifi_sta_disconnected.reason);
    }
  });
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(200);

  // 先探一次：这个 AP 到底是 WPA2 还是 WPA3。
  // ESP32 默认最低安全等级 WPA2 + SAE 用 hunting-and-pecking，
  // 碰上 WPA3-only 的 AP 会以 reason=2(AUTH_EXPIRE)/202(AUTH_FAIL) 失败。
  int  apEnc = -1;
  String apBssidStr = "-";
  {
    int n = WiFi.scanNetworks(false, true);
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == g_ssid) {
        apEnc = (int)WiFi.encryptionType(i);
        g_apCh = WiFi.channel(i);
        memcpy(g_apBssid, WiFi.BSSID(i), 6);
        apBssidStr = WiFi.BSSIDstr(i);
        g_apFound = true;
        break;
      }
    }
    WiFi.scanDelete();
    Serial.printf("[NET] 扫描 %d 个网络，目标%s ch=%d authmode=%d bssid=%s\n",
                  n, g_apFound ? "已找到" : "未找到", g_apCh, apEnc,
                  apBssidStr.c_str());
  }
  const bool isWpa3 = (apEnc == WIFI_AUTH_WPA3_PSK || apEnc == WIFI_AUTH_WPA2_WPA3_PSK);
  const char* encName =
      apEnc == WIFI_AUTH_WPA3_PSK      ? "WPA3-SAE" :
      apEnc == WIFI_AUTH_WPA2_WPA3_PSK ? "WPA2/WPA3 混合" :
      apEnc == WIFI_AUTH_WPA2_PSK      ? "WPA2-PSK" :
      apEnc == WIFI_AUTH_WPA_PSK       ? "WPA-PSK" :
      apEnc == WIFI_AUTH_OPEN          ? "开放" : "其他";
  Serial.printf("[NET] AP 加密: %s（%s）\n", encName, isWpa3 ? "过渡模式，需 SAE" : "纯 WPA2");
  Serial.printf("[NET] 本机 WiFi MAC: %s\n", WiFi.macAddress().c_str());

  // 三段式兜底，全用定向连接（锁 BSSID + 信道），谁先通就用谁：
  //  ① authmode>=3 只走 WPA2-PSK（最通用，绝大多数 IoT 场景一次就通）
  //  ② authmode>=7 过渡模式
  //  ③ authmode>=6 只走 SAE
  // PMF 一律 capable 但不 required —— 实测设成必需会直接找不到 AP(reason=201)
  if (!wifiConnectDirected(WIFI_AUTH_WPA2_PSK,      9000) &&
      !wifiConnectDirected(WIFI_AUTH_WPA2_WPA3_PSK, 8000) &&
      !wifiConnectDirected(WIFI_AUTH_WPA3_PSK,      8000)) {
    Serial.println("[NET] 三种安全模式都失败");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[NET] 已连接 IP=%s RSSI=%d 空闲堆=%u\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  (unsigned)ESP.getFreeHeap());
    Serial.printf("[NET] 电脑代理 http://%s:%u%s\n",
                  AGENT_HOST, (unsigned)AGENT_PORT, AGENT_PATH);
  } else {
    Serial.println("[NET] 连接失败 —— 只广播 BLE，数据 dump 到串口");
  }

  // WiFi 有结果了（连上或超时）再开广播
  if (!s_advOn) {
    BLEDevice::startAdvertising();
    s_advOn = true;
    Serial.printf("[BLE] 广播中：\"%s\"  (空闲堆 %u)\n",
                  g_devName.c_str(), (unsigned)ESP.getFreeHeap());
  }
  ledSet(false);
}

// ---------------- loop ----------------
void loop() {
  unsigned long now = millis();

  if (s_ledUntil && now > s_ledUntil) { ledSet(s_bleConn); s_ledUntil = 0; }

  // 连上后延迟 1.2 秒主动上报"就绪"——给客户端留出订阅 notify 的时间
  static unsigned long helloAt = 0;
  if (s_sendHello) {
    if (!helloAt) helloAt = now + 1200;
    if (now >= helloAt) {
      uint8_t st = 0x16;
      ntfSend(&st, 1);
      Serial.printf("[TX] 主动上报状态 16（就绪）-> %u 个通知通道\n", (unsigned)s_ntfCount);
      s_sendHello = false;
      helloAt = 0;
    }
  }

  // WiFi 断线重连：射频要和 BLE 广播共用，所以每次重连只抢 12 秒窗口，
  // 窗口内暂停广播（不暂停根本连不上），窗口外恢复广播（保证手机能扫到设备）。
  if (!s_scanning) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!s_advOn) {
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        if (adv) adv->start();
        s_advOn = true;
        Serial.printf("[NET] WiFi 已恢复 IP=%s，广播已恢复\n",
                      WiFi.localIP().toString().c_str());
      }
      s_wifiTrying = false;
    } else if (!s_wifiTrying) {
      if (now - s_lastWifiTry > 10000) {
        s_wifiTrying  = true;
        s_retryStart  = now;
        s_lastWifiTry = now;
        if (s_advOn) {
          BLEAdvertising* adv = BLEDevice::getAdvertising();
          if (adv) adv->stop();
          s_advOn = false;
        }
        Serial.println("[NET] 重连 WiFi（暂停广播 12 秒）...");
        WiFi.disconnect(false, false);
        WiFi.begin(g_ssid.c_str(), g_pass.c_str());
      } else if (!s_advOn) {
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        if (adv) adv->start();
        s_advOn = true;
      }
    } else if (now - s_retryStart > 12000) {
      s_wifiTrying = false;
      if (!s_advOn) {
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        if (adv) adv->start();
        s_advOn = true;
      }
    }
  }

  // 断开时强制收尾
  if (s_forceFlush) {
    s_forceFlush = false;
    if (s_jobActive) flushChunk(true);
  }

  // 段满 / 空闲 判定
  size_t n;
  unsigned long last, first;
  bool active;
  portENTER_CRITICAL(&s_mux);
  n = s_len; last = s_lastMs; first = s_firstMs; active = s_jobActive;
  portEXIT_CRITICAL(&s_mux);

  if (active) {
    bool full  = (n >= CHUNK_SIZE);
    bool idle  = (n > 0) && ((now - last) > JOB_IDLE_MS);
    bool stuck = (n > 0) && ((now - first) > JOB_HOLD_MS);
    if (full)        flushChunk(false);   // 段满先送出去，任务还没完
    else if (idle || stuck) flushChunk(true);
  }

  if (Serial.available()) handleSerial();

  // 状态每秒采一次（时间、WiFi 信号、跨天清零）
  static unsigned long lastTick = 0;
  if (now - lastTick > 1000) {
    lastTick = now;
    state_tick();
    bool up = (WiFi.status() == WL_CONNECTED);
    String ip = up ? WiFi.localIP().toString() : String("");
    state_setWifi(up, ip.c_str(), g_ssid.c_str(), up ? WiFi.RSSI() : 0);
  }

#if UI_ON
  display_task();     // 跑在 core 1，不占蓝牙/WiFi 那个核
  ui_update();
#endif

  delay(5);
}
