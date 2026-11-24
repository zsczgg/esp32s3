/**
 * 屏幕 + 触摸 + LVGL 对接层
 * 引脚全部来自 board_profile.h，这里不写死任何 GPIO
 */
#pragma once
#include <stdint.h>

// 初始化屏（在 setup 里最先调用，越早越好，能早点看到东西）
bool display_init();

// 初始化 LVGL 并挂上显示/触摸驱动。display_init() 成功后调用
void display_lvgl_init();

// 每帧调用（放在 loop 里，跑在 core 1，不抢蓝牙/WiFi 的核）
void display_task();

void    display_setBrightness(uint8_t v);   // 0~255
uint8_t display_getBrightness();
void    display_sleep(bool sleep);          // 熄屏省电

// 诊断用：屏和触摸是否初始化成功（界面会显示出来）
bool display_ok();
bool touch_ok();

// ---- 引脚排查工具（只在串口命令里用）----
// 直接把某个 GPIO 当背光拉高/拉低，绕过 LGFX 的 PWM 通道
void display_bl_pin(int pin, bool on);
// 依次把候选引脚拉高/拉低，每个 1.2 秒，屏幕变亮的那一下对应的脚就是背光脚
// from/to 是候选表下标，用来分批二分；to<=0 表示扫到底
void display_bl_sweep(int from = 0, int to = 0);
// 全屏纯色，绕开 LVGL 直接写屏（验证数据线是否接对）
void display_fill(uint8_t r, uint8_t g, uint8_t b);
// 六色循环，每色 1 秒（红绿蓝白黑，黑会看不出，所以最后回到白）
void display_color_cycle();

// 原始 SPI 探针：绕开 LovyanGFX，用裸 ST7789 指令在候选 DC 脚上逐个刷色块
// 用来判定「DC 到底是哪个脚」——不依赖库配置，也不依赖 init() 的返回值
void lcd_raw_probe();

// 把颜色一直保持在屏上（不超时），frame=true 时加 10px 黑边用于判断是否铺满
// cs / rst >= 0 时分别当作片选（拉低）和复位脚（低脉冲后拉高）
void lcd_raw_paint(int dc, int cs, uint8_t r, uint8_t g, uint8_t b, bool frame,
                   int rst = -1);

// 暂停/恢复 LVGL 刷新。测屏时必须暂停，否则界面会立刻把我刷的颜色盖掉
void display_pause(bool paused);
bool display_is_paused();

// 单脚背光 + 四色循环：把 pin 拉高，然后白→红→绿→蓝各 1.2 秒
// pin 传 -1 表示不驱动任何脚（只测画面）
void lcd_bl_test(int pin);

// 扫 I2C 总线，找触摸芯片和可能存在的 LCD 使能扩展芯片
void i2c_scan(int sda, int scl);
void i2c_dump(int addr, int start = 0);
void i2c_write(int addr, int reg, int val);

// 触摸两级验证：raw 直读芯片寄存器，live 走完整驱动链路
void tp_raw_test(int seconds = 15);
void tp_live_test(int seconds = 15);

// 运行时改触摸轴补偿 0~3（现场试方向用）
void touch_set_rotation(int r);

// 颜色校验页：八色块 + 32 级灰阶，常驻显示（发 uiresume 退出）
void color_test_page();

// 反色开关：颜色像底片时现场切换，不用重新编译
void display_set_invert(bool on);
bool display_get_invert();
void display_toggle_invert();

// 触摸统计：测试页显示手指坐标与点击次数
void display_touch_stat(uint16_t* x, uint16_t* y, uint32_t* n);
void display_touch_stat_reset();
