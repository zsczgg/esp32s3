"""
常驻串口监听：打开一次，之后不再关闭，避免反复打开串口复位板子。

用法: python -u tools/listen.py COM6
"""
import sys
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

s = serial.Serial(port, baud, timeout=0.3)
s.dtr = False
s.rts = False
s.reset_input_buffer()

print(f"--- 开始监听 {port} @ {baud} (板子会复位一次，之后保持连接) ---", flush=True)

while True:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
