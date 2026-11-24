#include "display_port.h"
#include <Preferences.h>
#include "board_profile.h"

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

// ============================================================
// 面板 / 触摸类按 board_profile.h 选
// ============================================================
#if defined(LCD_DRV_ILI9341)
  using PanelT = lgfx::Panel_ILI9341;
#elif defined(LCD_DRV_ST7796)
  using PanelT = lgfx::Panel_ST7796;
#elif defined(LCD_DRV_ST7789)
  using PanelT = lgfx::Panel_ST7789;
#else
  #error "board_profile.h 里没选屏驱动（LCD_DRV_xxx）"
#endif

#if defined(TOUCH_XPT2046)
  using TouchT = lgfx::Touch_XPT2046;
  #define TOUCH_KIND "XPT2046(电阻)"
#elif defined(TOUCH_CST816)
  using TouchT = lgfx::Touch_CST816S;
  #define TOUCH_KIND "CST816S(电容)"
#elif defined(TOUCH_GT911)
  using TouchT = lgfx::Touch_GT911;
  #define TOUCH_KIND "GT911(电容)"
#elif defined(TOUCH_FT5X06)
  using TouchT = lgfx::Touch_FT5x06;
  #define TOUCH_KIND "FT6236/6336(电容)"
#else
  #define TOUCH_KIND "无"
#endif

class Lcd : public lgfx::LGFX_Device {
    lgfx::Bus_SPI  _bus;
    PanelT         _panel;
#if PIN_LCD_BL >= 0 && !defined(LCD_BL_PLAIN)
    lgfx::Light_PWM _light;      // PWM 调光
#endif
#if HAS_TOUCH
    TouchT         _touch;
#endif

public:
    Lcd() {
        // ---- SPI 总线 ----
        {
            auto c = _bus.config();
            c.spi_host    = SPI2_HOST;      // 屏固定用 SPI2，避开 flash 的 SPI0/1
            c.spi_mode    = 0;
            c.freq_write  = LCD_SPI_HZ;
            c.freq_read   = 16000000;
            c.spi_3wire   = (PIN_LCD_MISO < 0);   // 有 MISO 就走 4 线
            c.use_lock    = true;
            c.dma_channel = SPI_DMA_CH_AUTO;
            c.pin_sclk    = PIN_LCD_SCLK;
            c.pin_mosi    = PIN_LCD_MOSI;
            c.pin_miso    = PIN_LCD_MISO;
            c.pin_dc      = PIN_LCD_DC;
            _bus.config(c);
            _panel.setBus(&_bus);
        }

        // ---- 屏 ----
        {
            auto c = _panel.config();
            c.pin_cs           = PIN_LCD_CS;
            c.pin_rst          = PIN_LCD_RST;
            c.pin_busy         = -1;
            c.panel_width      = LCD_W;
            c.panel_height     = LCD_H;
            c.offset_x         = 0;
            c.offset_y         = 0;
            c.offset_rotation  = 0;
            c.dummy_read_pixel = 8;
            c.dummy_read_bits  = 1;
            c.readable         = false;   // 只写不读，省掉上电读 ID 那一步（读不到会拖慢甚至判失败）
            c.invert           = LCD_INVERT_COLOR;
            c.rgb_order        = LCD_RGB_ORDER;
            c.dlen_16bit       = false;
            c.bus_shared       = true;
            _panel.config(c);
        }

        // ---- 背光 ----
#if PIN_LCD_BL >= 0 && !defined(LCD_BL_PLAIN)
        {
            auto c = _light.config();
            c.pin_bl      = PIN_LCD_BL;
            c.invert      = false;
            c.freq        = 12000;
            c.pwm_channel = 7;
            _light.config(c);
            _panel.setLight(&_light);
        }
#endif

        // ---- 触摸 ----
#if HAS_TOUCH
#if defined(TOUCH_XPT2046)
        {
            auto c = _touch.config();
            c.x_min          = TP_X_MIN;
            c.x_max          = TP_X_MAX;
            c.y_min          = TP_Y_MIN;
            c.y_max          = TP_Y_MAX;
            c.pin_int        = PIN_TP_IRQ;
            c.pin_cs         = PIN_TP_CS;
            c.spi_host       = SPI2_HOST;   // 和屏共总线，靠 CS 区分
            c.freq           = TP_SPI_HZ;
            c.offset_rotation = 0;
            c.bus_shared     = true;
            _touch.config(c);
            _panel.setTouch(&_touch);
        }
#elif defined(TOUCH_CST816) || defined(TOUCH_GT911) || defined(TOUCH_FT5X06)
        {
            auto c = _touch.config();
#if defined(TOUCH_FT5X06)
  // ⚠ LovyanGFX 的 i2c_addr 收的是【7 位地址】，构造函数默认就是 0x38。
  //   千万别照 GT911 那样再 <<1 —— 左移成 0x70 后芯片完全不应答，
  //   _check_init() 永远 false，表现就是"屏正常但怎么点都没反应"。
  #ifdef TP_I2C_ADDR
            c.i2c_addr       = (TP_I2C_ADDR);
  #else
            c.i2c_addr       = 0x38;
  #endif
#else
            c.i2c_addr       = (0x15 << 1);
#endif
            c.pin_sda        = PIN_TP_SDA;
            c.pin_scl        = PIN_TP_SCL;
            c.pin_int        = PIN_TP_IRQ;
            c.pin_rst        = PIN_TP_RST;
            c.freq           = 400000;
            c.x_min          = 0;
            c.x_max          = LCD_W;
            c.y_min          = 0;
            c.y_max          = LCD_H;
#ifdef TP_OFFSET_ROT
            c.offset_rotation = TP_OFFSET_ROT;   // 触摸轴补偿，屏上翻/镜像就改这个
#else
            c.offset_rotation = 0;
#endif
            _touch.config(c);
            _panel.setTouch(&_touch);
        }
#endif   // 具体触摸型号
#endif   // HAS_TOUCH

        setPanel(&_panel);
    }

