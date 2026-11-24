# 云打印盒

让手机上的**现成小程序**（顺丰速运 / 丰声 / 快递管家等）以为连上了一台蓝牙打印机，实际把打印任务转到电脑上那台打印机执行。

```
手机小程序 --BLE--> ESP32-S3 --WiFi/HTTP--> 电脑 print_agent.py --> 电脑打印机
             (冒充 HM-A300 等机型)   分段转发        拼票 + 解协议 + 送 PDF / 送打印
```

## 当前状态

| 模块 | 状态 |
|---|---|
| BLE 冒充打印机、接收任务 | 可用（顺丰小程序实测能扫到、能下发） |
| 电脑端拼包还原面单 | 可用 |
| CPCL 面单 → PDF（400dpi 矢量重绘） | 可用 |
| 直送 TSPL 打印机 | 不可用（见下方「已知限制」） |
| 2.8" 屏 + 触摸 LVGL 界面 | 可用（颜色/触摸/字体均已实测校准） |
| 屏幕测试页（单色/CSS 灰阶/实时坐标） | 可用，串口 `colortest` 进入 |

## 目录

```
ble-print-box/
├── firmware/                 ESP32-S3 固件（PlatformIO + Arduino）
│   ├── platformio.ini        环境：s3-lcd（带屏）/ s3-print（无屏精简）
│   └── src/
│       ├── config.h          要改的都在这里（WiFi / 电脑 IP / 设备名）
│       ├── config_local.h    本地私有覆盖（不入库，放真实 WiFi 密码）
│       ├── board_profile.h   板卡引脚与面板参数，换板只改这一个文件
│       ├── display_port.cpp  LovyanGFX + LVGL 驱动、触摸、诊断命令
│       ├── ui.cpp            界面（主界面 / 设置页 / 屏幕测试页）
│       ├── font_zh_16.c      定制中文字库（黑体 740 字）
│       └── main.cpp          BLE / WiFi / 任务转发主逻辑
├── pc-agent/
│   ├── print_agent.py        电脑端打印代理（HTTP 收包 → 拼票 → 打印机或 PDF）
│   └── jobs/                 任务与产物目录（真实面单不入库）
├── tools/
│   ├── render_cpcl.py        CPCL 指令渲染成 PNG（面单预览）
│   └── cpcl_to_pdf.py        CPCL → PDF，按 dpi 真矢量重绘
├── diag/                     早期硬件排查用的最小固件（wifi_only / softap / mac_spoof）
└── 硬件与开发档案.md          板卡引脚、已固化参数、踩坑表、串口诊断命令
```


## 第一步：填配置

打开 `firmware/src/config.h`，改两处：

| 项 | 改成什么 |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | 你家的 WiFi |
| `AGENT_HOST` | 跑 agent 的电脑局域网 IP（Windows 命令行 `ipconfig` 看 IPv4） |

> 手机、ESP32、电脑必须在**同一个 WiFi** 下。跨网段不行。

## 第二步：烧录

```bash
cd firmware
pio run -e s3-lcd -t upload --upload-port COM6   # 带屏版；无屏精简版用 -e s3-print
pio device monitor -p COM6 -b 115200
```

串口出现 `[BLE] 广播中：` 就说明在等了。板卡引脚全部集中在 `src/board_profile.h`，换板只改那一个文件；具体引脚表见 `硬件与开发档案.md`。

## 第三步：起电脑端

```bash
cd pc-agent
pip install -r requirements.txt
python print_agent.py
```

浏览器开 `http://127.0.0.1:8765/printers` 确认能看到你的打印机名字。
要指定某台打印机，就改 `print_agent.py` 顶部的 `PRINTER_NAME`。

**首次运行 Windows 会弹防火墙提示，必须允许「专用网络」。**

## 第四步：手机连接

打开顺丰小程序 → 打印面单 → 连接蓝牙打印机。

- **能扫到 `HM-A300`** → 连它，然后打一张试试
- **扫不到** → 串口输入 `names` 看候选机型，用 `name BTP-P33` 一个个换着试
- **扫到但连不上** → 看串口日志，通常是缺 notify 应答，告诉我现象我来补
- **连上但没数据** → 小程序可能在等打印机状态回包，同样需要补应答

## 关键原理（出问题时看这里）

**1. 设备名就是身份。** 顺丰系小程序只认它白名单里的型号。所以固件默认把自己叫成 `HM-A300`。改成别的牌子大概率扫不出来。

**2. 面单是位图，不是文字。** 快递面单在小程序里被渲染成点阵，用 ESC/POS 的 `GS v 0` 光栅指令下发。100×150mm @203dpi 约 **124KB**，超过 BLE 单包几个数量级，所以固件按 4KB 一段往电脑送，由电脑端拼起来还原成图。

