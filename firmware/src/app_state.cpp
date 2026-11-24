#include "app_state.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

static AppState        s_st;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t        s_bootMs = 0;

static inline void lock()   { portENTER_CRITICAL(&s_mux); }
static inline void unlock() { portEXIT_CRITICAL(&s_mux); }

// 跨天清零：只在日期变化时整组归零
static int s_lastYday = -1;

void state_init() {
    lock();
    memset(&s_st, 0, sizeof(s_st));
    s_st.phase = PH_IDLE;
    strncpy(s_st.devName, "HM-A300", sizeof(s_st.devName) - 1);
    unlock();
    s_bootMs = millis();
}

AppState state_snapshot() {
    AppState copy;
    lock();
    copy = s_st;
    unlock();
    return copy;
}

void state_setWifi(bool up, const char* ip, const char* ssid, int rssi) {
    lock();
    s_st.wifiUp = up;
    s_st.rssi = (int8_t)rssi;
    if (ip)   { strncpy(s_st.ip, ip, sizeof(s_st.ip) - 1);       s_st.ip[sizeof(s_st.ip) - 1] = 0; }
    if (ssid) { strncpy(s_st.ssid, ssid, sizeof(s_st.ssid) - 1); s_st.ssid[sizeof(s_st.ssid) - 1] = 0; }
    if (!up)  s_st.ip[0] = 0;
    unlock();
}

void state_setBle(bool conn) {
    lock();
    s_st.bleConn = conn;
    unlock();
}

void state_setDevName(const char* name) {
    if (!name) return;
    lock();
    strncpy(s_st.devName, name, sizeof(s_st.devName) - 1);
    s_st.devName[sizeof(s_st.devName) - 1] = 0;
    unlock();
}

void state_setAgent(bool ok) {
    lock();
    s_st.agentOk = ok;
    unlock();
}

void state_jobBegin(uint32_t jobId) {
    lock();
    s_st.jobId = jobId;
    s_st.rxBytes = 0;
    s_st.rxExpect = 0;
    s_st.percent = 0;
    s_st.phase = PH_RX;
    unlock();
}

void state_jobProgress(uint32_t bytes) {
    lock();
    s_st.rxBytes = bytes;
    s_st.percent = 0;          // 不知道总量，界面显示"已收 xxx 字节"
    if (s_st.phase == PH_IDLE) s_st.phase = PH_RX;
    unlock();
}

void state_jobProgress(uint32_t bytes, uint32_t expect) {
    lock();
    s_st.rxBytes = bytes;
    s_st.rxExpect = expect;
    s_st.percent = expect ? (uint8_t)((uint64_t)bytes * 100 / expect) : 0;
    if (s_st.percent > 100) s_st.percent = 100;
    if (s_st.phase == PH_IDLE) s_st.phase = PH_RX;
    unlock();
}

void state_jobPhase(Phase p) {
    lock();
    s_st.phase = p;
    unlock();
}

void state_jobEnd(JobResult r, uint32_t bytes, const char* tag) {
    lock();
    JobRecord& rec = s_st.hist[s_st.histHead];
    rec.id = s_st.jobId;
    rec.bytes = bytes;
    rec.result = (uint8_t)r;
    rec.hh = 0xFF;
    rec.mm = 0xFF;

    // 有 NTP 时间就填真实时刻
    time_t now = time(nullptr);
    if (now > 1600000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        rec.hh = (uint8_t)tmv.tm_hour;
        rec.mm = (uint8_t)tmv.tm_min;
        s_lastYday = tmv.tm_yday;
    }

    if (tag) {
        strncpy(rec.tag, tag, sizeof(rec.tag) - 1);
        rec.tag[sizeof(rec.tag) - 1] = 0;
    } else {
        snprintf(rec.tag, sizeof(rec.tag), "#%lu", (unsigned long)rec.id);
    }

    s_st.histHead = (s_st.histHead + 1) % HIST_MAX;
    if (s_st.histCount < HIST_MAX) s_st.histCount++;

    s_st.todayTotal++;
    if (r == JR_OK)       s_st.todayOk++;
    else if (r == JR_FAIL) s_st.todayFail++;

    s_st.phase = (r == JR_FAIL) ? PH_FAIL : PH_DONE;
    s_st.rxBytes = bytes;
    s_st.percent = 100;
    unlock();
}

void state_tick() {
    uint32_t up = (millis() - s_bootMs) / 1000;

    // 跨天清零：借 NTP 的本地日期判断
    int yday = -1;
    time_t now = time(nullptr);
    if (now > 1600000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        yday = tmv.tm_yday;
    }

    lock();
    s_st.uptimeSec = up;
    if (yday >= 0 && s_lastYday >= 0 && yday != s_lastYday) {
        s_st.todayTotal = 0;
        s_st.todayOk = 0;
        s_st.todayFail = 0;
    }
    if (yday >= 0) s_lastYday = yday;
    unlock();
}
