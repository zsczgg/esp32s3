/**
 * LVGL 界面
 * 只读 app_state 的快照来画，不直接碰蓝牙/WiFi 那层。
 * 需要改变设备行为的操作（切设备名、重连、重启）通过下面这组回调转给 main.cpp。
 */
#pragma once
#include <stdint.h>
#include <Arduino.h>

// 由 main.cpp 实现
void app_cycle_devname();     // 切到下一个预设设备名
void app_reconnect_wifi();    // 重连 WiFi
void app_reboot();            // 重启
void app_set_forward(bool on); // 自动转发开关
bool app_get_forward();
String app_agent_str();       // "192.168.8.221:8765"

void ui_init();
void ui_update();             // 每帧调用，内部限流到 ~10Hz

// 颜色校验页：常驻显示，点屏幕才返回主界面。
// 不自动消失，不用赶时间看。
void ui_show_color_page();

// 切到测试页第 idx 屏（0~8，8 是灰阶）
void ui_color_apply(int idx);

// 打印 LVGL 内存池用量（池耗尽会 PANIC，是"点着点着重启"的常见原因）
void ui_mem_report();