**3. 电脑端要按打印机类型选模式。**

| 你电脑接的打印机 | `SCALE` 设成 | 说明 |
|---|---|---|
| 热敏标签机（100×150 面单纸） | `native` | 原尺寸输出，一张一张打 |
| A4 激光 / 喷墨 | `fit` | 缩放到页面内，一张面单占一页 |
| 只想要 PDF，自己手动打印 | — | 把 `PDF_PRINT` 设为 `True`，见下方 |

`MODE` 一般留 `auto` 就行。

## 面单存 PDF 而不是直接打印

打印机是 TSPL 指令集、而小程序下发的是 CPCL，两者不等价（300dpi 的 TSPL 机器没法打 203dpi 的 CPCL，字体只能整数倍缩放）。所以最省事的做法是存成 PDF，自己用任意打印机出。

`pc-agent/print_agent.py` 顶部：

| 配置项 | 作用 |
|---|---|
| `PDF_PRINT` | `True` = 不送打印机，只存 PDF |
| `PDF_DPI` | 重绘分辨率，默认 400（矢量重绘，不是位图放大） |
| `PDF_PAPER` | `None` 按标签原尺寸；`"a4"` 排到 A4 |
| `PDF_DIR` | 输出目录；`None` 为 `pc-agent/jobs/` |
| `PDF_DATE_TREE` | `True` 按年/月/日分目录 |

命令行也能指定：`python print_agent.py --pdf-dir D:\面单 --pdf-month`。环境变量 `PRINTBOX_PDF_DIR` 同效。


## 排错表

| 现象 | 原因 | 处理 |
|---|---|---|
| 串口一直超时/乱码 | 波特率不对或没进下载模式 | 115200 8N1；按住 BOOT 再点 RST |
| `[NET] 连接失败` | WiFi 名或密码错，或 5GHz | ESP32 只支持 2.4GHz |
| `HTTP -1` | 电脑 IP 填错 / 防火墙拦了 | 电脑上 `curl http://127.0.0.1:8765/health` 测本地 |
| 电脑收到数据但打不出 | 缺 pywin32 | `pip install pywin32` |
| 打出来是乱码 | 把 RAW 直投给了 A4 打印机 | 改 `SCALE=fit` |
| 一张面单被切成好几张 | 段间隔超过 300ms | 加大 `JOB_IDLE_MS` |

## 串口命令

业务相关：

```
status          看状态（WiFi / IP / RSSI / BLE / 已收任务数）
names           看候选机型
name HM-A300    换广播名（立即生效，不重启）
wifi <SSID> <密码>  换 WiFi（存 NVS，重启生效）
fwd off         纯抓包模式，不转发，只 dump hex
dump            立刻打印当前缓冲的十六进制
clear           清空缓冲
reboot          重启
```

屏幕与诊断（带屏版本）：

```
colortest       屏幕测试页：全屏单色+色名，点屏幕换下一色，9 = 灰阶
mem             LVGL 内存池用量 + ESP 空闲堆
lcdinfo         屏与触摸的引脚、分辨率、驱动信息
i2cscan         I2C 扫描（应看到 0x18 音频、0x38 触摸）
tprot 0~3       触摸坐标轴补偿，现场试方向
tpraw [秒]      绕过驱动直读触摸芯片点数（判断硬件层通不通）
tplive [秒]     走完整驱动链路读坐标（判断驱动层通不通）
fill r g b      全屏填色
uistop / uiresume  暂停 / 恢复 LVGL，直连驱动画图用
```

## 已知限制

| 限制 | 原因 | 影响 |
|---|---|---|
| 广播包里不带 0xFF00 服务 UUID | Flags + txpower + UUID + 名字 = 33 字节，超过单包 31 上限（Arduino BLE 库自身还有 UAF 缺陷，已在固件里绕开） | 按名字识别的小程序不受影响；若某小程序靠 UUID 过滤扫描会扫不到 |
| 不能直打 TSPL 机器 | 分辨率与字体缩放不匹配 | 用 `PDF_PRINT` 存 PDF 自行打印 |
| 触摸右边缘约 60px 偏差 | 触摸玻璃边缘死区 | 未做线性拉伸 |


## 关于这事的可行性

`fwd off` + `dump` 就是给你抓协议的。如果小程序死活不配合，先用这个看它到底发了什么，再决定下一步。

另外这只是自己用的 DIY，别做成产品去卖——模拟别家品牌型号这事自己玩没问题，商用是另一回事。