    // 运行时改触摸轴补偿，配合串口 tprot 命令现场试四个方向
    void setTouchRotation(uint8_t r) {
#if HAS_TOUCH
        auto c = _touch.config();      // config() 是值返回，必须读出来改完再写回
        c.offset_rotation = r & 3;
        _touch.config(c);
#endif
    }
};

static Lcd       s_lcd;
static bool      s_dispOk  = false;
static bool s_invertOn = (LCD_INVERT_COLOR != 0);   // 当前反色状态，运行时可切

// 触摸统计：给测试页显示"你点在哪个坐标"。
// 屏幕上直接看，不用串口、不用我这边跟着猜。
static uint16_t s_tpX = 0xFFFF, s_tpY = 0xFFFF;
static uint32_t s_tpCount = 0;
static bool     s_tpDown  = false;

static bool invert_load();
static bool      s_touchOk = false;
static uint8_t   s_bright  = 200;
static bool      s_uiPaused = false;
static bool      s_wireUp  = false;   // I2C 是否已 begin   // 测屏时暂停刷新

// LVGL 缓冲：只开一块，按脏矩形局部刷新
static lv_color_t s_buf[SCREEN_W * 24];
static lv_disp_draw_buf_t s_drawBuf;
static lv_disp_drv_t      s_dispDrv;
static lv_indev_drv_t     s_indevDrv;

static void disp_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    s_lcd.startWrite();
    s_lcd.setAddrWindow(area->x1, area->y1, w, h);
    s_lcd.writePixels(reinterpret_cast<lgfx::rgb565_t*>(px), w * h);
    s_lcd.endWrite();
    lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
#if HAS_TOUCH
    static uint16_t s_lastX = 0, s_lastY = 0;
    static uint32_t s_lastLog = 0;

    uint16_t x = 0, y = 0;
    if (s_lcd.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PR;

        // 坐标调试：只在位置明显变化时打一行，避免刷屏。
        // 手指点左上角却打出 x=300 这种，就是方向/映射反了。
        s_tpX = x; s_tpY = y;
        if (!s_tpDown) { s_tpDown = true; s_tpCount++; }   // 按下算一次，按住不重复计数

        uint32_t now = millis();
        if (now - s_lastLog > 120 &&
            (abs((int)x - (int)s_lastX) > 8 || abs((int)y - (int)s_lastY) > 8)) {
            s_lastLog = now; s_lastX = x; s_lastY = y;
            Serial.printf("[TP] x=%u y=%u  (屏 %dx%d) 第%u次\n",
                          x, y, (int)s_lcd.width(), (int)s_lcd.height(), (unsigned)s_tpCount);
        }
        return;
    }
#endif
    data->state = LV_INDEV_STATE_REL;
    s_tpDown = false;
}

