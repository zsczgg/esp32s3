/**
 * 全局状态 —— 协议层（core 0）往这里写，界面（core 1）从这里读快照
 *
 * 为什么要单独一层：
 *   蓝牙回调和 WiFi 循环在 core 0，LVGL 在 core 1。两边直接摸同一批变量
 *   会出现"读到一半被改"的撕裂（比如字节数变了但百分比没变）。
 *   这里用自旋锁包住一个很小的结构体，UI 每次取一份完整拷贝去画，
 *   临界区只有几十字节 memcpy，不会影响转发实时性。
 */
#pragma once
#include <stdint.h>

#define HIST_MAX 6

enum JobResult : uint8_t {
    JR_OK = 0,      // 已转发 / PDF 已存
    JR_EMPTY = 1,   // 全 0 填充包，丢弃
    JR_FAIL = 2,    // 转发失败
    JR_DROP = 3,    // 超长截断
};

enum Phase : uint8_t {
    PH_IDLE = 0,    // 等手机来连
    PH_RX,          // 正在收数据
    PH_TX,          // 正在往电脑转发
    PH_DONE,        // 这一票完成
    PH_FAIL,        // 这一票失败
};

struct JobRecord {
    uint32_t id;
    uint32_t bytes;
    uint8_t  result;
    uint8_t  hh, mm;        // 没对上时间就都是 0xFF
    char     tag[12];       // 任务号或来源，界面上显示
};

struct AppState {
    // 网络
    bool     wifiUp;
    char     ip[16];
    char     ssid[33];
    int8_t   rssi;

    // 蓝牙
    bool     bleConn;
    char     devName[20];

    // 电脑代理
    bool     agentOk;

    // 当前这一票
    Phase    phase;
    uint32_t rxBytes;
    uint32_t rxExpect;      // 0 = 不知道总量（分段转发时就是这样）
    uint32_t jobId;
    uint8_t  percent;

    // 今日统计（跨天自动清零）
    uint32_t todayTotal;
    uint32_t todayOk;
    uint32_t todayFail;

    // 历史记录（环形，histHead 指向最新一条的下一个位置）
    JobRecord hist[HIST_MAX];
    uint8_t   histCount;
    uint8_t   histHead;

    uint32_t uptimeSec;
};

void     state_init();
AppState state_snapshot();          // UI 用，拿一份完整拷贝

void state_setWifi(bool up, const char* ip, const char* ssid, int rssi);
void state_setBle(bool conn);
void state_setDevName(const char* name);
void state_setAgent(bool ok);

void state_jobBegin(uint32_t jobId);
void state_jobProgress(uint32_t bytes);          // 未知总量，只报已收字节
void state_jobProgress(uint32_t bytes, uint32_t expect);
void state_jobPhase(Phase p);
void state_jobEnd(JobResult r, uint32_t bytes, const char* tag);

// 把 NTP 校时后的时间写进后续记录；没校时就不调用
void state_tick();
