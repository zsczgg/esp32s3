#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口监视 —— pio device monitor 的替代品。
好处是能精确控制 DTR/RTS（C3 的 USB-JTAG 对这个很敏感），也能主动复位。

用法：
    python monitor.py                 监听 COM10，15 秒
    python monitor.py COM10 30        监听 30 秒
    python monitor.py COM10 15 --reset  先复位再听（能抓到完整启动日志）
"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
do_reset = "--reset" in sys.argv

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()

if do_reset:
    s.rts = True          # 拉低 EN
    time.sleep(0.15)
    s.rts = False
    time.sleep(0.1)

t0 = time.time()
got = 0
try:
    while time.time() - t0 < secs:
        data = s.read(8192)
        if data:
            got += len(data)
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
finally:
    s.close()
    if got == 0:
        print(f"\n[monitor] {secs:.0f} 秒内 0 字节", flush=True)
    else:
        print(f"\n[monitor] 共收到 {got} 字节", flush=True)