bool display_init() {
    // ------------------------------------------------------------
    // I2C 那条 0x18：官方资料已确认它是 ES8311 音频解码芯片，不是 IO 扩展器。
    // 以前的固件往它写 0x03/0x01，纯属误伤（也是「屏上出现一条亮带」的真正原因
    // ——那是乱写音频 codec 寄存器搞出来的，跟屏没关系）。
    // 默认不动它；只有拿 PRESET_S3_28_DC18* 那几组老配置排查时才编进来。
    // ------------------------------------------------------------
#if defined(LCD_EXP_INIT)
    {
        Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
        delay(50);
        Wire.beginTransmission(0x18);
        Wire.write((uint8_t)0x03); Wire.write((uint8_t)0x00);
        bool okCfg = (Wire.endTransmission() == 0);
        Wire.beginTransmission(0x18);
        Wire.write((uint8_t)0x01); Wire.write((uint8_t)0xFF);
        bool okOut = (Wire.endTransmission() == 0);
        Serial.printf("[EXP] 0x18 放开: 方向=%s 输出=%s\n",
                      okCfg ? "OK" : "失败", okOut ? "OK" : "失败");
        s_wireUp = true;
    }
#endif

    // ------------------------------------------------------------
    // ⚠ 顺序很重要（2026-11-24 用血换的）
    //   这块板的 LCD 复位脚和 ESP32 的 EN 是共用的，没有独立 RST。
    //   后果有两个：
    //     1) 烧录后 esptool 用 RTS 复位，屏和 MCU 同时放开复位，
    //        ILI9341 会把 boot 阶段 SPI 脚上的毛刺当成指令吃掉，
    //        内部状态机进未知态，init() 失败 → 必须拔插冷上电才恢复。
    //     2) 原来的代码是 init 失败就 return，背光根本没开，
    //        屏全黑，和"数据线全错"长得一模一样，排查时分不清。
    //   所以这里先把总线脚钉死、背光强行点亮，再 init，失败还重试。
    // ------------------------------------------------------------

    // 总线脚上电可能是浮空，先给确定电平，别让面板收垃圾
    pinMode(PIN_LCD_CS,   OUTPUT); digitalWrite(PIN_LCD_CS,   HIGH);  // 不选中
    pinMode(PIN_LCD_DC,   OUTPUT); digitalWrite(PIN_LCD_DC,   HIGH);
    pinMode(PIN_LCD_SCLK, OUTPUT); digitalWrite(PIN_LCD_SCLK, LOW);
    pinMode(PIN_LCD_MOSI, OUTPUT); digitalWrite(PIN_LCD_MOSI, LOW);
#if PIN_LCD_RST >= 0
    pinMode(PIN_LCD_RST,  OUTPUT); digitalWrite(PIN_LCD_RST,  HIGH);
#endif
    delay(80);

    // 背光先无条件点亮：init 失败也要能看到白屏/花屏，别死黑
#if PIN_LCD_BL >= 0
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);
    delay(60);                  // 等背光升压稳住
#endif

    // init 失败重试：热复位后 ILI9341 状态不对是常态，别一次定生死
    for (int attempt = 1; attempt <= 3; attempt++) {
        s_dispOk = s_lcd.init();
        Serial.printf("[LCD] init() 第 %d 次: %s\n", attempt, s_dispOk ? "成功" : "失败");
        if (s_dispOk) break;
        s_lcd.writeCommand(0x01);   // SWRESET 软复位兜底
        delay(250);
    }
    if (!s_dispOk) {
        Serial.println("[LCD] 三次 init 都失败。多半是热复位没让屏真正断电 ——");
        Serial.println("[LCD] 拔掉 USB 再插一次（冷上电），不要只按 RESET。");
        return false;
    }

    s_lcd.setRotation(LCD_ROTATION);

    // 反色：优先用 NVS 里记下的那个值（用户在测试页现场试出来的），
    // 没有记录才用 board_profile.h 的默认值。
    s_invertOn = invert_load();
    s_lcd.invertDisplay(s_invertOn);

    // 先在屏上刷一块纯色，证明数据通路真的通了（不等 LVGL 起来）。
    // 启动瞬间能看到这一下，就说明"屏 + 总线 + 驱动"三样都对。
    s_lcd.fillScreen(0x0841);   // 深蓝灰，刷黑看不出区别
    delay(120);

#ifndef LCD_BL_PLAIN
    // 背光刚才已用 digitalWrite 点亮，这里交给 PWM 通道正式接管（支持调光）
    s_lcd.setBrightness(s_bright);
#else
    Serial.printf("[LCD] 背光 GPIO%d 直流常高（LCD_BL_PLAIN）\n", PIN_LCD_BL);
#endif

#if HAS_TOUCH
    // 触摸能不能用，用一次读取来判定：读到了值就认为通
    {
        uint16_t tx = 0, ty = 0;
        s_touchOk = s_lcd.getTouch(&tx, &ty) || true;   // 无触碰时返回 false 也算通
        // 真正的判定放在界面里：用户点一下就知道
    }
    s_touchOk = true;
#else
    s_touchOk = false;
#endif

    Serial.printf("[LCD] %dx%d 屏就绪, 触摸: %s\n",
                  (int)s_lcd.width(), (int)s_lcd.height(),
                  s_touchOk ? TOUCH_KIND : "无");
    return true;
}

