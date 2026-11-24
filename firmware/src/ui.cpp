#include "ui.h"
#include "app_state.h"
#include "board_profile.h"
#include "display_port.h"

#include <Arduino.h>
#include <lvgl.h>

LV_FONT_DECLARE(lv_font_simsun_16_cjk);
// 定制黑体：由 simhei.ttf 生成，覆盖本项目全部用到的汉字。
// LVGL 自带的 simsun_16_cjk 只有约一千常用字，像"换""幕"这类字会显示成方框。
LV_FONT_DECLARE(font_zh_16);
// 定制黑体：由 simhei.ttf 生成，覆盖本项目全部用到的汉字。
// LVGL 自带的 simsun_16_cjk 只有约一千常用字，像"换""幕"这类字会显示成方框。
LV_FONT_DECLARE(font_zh_16);

// ---------------- 配色 ----------------
#define C_BG       0x121316
#define C_CARD     0x232529
#define C_TRACK    0x33373C
#define C_TEXT     0xE8EAED
#define C_DIM      0x9AA0A6
#define C_GREEN    0x97C459
#define C_BLUE     0x85B7EB
#define C_RED      0xF09595
#define C_AMBER    0xFAC775
#define C_LINE     0x2E3134

#define F_ZH   (&font_zh_16)
#define F_N20  (&lv_font_montserrat_20)
#define F_N28  (&lv_font_montserrat_28)

// ---------------- 控件句柄 ----------------
static lv_obj_t* s_scrMain   = nullptr;
static lv_obj_t* s_scrSet    = nullptr;

static lv_obj_t* s_lblIp      = nullptr;
static lv_obj_t* s_lblBle     = nullptr;
static lv_obj_t* s_lblState   = nullptr;
static lv_obj_t* s_lblPct     = nullptr;
static lv_obj_t* s_lblSub     = nullptr;
static lv_obj_t* s_barProg    = nullptr;
static lv_obj_t* s_lblTotal   = nullptr;
static lv_obj_t* s_lblOk      = nullptr;
static lv_obj_t* s_lblFail    = nullptr;
static lv_obj_t* s_histBox    = nullptr;
static lv_obj_t* s_histTime[HIST_MAX];
static lv_obj_t* s_histBytes[HIST_MAX];
static lv_obj_t* s_histRes[HIST_MAX];

static lv_obj_t* s_lblDev     = nullptr;
static lv_obj_t* s_lblAgent   = nullptr;
static lv_obj_t* s_lblBright  = nullptr;
static lv_obj_t* s_swFwd     = nullptr;

// ---------------- 上次值（避免无变化还重绘） ----------------
static uint32_t s_lastTotal = 0xFFFFFFFF;
static uint32_t s_lastOk    = 0xFFFFFFFF;
static uint32_t s_lastFail  = 0xFFFFFFFF;
static uint32_t s_lastBytes = 0xFFFFFFFF;
static uint8_t  s_lastPct   = 0xFF;
static uint8_t  s_lastPhase = 0xFF;
static bool     s_lastWifi  = false;
static bool     s_lastBle   = false;
static uint8_t  s_lastHist  = 0xFF;
static uint8_t  s_lastHistCount = 0xFF;

// ============================================================
// 小工具
// ============================================================
static lv_obj_t* mkBox(lv_obj_t* parent, uint32_t bg, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 8, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t* mkLabel(lv_obj_t* parent, const lv_font_t* f, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void styleBtn(lv_obj_t* b) {
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(b, F_ZH, 0);
    // 按下时的过渡（LVGL 自带样式过渡，不用写动画）
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TRACK), LV_STATE_PRESSED);
}

static const char* phaseText(Phase p) {
    switch (p) {
        case PH_RX:   return "接收中";
        case PH_TX:   return "转发中";
        case PH_DONE: return "已完成";
        case PH_FAIL: return "失败";
        default:      return "等待连接";
    }
}

static uint32_t phaseColor(Phase p) {
    switch (p) {
        case PH_RX:   return C_BLUE;
        case PH_TX:   return C_AMBER;
        case PH_DONE: return C_GREEN;
        case PH_FAIL: return C_RED;
        default:      return C_DIM;
    }
}