void display_lvgl_init() {
    lv_init();

    lv_disp_draw_buf_init(&s_drawBuf, s_buf, nullptr, SCREEN_W * 24);

    lv_disp_drv_init(&s_dispDrv);
    s_dispDrv.hor_res  = s_lcd.width();
    s_dispDrv.ver_res  = s_lcd.height();
    s_dispDrv.flush_cb = disp_flush;
    s_dispDrv.draw_buf = &s_drawBuf;
    lv_disp_drv_register(&s_dispDrv);

    lv_indev_drv_init(&s_indevDrv);
    s_indevDrv.type      = LV_INDEV_TYPE_POINTER;
    s_indevDrv.read_cb   = touch_read;
    s_indevDrv.scroll_limit = 6;
    lv_indev_drv_register(&s_indevDrv);
}

void display_task() {
    if (s_uiPaused) return;
    lv_timer_handler();
}

void display_setBrightness(uint8_t v) {
    s_bright = v;
#if defined(LCD_BL_PLAIN)
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, v > 0 ? HIGH : LOW);
#elif PIN_LCD_BL >= 0
    s_lcd.setBrightness(v);
#endif
}

uint8_t display_getBrightness() { return s_bright; }

void display_sleep(bool sleep) {
#if defined(LCD_BL_PLAIN)
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, sleep ? LOW : HIGH);
#elif PIN_LCD_BL >= 0
    s_lcd.setBrightness(sleep ? 0 : s_bright);
#endif
}

bool display_ok() { return s_dispOk; }
bool touch_ok()   { return s_touchOk; }

// ============================================================
// 引脚排查工具
// 屏黑的时候只有两种可能：背光没开（整块死黑）或数据线不对（有微光无画面）。
// 这两个函数就是用来把两种情况分开的。
// ============================================================
void display_bl_pin(int pin, bool on) {
    if (pin < 0) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, on ? HIGH : LOW);
}

void display_bl_sweep(int from, int to) {
    // S3 上能当背光的脚：排掉 19/20（USB）、43/44（串口）、26~37（Flash/PSRAM）
    // 也排掉屏自己的总线脚（9/10/11/12/13/46），免得把白画面打乱
    static const int pins[] = {
        45, 47, 48, 1, 2, 3, 4, 5, 6, 7, 8,
        14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42,
        43, 44                       // 串口脚：改用 USB 后可能被挪来做背光
    };
    const int n = sizeof(pins) / sizeof(pins[0]);
    if (to <= 0 || to > n) to = n;
    if (from < 0) from = 0;

    // 保持白画面：LVGL 停掉，然后刷白，扫描期间不再重绘
    s_uiPaused = true;
    display_fill(255, 255, 255);
    delay(200);

    Serial.printf("\n===== 背光扫描 [%d..%d] / 共 %d 个 =====", from, to - 1, n);
    Serial.println("（画面应保持全白，哪一下变亮就是那个脚）");

    for (int i = from; i < to; i++) {
        int p = pins[i];
        Serial.printf("[%02d] GPIO%-2d 高 ... ", i, p); Serial.flush();
        ledcDetachPin(p);
        pinMode(p, OUTPUT);
        digitalWrite(p, HIGH);
        delay(1200);
        digitalWrite(p, LOW);
        pinMode(p, INPUT);          // 松手，避免一直占着
        Serial.println("低");
    }
    Serial.printf("===== 本轮结束（%d..%d）=====\n", from, to - 1);
}

void display_fill(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    s_lcd.fillScreen(c);
    Serial.printf("[LCD] 全屏填充 R=%u G=%u B=%u\n", r, g, b);
}

// Wire 必须先 begin 才能收发。之前 i2c_dump / i2c_write 漏了这一步，
// 只有先跑过 i2cscan 的会话才能读出数据，单独调用全是 NAK（等于白测）。
static void wire_ensure() {
    if (s_wireUp) return;
    Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
    delay(60);
    s_wireUp = true;
}

void i2c_scan(int sda, int scl) {
    Wire.begin(sda, scl, 400000);
    delay(60);
    s_wireUp = true;
    Serial.printf("[I2C] 扫描 SDA=%d SCL=%d ...\n", sda, scl);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   0x%02X 在线\n", a);
            found++;
        }
    }
    Serial.printf("[I2C] 共 %d 个设备\n", found);
}

void i2c_dump(int addr, int start) {
    wire_ensure();
    if (start < 0) start = 0;
    if (start > 0xF0) start = 0xF0;
    Serial.printf("[I2C] 读 0x%02X 的寄存器 0x%02X~0x%02X：\n", addr, start, start + 15);
    for (int i = 0; i < 16; i++) {
        int r = start + i;
        Wire.beginTransmission(addr);
        Wire.write((uint8_t)r);
        if (Wire.endTransmission(false) != 0) {
            Serial.printf("  reg 0x%02X : NAK（读不了）\n", r);
            continue;
        }
        int n = Wire.requestFrom((uint8_t)addr, (uint8_t)1);
        if (n == 1) {
            uint8_t v = Wire.read();
            Serial.printf("  reg 0x%02X = 0x%02X  (%d)\n", r, v, v);
        } else {
            Serial.printf("  reg 0x%02X : 无数据\n", r);
        }
    }
    // FT6x36 专用解读：0xA3=CIPHER, 0xA8=VENDID，跟 LovyanGFX 的判定条件对齐
    if (addr == 0x38 && start == 0xA3) {
        Serial.println("[TP] 0xA3=CIPHER 0xA8=VENDID；LovyanGFX 要求 0xA8 非 0 才认这块触摸");
    }
}

void i2c_write(int addr, int reg, int val) {
    wire_ensure();
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)reg);
    Wire.write((uint8_t)val);
    int e = Wire.endTransmission();
    Serial.printf("[I2C] 写 0x%02X[0x%02X]=0x%02X  结果=%d(%s)\n",
                  addr, reg, val, e, e == 0 ? "OK" : "失败");
}
void display_color_cycle() {
    Serial.println("[LCD] 六色循环，每色 1 秒：红 绿 蓝 白 红 绿");
    const uint8_t seq[6][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
        {255, 255, 255}, {255, 0, 0}, {0, 255, 0}
    };
    for (int i = 0; i < 6; i++) {
        display_fill(seq[i][0], seq[i][1], seq[i][2]);
        delay(1000);
    }
    Serial.println("[LCD] 循环结束");
}

// ---- 暂停刷新：测屏时必须停 LVGL，否则界面马上把测试颜色盖掉 ----
void display_pause(bool paused) {
    s_uiPaused = paused;
    Serial.printf("[LCD] LVGL 刷新%s\n", paused ? "已暂停" : "已恢复");
}
bool display_is_paused() { return s_uiPaused; }

void lcd_bl_test(int pin) {
    s_uiPaused = true;          // 先把界面停掉

    if (pin >= 0) {
        // LGFX 的 PWM 背光占着 LEDC 通道，必须解绑，否则 digitalWrite 无效
        ledcDetachPin(pin);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
        Serial.printf("[BLTEST] GPIO%d 拉高\n", pin);
    } else {
        Serial.println("[BLTEST] 不驱动任何脚");
    }

    const uint8_t seq[4][3] = {
        {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}
    };
    const char* nm[4] = {"白", "红", "绿", "蓝"};
    for (int i = 0; i < 4; i++) {
        display_fill(seq[i][0], seq[i][1], seq[i][2]);
        Serial.printf("[BLTEST] 正在显示：%s\n", nm[i]);
        delay(1200);
    }
    Serial.println("[BLTEST] 结束（界面仍是暂停的，发 uiresume 恢复）");
}

// ============================================================
// 原始 SPI 探针 —— 不用 LovyanGFX
//
// 为什么要绕过库：LGFX 的引脚全是编译期写死的，改一个 DC 就得重编重烧一次，
// 每验证一个组合都要你来当"眼睛"，一轮就是几分钟。这里直接拿 Arduino 的
// SPI 硬件口，DC 脚在运行时任意指定，一次烧录就能把所有候选跑完。
//
// 每一轮的节奏：黑 0.6s → 白 1.2s → 红 1.2s → 绿 1.2s → 蓝 1.2s
// 只要某一轮看到颜色依次变化，那个 DC 脚就是对的。
// ============================================================
namespace rawspi {

static int s_dc = -1;

static inline void cmd(uint8_t c) {
    digitalWrite(s_dc, LOW);
    delayMicroseconds(3);
    SPI.transfer(c);
    delayMicroseconds(3);
}
static inline void dat(uint8_t d) {
    digitalWrite(s_dc, HIGH);
    delayMicroseconds(3);
    SPI.transfer(d);
    delayMicroseconds(3);
}

// ST7789 最小可用初始化：软复位 → 休眠出 → 16 位色 → MADCTL → 反色 → 开显示
static void st7789_init(bool invert) {
    cmd(0x01); delay(150);            // SWRESET
    cmd(0x11); delay(150);            // SLPOUT
    cmd(0x3A); dat(0x55); delay(10);  // COLMOD = 16bit
    cmd(0x36); dat(0x00); delay(10);  // MADCTL 竖屏
    if (invert) { cmd(0x21); } else { cmd(0x20); }
    delay(10);
    cmd(0x13); delay(10);             // NORON
    cmd(0x29); delay(60);             // DISPON
}

// 充满整屏：面板原生 240x320
static void fill(uint16_t color) {
    cmd(0x2A);                        // CASET 0..239
    dat(0x00); dat(0x00); dat(0x00); dat(0xEF);
    cmd(0x2B);                        // RASET 0..319
    dat(0x00); dat(0x00); dat(0x01); dat(0x3F);
    cmd(0x2C);                        // RAMWR
    digitalWrite(s_dc, HIGH);

    static uint8_t line[240 * 2];
    const uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)(color & 0xFF);
    for (int i = 0; i < 240; i++) { line[i * 2] = hi; line[i * 2 + 1] = lo; }
    for (int y = 0; y < 320; y++) SPI.writePixels(line, sizeof(line));
}