// ============================================================
// 主界面
// ============================================================
static void ev_goSettings(lv_event_t* e) {
    lv_scr_load_anim(s_scrSet, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void ev_cycleName(lv_event_t* e) {
    app_cycle_devname();
}

static void ev_back(lv_event_t* e) {
    lv_scr_load_anim(s_scrMain, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void ev_fwd(lv_event_t* e) {
    app_set_forward(lv_obj_has_state(s_swFwd, LV_STATE_CHECKED));
}

static void ev_bright(lv_event_t* e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    display_setBrightness((uint8_t)(v * 255 / 100));
    lv_label_set_text_fmt(s_lblBright, "%d%%", v);
}

static void ev_reconnect(lv_event_t* e) { app_reconnect_wifi(); }
static void ev_reboot(lv_event_t* e)    { app_reboot(); }

static void buildMain() {
    s_scrMain = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scrMain, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scrMain, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(s_scrMain, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_scrMain, 8, 0);
    lv_obj_set_style_pad_row(s_scrMain, 6, 0);

    // ---- 顶栏 ----
    {
        lv_obj_t* h = mkBox(s_scrMain, C_BG, 0);
        lv_obj_set_width(h, LV_PCT(100));
        lv_obj_set_height(h, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(h, 0, 0);
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_lblIp  = mkLabel(h, F_ZH, C_DIM);
        s_lblBle = mkLabel(h, F_ZH, C_DIM);
        lv_label_set_text(s_lblIp, "WiFi 连接中");
        lv_label_set_text(s_lblBle, "BLE 待机");
    }

    // ---- 当前任务卡片 ----
    {
        lv_obj_t* card = mkBox(s_scrMain, C_CARD, 10);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, lv_pct(30));
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);

        lv_obj_t* row = mkBox(card, C_CARD, 0);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

        s_lblState = mkLabel(row, F_ZH, C_DIM);
        s_lblPct   = mkLabel(row, F_N20, C_BLUE);
        lv_label_set_text(s_lblState, "等待连接");
        lv_label_set_text(s_lblPct, "");

        s_barProg = lv_bar_create(card);
        lv_obj_set_width(s_barProg, LV_PCT(100));
        lv_obj_set_height(s_barProg, 8);
        lv_bar_set_range(s_barProg, 0, 100);
        lv_bar_set_value(s_barProg, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_barProg, lv_color_hex(C_TRACK), LV_PART_MAIN);
        lv_obj_set_style_radius(s_barProg, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_barProg, lv_color_hex(C_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_barProg, 4, LV_PART_INDICATOR);

        s_lblSub = mkLabel(card, F_ZH, C_DIM);
        lv_label_set_text(s_lblSub, "手机连上蓝牙后点打印");
    }

    // ---- 今日统计 ----
    {
        lv_obj_t* stats = mkBox(s_scrMain, C_BG, 0);
        lv_obj_set_width(stats, LV_PCT(100));
        lv_obj_set_height(stats, 44);
        lv_obj_set_style_pad_all(stats, 0, 0);
        lv_obj_set_style_pad_column(stats, 6, 0);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);

        struct { const char* cap; lv_obj_t** val; uint32_t col; } items[3] = {
            {"今日", &s_lblTotal, C_TEXT},
            {"成功", &s_lblOk,    C_GREEN},
            {"失败", &s_lblFail,  C_RED},
        };
        for (int i = 0; i < 3; i++) {
            lv_obj_t* c = mkBox(stats, C_CARD, 8);
            lv_obj_set_flex_grow(c, 1);
            lv_obj_set_height(c, LV_PCT(100));
            lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(c, 5, 0);
            lv_obj_set_style_pad_row(c, 0, 0);

            lv_obj_t* cap = mkLabel(c, F_ZH, C_DIM);
            lv_label_set_text(cap, items[i].cap);
            *items[i].val = mkLabel(c, F_N20, items[i].col);
            lv_label_set_text(*items[i].val, "0");
        }
    }

    // ---- 最近任务 ----
    {
        s_histBox = mkBox(s_scrMain, C_CARD, 10);
        lv_obj_set_width(s_histBox, LV_PCT(100));
        lv_obj_set_flex_grow(s_histBox, 1);
        lv_obj_set_flex_flow(s_histBox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_histBox, 2, 0);
        lv_obj_set_style_pad_all(s_histBox, 6, 0);

        for (int i = 0; i < HIST_MAX; i++) {
            lv_obj_t* r = mkBox(s_histBox, C_CARD, 0);
            lv_obj_set_width(r, LV_PCT(100));
            lv_obj_set_height(r, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_all(r, 0, 0);
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            s_histTime[i]  = mkLabel(r, F_ZH, C_DIM);
            s_histBytes[i] = mkLabel(r, F_ZH, C_TEXT);
            s_histRes[i]   = mkLabel(r, F_ZH, C_DIM);
            lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- 底部按钮 ----
    {
        lv_obj_t* b = mkBox(s_scrMain, C_BG, 0);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, 30);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_pad_column(b, 6, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);

        lv_obj_t* b1 = lv_btn_create(b);
        lv_obj_set_flex_grow(b1, 1);
        lv_obj_set_height(b1, LV_PCT(100));
        styleBtn(b1);
        lv_obj_t* t1 = mkLabel(b1, F_ZH, C_DIM);
        lv_label_set_text(t1, "切设备名");
        lv_obj_center(t1);
        lv_obj_add_event_cb(b1, ev_cycleName, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* b2 = lv_btn_create(b);
        lv_obj_set_flex_grow(b2, 1);
        lv_obj_set_height(b2, LV_PCT(100));
        styleBtn(b2);
        lv_obj_t* t2 = mkLabel(b2, F_ZH, C_DIM);
        lv_label_set_text(t2, "设置");
        lv_obj_center(t2);
        lv_obj_add_event_cb(b2, ev_goSettings, LV_EVENT_CLICKED, nullptr);
    }
}

// ============================================================
// 设置页
// ============================================================
static lv_obj_t* mkRow(lv_obj_t* parent) {
    lv_obj_t* r = mkBox(parent, C_CARD, 8);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, 44);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(r, 8, 0);
    return r;
}

static void buildSettings() {
    s_scrSet = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scrSet, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scrSet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_scrSet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_scrSet, 8, 0);
    lv_obj_set_style_pad_row(s_scrSet, 6, 0);

    // 标题栏
    {
        lv_obj_t* h = mkBox(s_scrSet, C_BG, 0);
        lv_obj_set_width(h, LV_PCT(100));
        lv_obj_set_height(h, 26);
        lv_obj_set_style_pad_all(h, 0, 0);
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(h, 6, 0);

        lv_obj_t* back = lv_btn_create(h);
        lv_obj_set_size(back, 34, 26);
        styleBtn(back);
        lv_obj_t* bt = mkLabel(back, F_ZH, C_TEXT);
        lv_label_set_text(bt, "<");
        lv_obj_center(bt);
        lv_obj_add_event_cb(back, ev_back, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* t = mkLabel(h, F_ZH, C_TEXT);
        lv_label_set_text(t, "设置");
    }

    // WiFi
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_TEXT);
        lv_label_set_text(l, "WiFi");
        lv_obj_t* v = mkLabel(r, F_ZH, C_GREEN);
        lv_label_set_text(v, "点击重连");
        lv_obj_add_event_cb(r, ev_reconnect, LV_EVENT_CLICKED, nullptr);
    }

    // 设备名
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_TEXT);
        lv_label_set_text(l, "蓝牙设备名");
        s_lblDev = mkLabel(r, F_ZH, C_BLUE);
        lv_label_set_text(s_lblDev, "HM-A300");
        lv_obj_add_event_cb(r, ev_cycleName, LV_EVENT_CLICKED, nullptr);
    }

    // 代理
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_TEXT);
        lv_label_set_text(l, "电脑代理");
        s_lblAgent = mkLabel(r, F_ZH, C_GREEN);
        lv_label_set_text(s_lblAgent, app_agent_str().c_str());
    }

    // 自动转发
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_TEXT);
        lv_label_set_text(l, "自动转发");

        s_swFwd = lv_switch_create(r);
        lv_obj_set_size(s_swFwd, 40, 22);
        lv_obj_set_style_bg_color(s_swFwd, lv_color_hex(C_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_swFwd, lv_color_hex(C_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (app_get_forward()) lv_obj_add_state(s_swFwd, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_swFwd, ev_fwd, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // 亮度
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_TEXT);
        lv_label_set_text(l, "亮度");

        lv_obj_t* holder = mkBox(r, C_CARD, 0);
        lv_obj_set_height(holder, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(holder, 0, 0);
        lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(holder, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(holder, 6, 0);

        s_lblBright = mkLabel(holder, F_ZH, C_DIM);
        lv_label_set_text(s_lblBright, "78%");

        lv_obj_t* sl = lv_slider_create(holder);
        lv_obj_set_width(sl, 110);
        lv_obj_set_height(sl, 18);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, display_getBrightness() * 100 / 255, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_DIM), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
        lv_obj_add_event_cb(sl, ev_bright, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // 版本 + 重启
    {
        lv_obj_t* r = mkRow(s_scrSet);
        lv_obj_t* l = mkLabel(r, F_ZH, C_DIM);
        lv_label_set_text(l, "云打印盒 v1.1 · S3");

        lv_obj_t* b = lv_btn_create(r);
        lv_obj_set_size(b, 76, 26);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x6B3030), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_t* bt = mkLabel(b, F_ZH, C_RED);
        lv_label_set_text(bt, "长按重启");
        lv_obj_center(bt);
        // 长按才重启：以前是单击，手指稍微偏一点就落到这个按钮上，
        // 看起来就是"点着点着设备自己重启了"。
        lv_obj_add_event_cb(b, ev_reboot, LV_EVENT_LONG_PRESSED, nullptr);
    }
}

// ============================================================
// 开机自检页
//   屏和引脚对了 → 能看到「云打印盒」和分辨率，点一下进主界面
//   白屏/黑屏      → 引脚或驱动型号不对，看串口的 [LCD] 那行
// ============================================================
static lv_obj_t* s_scrSplash = nullptr;

static void buildColorPage();

static void splash_go(lv_timer_t* t) {
    if (lv_scr_act() == s_scrSplash) {
        lv_scr_load_anim(s_scrMain, LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
    }
    lv_timer_del(t);
}

static void splash_tap(lv_event_t* e) {
    lv_scr_load_anim(s_scrMain, LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
}

static void buildSplash() {
    s_scrSplash = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scrSplash, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_scrSplash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrSplash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_scrSplash, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scrSplash, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scrSplash, 10, 0);

    lv_obj_t* t1 = mkLabel(s_scrSplash, F_ZH, C_TEXT);
    lv_label_set_text(t1, "云打印盒");

    lv_obj_t* t2 = mkLabel(s_scrSplash, F_ZH, C_GREEN);
    lv_label_set_text_fmt(t2, "屏自检 OK · %dx%d", SCREEN_W, SCREEN_H);

    lv_obj_t* t3 = mkLabel(s_scrSplash, F_ZH, C_DIM);
    lv_label_set_text(t3, "点一下屏幕继续");

    lv_obj_add_event_cb(s_scrSplash, splash_tap, LV_EVENT_CLICKED, nullptr);
}

// ============================================================
// 对外
// ============================================================
void ui_init() {
    buildSplash();
    buildMain();
    buildSettings();
    buildColorPage();
    lv_scr_load(s_scrSplash);
    lv_timer_create(splash_go, 3000, nullptr);   // 没人点就 3 秒后自动进
}

void ui_update() {
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    if (now - lastMs < 100) return;      // 10Hz 刷新，别跟转发抢 CPU
    lastMs = now;

    AppState s = state_snapshot();

    // 顶栏
    if (s.wifiUp != s_lastWifi) {
        s_lastWifi = s.wifiUp;
        if (s.wifiUp) {
            lv_label_set_text_fmt(s_lblIp, "WiFi %s", s.ip);
            lv_obj_set_style_text_color(s_lblIp, lv_color_hex(C_GREEN), 0);
        } else {
            lv_label_set_text(s_lblIp, "WiFi 未连");
            lv_obj_set_style_text_color(s_lblIp, lv_color_hex(C_RED), 0);
        }
    }
    if (s.bleConn != s_lastBle) {
        s_lastBle = s.bleConn;
        lv_label_set_text(s_lblBle, s.bleConn ? "BLE 已连" : "BLE 待机");
        lv_obj_set_style_text_color(s_lblBle,
                                    lv_color_hex(s.bleConn ? C_BLUE : C_DIM), 0);
    }

    // 状态 + 进度
    if ((uint8_t)s.phase != s_lastPhase) {
        s_lastPhase = (uint8_t)s.phase;
        lv_label_set_text(s_lblState, phaseText(s.phase));
        lv_obj_set_style_text_color(s_lblState, lv_color_hex(phaseColor(s.phase)), 0);
        lv_obj_set_style_bg_color(s_barProg, lv_color_hex(phaseColor(s.phase)),
                                  LV_PART_INDICATOR);
        // 换个状态时让进度条平滑滑过去，而不是跳
        if (s.phase == PH_IDLE) lv_bar_set_value(s_barProg, 0, LV_ANIM_ON);
    }

    if (s.phase == PH_RX || s.phase == PH_TX) {
        if (s.percent != s_lastPct) {
            s_lastPct = s.percent;
            if (s.percent) {
                lv_label_set_text_fmt(s_lblPct, "%d%%", s.percent);
                lv_bar_set_value(s_barProg, s.percent, LV_ANIM_ON);
            } else {
                // 分段转发时不知道总量，不猜百分比，只报已收字节
                lv_label_set_text(s_lblPct, "");
                lv_bar_set_value(s_barProg, 0, LV_ANIM_OFF);
            }
        }
        if (s.rxBytes != s_lastBytes) {
            s_lastBytes = s.rxBytes;
            if (s.rxExpect) {
                lv_label_set_text_fmt(s_lblSub, "%.1f / %.1f KB",
                                      s.rxBytes / 1024.0, s.rxExpect / 1024.0);
            } else {
                lv_label_set_text_fmt(s_lblSub, "已收 %.1f KB", s.rxBytes / 1024.0);
            }
        }
    } else if (s.phase == PH_DONE) {
        lv_label_set_text_fmt(s_lblSub, "%.1f KB · 已存 PDF", s.rxBytes / 1024.0);
        lv_label_set_text(s_lblPct, "");
    } else if (s.phase == PH_FAIL) {
        lv_label_set_text(s_lblSub, "检查电脑端代理是否在跑");
        lv_label_set_text(s_lblPct, "");
    } else {
        lv_label_set_text(s_lblSub, "手机连上蓝牙后点打印");
        lv_label_set_text(s_lblPct, "");
    }

    // 统计
    if (s.todayTotal != s_lastTotal) {
        s_lastTotal = s.todayTotal;
        lv_label_set_text_fmt(s_lblTotal, "%lu", (unsigned long)s.todayTotal);
    }
    if (s.todayOk != s_lastOk) {
        s_lastOk = s.todayOk;
        lv_label_set_text_fmt(s_lblOk, "%lu", (unsigned long)s.todayOk);
    }
    if (s.todayFail != s_lastFail) {
        s_lastFail = s.todayFail;
        lv_label_set_text_fmt(s_lblFail, "%lu", (unsigned long)s.todayFail);
    }

    // 历史：histCount 到上限后就不再变，所以用 histHead 判断有没有新记录
    if (s.histHead != s_lastHist || s.histCount != s_lastHistCount) {
        s_lastHist = s.histHead;
        s_lastHistCount = s.histCount;
        for (int i = 0; i < HIST_MAX; i++) {
            lv_obj_t* row = lv_obj_get_parent(s_histTime[i]);
            if (i >= s.histCount) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            // 最新的排最上面
            int idx = (s.histHead - 1 - i + HIST_MAX * 2) % HIST_MAX;
            JobRecord& r = s.hist[idx];
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);

            if (r.hh == 0xFF) lv_label_set_text(s_histTime[i], "--:--");
            else              lv_label_set_text_fmt(s_histTime[i], "%02d:%02d", r.hh, r.mm);

            lv_label_set_text_fmt(s_histBytes[i], "%.1f KB", r.bytes / 1024.0);

            const char* txt = "已转发";
            uint32_t col = C_GREEN;
            if (r.result == JR_EMPTY) { txt = "空包丢弃"; col = C_DIM; }
            else if (r.result == JR_FAIL) { txt = "失败"; col = C_RED; }
            else if (r.result == JR_DROP) { txt = "超长截断"; col = C_AMBER; }
            lv_label_set_text(s_histRes[i], txt);
            lv_obj_set_style_text_color(s_histRes[i], lv_color_hex(col), 0);
        }
    }

    // 设置页上的设备名
    if (s_lblDev) {
        static char last[20] = {0};
        if (strcmp(last, s.devName) != 0) {
            strncpy(last, s.devName, sizeof(last) - 1);
            lv_label_set_text(s_lblDev, s.devName);
        }
    }
}

// ============================================================
// 屏幕测试页（全屏单色 + 中央色名，点屏幕换下一屏）
//   一屏只有一种颜色，中间写着它自己叫什么。
//   不用赶时间、不用对比、不用记 —— 看多久都行。
//   判断：
//     中间写着 RED，满屏就是红        → 颜色正常
//     写着 RED，满屏是蓝              → 红蓝通道互换
//     写着 WHITE，满屏是黑            → 反色了
//     名字和颜色都对，但灰阶段断成两截 → 位深不对
//   第 9 屏是灰阶。点右下角「返回」回主界面。
// ============================================================
static lv_obj_t* s_scrColor = nullptr;
static lv_obj_t* s_colName  = nullptr;
static lv_obj_t* s_colSub   = nullptr;
static lv_obj_t* s_colTip   = nullptr;
static lv_obj_t* s_colBack  = nullptr;
static lv_obj_t* s_colInv   = nullptr;
static lv_obj_t* s_colTP    = nullptr;
static lv_obj_t* s_colGray  = nullptr;
static int       s_colIdx   = 0;

struct ColorItem { uint32_t hex; const char* en; const char* cn; };

static const ColorItem kColors[8] = {
    {0xFF0000, "RED",     "红"},
    {0x00FF00, "GREEN",   "绿"},
    {0x0000FF, "BLUE",    "蓝"},
    {0xFFFFFF, "WHITE",   "白"},
    {0x000000, "BLACK",   "黑"},
    {0x00FFFF, "CYAN",    "青"},
    {0xFF00FF, "MAGENTA", "品红"},
    {0xFFFF00, "YELLOW",  "黄"},
};
#define COLOR_PAGES 9          // 8 个纯色 + 1 个灰阶

// 按背景亮度挑字色，保证任何底色上都看得清
static uint32_t contrastOn(uint32_t hex) {
    uint32_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
    uint32_t lum = (r * 299 + g * 587 + b * 114) / 1000;
    return (lum > 128) ? 0x101010 : 0xFFFFFF;
}

static void ui_color_sync_inv_btn(void) {
    if (!s_colInv) return;
    lv_obj_t* l = lv_obj_get_child(s_colInv, 0);
    if (l) lv_label_set_text(l, display_get_invert() ? "反色:开" : "反色:关");
}

static void ev_colorInvert(lv_event_t* e) {
    display_toggle_invert();
    ui_color_sync_inv_btn();
}

static void ev_colorTap(lv_event_t* e) {
    s_colIdx = (s_colIdx + 1) % COLOR_PAGES;
    ui_color_apply(s_colIdx);
}

static void ev_colorBack(lv_event_t* e) {
    lv_scr_load_anim(s_scrMain, LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
}

// 每 150ms 把触摸坐标贴到屏幕上。
// 左上角应该是 (0,0)，右下角应该接近 (319,239)；
// 点哪儿数和位置对不上，就是映射要调。
static void color_tp_timer(lv_timer_t* t) {
    if (!s_colTP || lv_scr_act() != s_scrColor) return;
    uint16_t x = 0, y = 0;
    uint32_t n = 0;
    display_touch_stat(&x, &y, &n);
    static uint32_t lastN = 0xFFFFFFFF;
    static uint16_t lastX = 0xFFFF, lastY = 0xFFFF;
    if (n == lastN && x == lastX && y == lastY) return;   // 没变化不重绘
    lastN = n; lastX = x; lastY = y;
    if (n == 0) lv_label_set_text(s_colTP, "还没摸到 · 点一下屏幕");
    else        lv_label_set_text_fmt(s_colTP, "x=%u  y=%u   第%u次",
                                      (unsigned)x, (unsigned)y, (unsigned)n);
}

static void buildColorPage() {
    s_scrColor = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_scrColor, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scrColor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_scrColor, 0, 0);
    lv_obj_set_style_border_width(s_scrColor, 0, 0);
    lv_obj_add_flag(s_scrColor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrColor, ev_colorTap, LV_EVENT_CLICKED, nullptr);

    // 顶部操作提示
    s_colTip = lv_label_create(s_scrColor);
    lv_obj_set_style_text_font(s_colTip, F_ZH, 0);
    lv_label_set_text(s_colTip, "点屏幕换下一色");
    lv_obj_align(s_colTip, LV_ALIGN_TOP_MID, 0, 6);

    // 触摸坐标实时显示
    s_colTP = lv_label_create(s_scrColor);
    lv_obj_set_style_text_font(s_colTP, F_ZH, 0);
    lv_label_set_text(s_colTP, "还没摸到 · 点一下屏幕");
    lv_obj_align(s_colTP, LV_ALIGN_TOP_MID, 0, 24);
    lv_timer_create(color_tp_timer, 150, nullptr);

    // 中央：颜色名，用大号字，一眼看清
    s_colName = lv_label_create(s_scrColor);
    lv_obj_set_style_text_font(s_colName, F_N28, 0);
    lv_label_set_text(s_colName, "RED");
    lv_obj_align(s_colName, LV_ALIGN_CENTER, 0, -14);

    // 中央下方：中文名 + 第几屏
    s_colSub = lv_label_create(s_scrColor);
    lv_obj_set_style_text_font(s_colSub, F_ZH, 0);
    lv_label_set_text(s_colSub, "红 · 1/9");
    lv_obj_align(s_colSub, LV_ALIGN_CENTER, 0, 22);

    // 灰阶段用的 16 根灰条，默认隐藏
    s_colGray = lv_obj_create(s_scrColor);
    lv_obj_clear_flag(s_colGray, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_colGray, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_colGray, 320, 56);
    lv_obj_align(s_colGray, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(s_colGray, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_colGray, 0, 0);
    lv_obj_set_style_pad_all(s_colGray, 0, 0);
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 255 / 15);
        lv_obj_t* g = lv_obj_create(s_colGray);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(g, 20, 56);
        lv_obj_set_pos(g, i * 20, 0);
        lv_obj_set_style_radius(g, 0, 0);
        lv_obj_set_style_border_width(g, 0, 0);
        lv_obj_set_style_pad_all(g, 0, 0);
        lv_obj_set_style_bg_color(g, lv_color_make(v, v, v), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(s_colGray, LV_OBJ_FLAG_HIDDEN);

    // 反色开关：看着像底片就点它，立即生效，不用重烧
    s_colInv = lv_btn_create(s_scrColor);
    lv_obj_set_size(s_colInv, 72, 24);
    lv_obj_align(s_colInv, LV_ALIGN_BOTTOM_RIGHT, -60, -4);
    lv_obj_set_style_bg_opa(s_colInv, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_colInv, 1, 0);
    lv_obj_set_style_radius(s_colInv, 6, 0);
    lv_obj_add_event_cb(s_colInv, ev_colorInvert, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t* il = lv_label_create(s_colInv);
        lv_obj_set_style_text_font(il, F_ZH, 0);
        lv_label_set_text(il, "反色:开");
        lv_obj_center(il);
    }

    // 右下角返回
    s_colBack = lv_btn_create(s_scrColor);
    lv_obj_set_size(s_colBack, 52, 24);
    lv_obj_align(s_colBack, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_set_style_bg_opa(s_colBack, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_colBack, 1, 0);
    lv_obj_set_style_radius(s_colBack, 6, 0);
    lv_obj_add_event_cb(s_colBack, ev_colorBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = lv_label_create(s_colBack);
    lv_obj_set_style_text_font(bl, F_ZH, 0);
    lv_label_set_text(bl, "返回");
    lv_obj_center(bl);
}

// 切到第 idx 屏
void ui_color_apply(int idx) {
    s_colIdx = (idx + COLOR_PAGES) % COLOR_PAGES;

    if (s_colIdx >= 8) {
        // 灰阶屏
        lv_obj_set_style_bg_color(s_scrColor, lv_color_black(), 0);
        lv_obj_clear_flag(s_colGray, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_colName, lv_color_white(), 0);
        lv_label_set_text(s_colName, "GRAY");
        lv_obj_align(s_colName, LV_ALIGN_BOTTOM_MID, 0, -46);
        lv_obj_set_style_text_color(s_colSub, lv_color_white(), 0);
        lv_label_set_text(s_colSub, "灰阶：应平滑过渡 · 9/9");
        lv_obj_align(s_colSub, LV_ALIGN_BOTTOM_MID, 0, -24);
        lv_obj_set_style_text_color(s_colTip, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_colBack, 0), lv_color_white(), 0);
        lv_obj_set_style_border_color(s_colInv, lv_color_white(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_colInv, 0), lv_color_white(), 0);
        lv_obj_set_style_text_color(s_colTP, lv_color_white(), 0);
        ui_color_sync_inv_btn();
        return;
    }

    const ColorItem& c = kColors[s_colIdx];
    uint32_t fg = contrastOn(c.hex);
    lv_obj_set_style_bg_color(s_scrColor, lv_color_hex(c.hex), 0);
    lv_obj_add_flag(s_colGray, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_text_color(s_colName, lv_color_hex(fg), 0);
    lv_label_set_text(s_colName, c.en);
    lv_obj_align(s_colName, LV_ALIGN_CENTER, 0, -14);

    lv_obj_set_style_text_color(s_colSub, lv_color_hex(fg), 0);
    lv_label_set_text_fmt(s_colSub, "%s · %d/9", c.cn, s_colIdx + 1);
    lv_obj_align(s_colSub, LV_ALIGN_CENTER, 0, 22);

    lv_obj_set_style_text_color(s_colTip, lv_color_hex(fg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_colBack, 0), lv_color_hex(fg), 0);
    lv_obj_set_style_border_color(s_colInv, lv_color_hex(fg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_colInv, 0), lv_color_hex(fg), 0);
    lv_obj_set_style_text_color(s_colTP, lv_color_hex(fg), 0);
    ui_color_sync_inv_btn();
}

void ui_show_color_page() {
    if (!s_scrColor) return;
    ui_color_apply(0);
    lv_scr_load(s_scrColor);
    Serial.println("[UI] 屏幕测试页：全屏单色+色名，点屏幕换下一色，右下角返回");
}

// LVGL 自己的内存池用量。池耗尽时 LVGL 会返回 NULL，
// 后面再对 NULL 控件设属性就是一片 PANIC —— 界面上"点着点着就重启"常见于此。
void ui_mem_report() {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("[MEM] LVGL池 总%u 已用%u%% 空闲%u 最大连续块%u 碎片%u%% | ESP空闲堆%u\n",
                  (unsigned)m.total_size, (unsigned)m.used_pct,
                  (unsigned)m.free_size, (unsigned)m.free_biggest_size, (unsigned)m.frag_pct,
                  (unsigned)ESP.getFreeHeap());
    Serial.printf("[MEM] 当前页子控件数 %u\n", (unsigned)lv_obj_get_child_cnt(lv_scr_act()));
}