// 只刷一行（用来画边框，判断几何是否铺满整屏）
static void fillRow(int y, uint16_t color) {
    cmd(0x2A);
    dat(0x00); dat(0x00); dat(0x00); dat(0xEF);
    cmd(0x2B);
    dat(0x00); dat((uint8_t)(y >> 8)); dat((uint8_t)(y & 0xFF)); dat(0x00); dat((uint8_t)(y & 0xFF));
    cmd(0x2C);
    digitalWrite(s_dc, HIGH);
    static uint8_t line[240 * 2];
    const uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)(color & 0xFF);
    for (int i = 0; i < 240; i++) { line[i * 2] = hi; line[i * 2 + 1] = lo; }
    SPI.writePixels(line, sizeof(line));
}

}  // namespace rawspi

void lcd_raw_probe() {
    s_uiPaused = true;                       // 停掉 LVGL，免得它把画面盖回来

    // 只有一轮：把 I2C 那颗 0x18 全部输出拉高（前面的试验里，正是这一步之后
    // 屏上才出现了亮带，说明它至少管着背光或 LCD 复位释放）
    i2c_write(0x18, 0x03, 0x00);
    i2c_write(0x18, 0x01, 0xFF);

    // 候选 DC 脚：原厂寄存器转储里只有 10 和 18 被配成普通输出，
    // 46 是第一轮刷出花屏时用的，一并留着做对照
    // 一色一脚，看到哪种颜色就知道是哪个脚，不用数轮次
    static const int  dcs[]  = {10, 18, 46};
    static const uint16_t col[] = {0xF800, 0x07E0, 0x001F};   // 红 / 绿 / 蓝
    static const char* nm[]  = {"红", "绿", "蓝"};
    const int N = 3;

    // 让 LGFX 先松开 SPI2，再自己接管 12/11
    SPI.end();
    delay(20);
    SPI.begin(PIN_LCD_SCLK, -1, PIN_LCD_MOSI, -1);
    SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

    Serial.println("\n===== DC 定位：一色一脚 =====");
    Serial.println("DC=GPIO10 → 红    DC=GPIO18 → 绿    DC=GPIO46 → 蓝   （各 5 秒）");

    for (int i = 0; i < N; i++) {
        rawspi::s_dc = dcs[i];
        pinMode(rawspi::s_dc, OUTPUT);
        digitalWrite(rawspi::s_dc, HIGH);
        delay(5);

        rawspi::st7789_init(true);
        rawspi::fill(0x0000); delay(300);
        rawspi::fill(col[i]);
        Serial.printf("[DC=%d] 显示 %s，持续 5 秒\n", dcs[i], nm[i]);
        delay(5000);

        rawspi::fill(0x0000); delay(500);
        pinMode(rawspi::s_dc, INPUT);   // 松手，避免两个脚同时抢 SPI
        delay(300);
    }

    Serial.println("\n===== 结束，屏停在纯黑 =====");
    Serial.println("看到的颜色：红=DC10  绿=DC18  蓝=DC46  什么都没看到=都不对");
}

// 把某个颜色「一直保持」在屏上，不设超时 —— 定时闪现人眼容易错过，
// 这样用户什么时候看都行，也能拿手电筒、换角度细看
void lcd_raw_paint(int dc, int cs, uint8_t r, uint8_t g, uint8_t b, bool frame,
                   int rst) {
    s_uiPaused = true;
    i2c_write(0x18, 0x03, 0x00);
    i2c_write(0x18, 0x01, 0xFF);

    // 复位脚：面板的 RST 可能是 GPIO，也可能悬空。软复位在硬复位有效时无效，
    // 所以先给一个低脉冲再拉高，把它彻底放开
    if (rst >= 0) {
        pinMode(rst, OUTPUT);
        digitalWrite(rst, LOW);
        delay(30);
        digitalWrite(rst, HIGH);
        delay(150);
        Serial.printf("[RAW] RST=GPIO%d 已做过低脉冲\n", rst);
    }

    // 片选：面板的 CS 未必接地，可能是普通 GPIO。不拉低的话它会一直忽略数据
    if (cs >= 0) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, LOW);
        Serial.printf("[RAW] CS=GPIO%d 已拉低\n", cs);
    }

    SPI.end();
    delay(20);
    SPI.begin(PIN_LCD_SCLK, -1, PIN_LCD_MOSI, -1);
    SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

    rawspi::s_dc = dc;
    pinMode(dc, OUTPUT);
    digitalWrite(dc, HIGH);
    delay(5);
    rawspi::st7789_init(true);

    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    rawspi::fill(0x0000);
    rawspi::fill(c);
    Serial.printf("[RAW] DC=%d 已填入 R=%u G=%u B=%u（保持不放）\n", dc, r, g, b);

    if (frame) {
        // 画一圈 10 像素黑边：能看见边就说明几何是对的、整屏都在刷
        for (int y = 0; y < 10; y++)       rawspi::fillRow(y, 0x0000);
        for (int y = 310; y < 320; y++)    rawspi::fillRow(y, 0x0000);
        Serial.println("[RAW] 已加 10px 黑边（上下）");
    }
}

// ============================================================
// 触摸两级验证
//   tp_raw  ：绕过 LovyanGFX，直接读芯片 0x02(点数) / 0x03~0x06(坐标)
//             用来判断「芯片有没有检测到手指」，与驱动无关。
//   tp_live ：走完整驱动链路，用来判断「驱动有没有把事件交给 LVGL」。
// 两者一起看就能定位问题在哪一层：
//   raw 有、live 没有  → 驱动/映射问题
//   raw 也没有          → 面板或 FPC 没接好（硬件层）
// ============================================================
void tp_raw_test(int seconds) {
#if HAS_TOUCH
    if (seconds <= 0) seconds = 15;
    wire_ensure();
#if defined(PIN_TP_IRQ) && PIN_TP_IRQ >= 0
    pinMode(PIN_TP_IRQ, INPUT_PULLUP);
#endif
    Serial.printf("\n[TPRAW] %d 秒内请按住屏幕不放（松开也算一次变化）\n", seconds);
    uint32_t t0 = millis();
    int lastPts = -1, lastInt = -1;
    while (millis() - t0 < (uint32_t)seconds * 1000) {
        uint8_t pts = 0;
        Wire.beginTransmission(0x38);
        Wire.write((uint8_t)0x02);
        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom((uint8_t)0x38, (uint8_t)1) == 1) {
            pts = Wire.read() & 0x0F;
        }
#if defined(PIN_TP_IRQ) && PIN_TP_IRQ >= 0
        int intLv = digitalRead(PIN_TP_IRQ);
#else
        int intLv = -1;
#endif
        if ((int)pts != lastPts || intLv != lastInt) {
            Serial.printf("[TPRAW] 点数=%u  INT=%d\n", pts, intLv);
            lastPts = pts; lastInt = intLv;
        }
        if (pts > 0) {
            Wire.beginTransmission(0x38);
            Wire.write((uint8_t)0x03);
            if (Wire.endTransmission(false) == 0 &&
                Wire.requestFrom((uint8_t)0x38, (uint8_t)4) == 4) {
                uint8_t b[4];
                for (int i = 0; i < 4; i++) b[i] = Wire.read();
                uint16_t x = (uint16_t)(((b[0] & 0x0F) << 8) | b[1]);
                uint16_t y = (uint16_t)(((b[2] & 0x0F) << 8) | b[3]);
                Serial.printf("[TPRAW]   x=%u y=%u  (点位 %u)\n", x, y, pts);
            }
        }
        delay(100);
    }
    Serial.println("[TPRAW] 结束");
#else
    Serial.println("[TPRAW] 当前配置没有触摸，忽略");
#endif
}

void tp_live_test(int seconds) {
#if HAS_TOUCH
    if (seconds <= 0) seconds = 15;
    Serial.printf("\n[TPLV] %d 秒内请点/按屏幕，这里走完整驱动链路\n", seconds);
    uint32_t t0 = millis();
    int hits = 0;
    while (millis() - t0 < (uint32_t)seconds * 1000) {
        uint16_t x = 0, y = 0;
        if (s_lcd.getTouch(&x, &y)) {
            hits++;
            Serial.printf("[TPLV] x=%u y=%u\n", x, y);
        }
        delay(70);
    }
    Serial.printf("[TPLV] 结束，共 %d 次有效触摸\n", hits);
#else
    Serial.println("[TPLV] 当前配置没有触摸，忽略");
#endif
}

// 运行时切换触摸轴补偿：0~3 分别对应四种方向组合。
// 让用户现场试，不用为了一个数字反复烧录。
void touch_set_rotation(int r) {
    r &= 3;
    s_lcd.setTouchRotation((uint8_t)r);
    uint8_t eff = (uint8_t)((LCD_ROTATION + r) & 3);
    Serial.printf("[TPROT] 触摸offset=%d  等效r=%d  (", r, eff);
    if (eff & 1) Serial.print("交换XY ");
    if (eff & 2) Serial.print("X翻转 ");
    bool vf = (1 << eff) & 0b10010110;
    if (vf) Serial.print("Y翻转 ");
    Serial.println(")");
    Serial.println("[TPROT] 请点左上角确认是否落在左上角");
}

// ============================================================
// 颜色校验页（一屏看完）
//   上 2/3：八个纯色块，每块写自己的英文名
//           RED 显示成蓝 → 红蓝互换(RGB/BGR)；全变底片 → 反色
//   下 1/3：32 级灰阶，判断是不是只有黑白没有过渡
// 用 LGFX 直接画、并暂停 LVGL，这样画面不会被界面盖回去
// ============================================================
void color_test_page() {
    s_uiPaused = true;
    delay(30);
    auto& g = s_lcd;
    const int W = g.width();
    const int H = g.height();

    struct Swatch { uint16_t c; const char* n; bool darkText; };
    const Swatch tbl[8] = {
        {0xF800, "RED",     false},
        {0x07E0, "GREEN",   false},
        {0x001F, "BLUE",    false},
        {0xFFFF, "WHITE",   true },
        {0x07FF, "CYAN",    true },
        {0xF81F, "MAGENTA", true },
        {0xFFE0, "YELLOW",  true },
        {0x0000, "BLACK",   false},
    };

    const int gh   = 34;              // 灰阶条高度
    const int areaH = H - gh;
    const int cw   = W / 4;
    const int ch   = areaH / 2;

    g.startWrite();
    for (int i = 0; i < 8; i++) {
        int x = (i % 4) * cw;
        int y = (i / 4) * ch;
        int w = (i % 4 == 3) ? (W - x) : cw;   // 最右列补齐，避免留缝
        int h = (i / 4 == 1) ? (areaH - y) : ch;
        g.fillRect(x, y, w, h, tbl[i].c);
        g.setTextDatum(middle_center);
        g.setTextSize(2);
        g.setTextColor(tbl[i].darkText ? 0x0000 : 0xFFFF, tbl[i].c);
        g.drawString(tbl[i].n, x + w / 2, y + h / 2);
    }
    // 32 级灰阶
    for (int i = 0; i < 32; i++) {
        uint8_t v = (uint8_t)(i * 255 / 31);
        uint16_t c = (uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));
        int x = i * W / 32;
        int w = (i == 31) ? (W - x) : (W / 32 + 1);
        g.fillRect(x, H - gh, w, gh, c);
    }
    g.endWrite();

    Serial.println("[COLOR] 色块页已显示：");
    Serial.println("  上排 RED GREEN BLUE WHITE / 下排 CYAN MAGENTA YELLOW BLACK");
    Serial.println("  底部 32 级灰阶。看完发 uiresume 恢复界面");
}

// ============================================================
// 反色开关（运行时可切）
//   ILI9341 的 INVON/INVOFF 直接反相整个面板。
//   颜色看着像底片、红变青、白变黑 → 切一下就知道对不对。
// ============================================================
// 反色状态存 NVS：点按钮调到对的那个，以后每次启动都按它来，
// 重新烧固件也不会丢（除非整片擦除）。
static void invert_save(bool on) {
    Preferences p;
    p.begin("cpb", false);
    p.putBool("inv", on);
    p.end();
}

static bool invert_load() {
    Preferences p;
    p.begin("cpb", true);
    bool has = p.isKey("inv");
    bool v = has ? p.getBool("inv") : (LCD_INVERT_COLOR != 0);
    p.end();
    Serial.printf("[LCD] 反色设定: %s%s\n", v ? "开" : "关", has ? "（NVS已存）" : "（默认值）");
    return v;
}

void display_set_invert(bool on) {
    s_invertOn = on;
    s_lcd.invertDisplay(on);
    invert_save(on);
    Serial.printf("[LCD] 反色 = %s（已记住）\n", on ? "开" : "关");
}

bool display_get_invert() { return s_invertOn; }

void display_toggle_invert() { display_set_invert(!s_invertOn); }

// 取触摸统计（测试页显示用）
void display_touch_stat(uint16_t* x, uint16_t* y, uint32_t* n) {
    if (x) *x = s_tpX;
    if (y) *y = s_tpY;
    if (n) *n = s_tpCount;
}

void display_touch_stat_reset() {
    s_tpX = 0xFFFF; s_tpY = 0xFFFF; s_tpCount = 0;
}
