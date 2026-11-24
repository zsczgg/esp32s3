#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
电脑端链路自测 —— 不依赖 ESP32。

造一张 100x150mm 的假面单，编码成 ESC/POS 的 GS v 0 光栅数据，
POST 给 print_agent.py，看它能不能还原成 PDF。

用法：
    python test_agent.py                发一次
    python test_agent.py --keep         保留生成的中间文件
"""
import sys
import time
import datetime
import urllib.request
import urllib.error
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

AGENT_URL = "http://127.0.0.1:8765/print"
JOBS_DIR = Path(__file__).resolve().parent.parent / "pc-agent" / "jobs"

W, H = 800, 1240   # 100x150mm @ 203dpi


def font(size):
    for p in ("C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            continue
    return ImageFont.load_default()


def make_waybill() -> Image.Image:
    """黑白位图，1 = 白，0 = 黑"""
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)

    d.rectangle([2, 2, W - 3, H - 3], outline=0, width=4)
    d.text((36, 36), "顺丰速运  SF EXPRESS", font=font(56), fill=0)
    d.line([36, 118, W - 36, 118], fill=0, width=3)

    d.text((36, 140), "运单号", font=font(30), fill=0)
    d.text((36, 180), "SF1234567890123", font=font(58), fill=0)

    d.line([36, 270, W - 36, 270], fill=0, width=2)
    rows = [
        ("寄件人", "深圳市南山区科技园 张三  138****8888"),
        ("收件人", "首尔特别市 麻浦区 李四  010-1234-5678"),
        ("物品", "文件 / Documents"),
        ("重量", "0.5 kg"),
        ("时间", datetime.datetime.now().strftime("%Y-%m-%d %H:%M")),
    ]
    y = 300
    for k, v in rows:
        d.text((36, y), k, font=font(30), fill=0)
        d.text((160, y), v, font=font(30), fill=0)
        y += 62

    # 假条码区
    d.rectangle([36, 720, W - 36, 900], outline=0, width=2)
    x = 60
    seed = 1
    while x < W - 60:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        w = 3 + (seed % 7)
        if (seed >> 8) & 1:
            d.rectangle([x, 730, x + w, 890], fill=0)
        x += w + 3
    d.text((36, 920), "*SF1234567890123*", font=font(34), fill=0)

    d.line([36, 1000, W - 36, 1000], fill=0, width=2)
    d.text((36, 1030), "云打印盒 自测页  CLOUD PRINT BOX TEST", font=font(28), fill=0)
    d.text((36, 1090), "如果你看到这张, 说明 位图解析 + 渲染 正常", font=font(26), fill=0)
    return img


def to_escpos(img: Image.Image) -> bytes:
    """编码成 ESC/POS GS v 0 光栅块"""
    w, h = img.size
    bpr = (w + 7) // 8
    px = img.load()
    body = bytearray()
    for y in range(h):
        for bx in range(bpr):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and px[x, y] == 0:
                    b |= (0x80 >> bit)
            body.append(b)
    header = bytes([
        0x1D, 0x76, 0x30, 0x00,          # GS v 0 m
        bpr & 0xFF, (bpr >> 8) & 0xFF,   # xL xH
        h & 0xFF, (h >> 8) & 0xFF,       # yL yH
    ])
    return header + bytes(body)


def main():
    keep = "--keep" in sys.argv

    print(f"[1/3] 生成假面单 {W}x{H} ...")
    img = make_waybill()

    print("[2/3] 编码 ESC/POS GS v 0 ...")
    payload = to_escpos(img)
    print(f"      {len(payload)} 字节（原始位图 {W // 8 * H} 字节）")

    if keep:
        dbg = JOBS_DIR / "selftest_input.png"
        JOBS_DIR.mkdir(exist_ok=True)
        img.save(dbg)
        print(f"      输入图已存 {dbg}")

    print(f"[3/3] POST -> {AGENT_URL}")
    req = urllib.request.Request(
        AGENT_URL, data=payload,
        headers={
            "Content-Type": "application/octet-stream",
            "X-Job-Id": f"selftest-{int(time.time())}",
            "X-Chunk-Seq": "0",
            "X-Final": "1",
            "X-Device": "pc-selftest",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            print(f"      HTTP {r.status} {r.read().decode()}")
    except urllib.error.URLError as e:
        print(f"      连不上 agent：{e}")
        print("      先启动：cd pc-agent && python print_agent.py --dry-run")
        return 1

    time.sleep(2.5)
    print("\n[结果] jobs 目录最近的文件：")
    files = sorted(JOBS_DIR.glob("*"), key=lambda p: p.stat().st_mtime, reverse=True)[:5]
    if not files:
        print("      （空）")
    for f in files:
        print(f"      {f.name}  {f.stat().st_size} 字节")
    print("\n如果是 dry-run 模式，去看 .pdf 那个能不能正常打开、内容对不对。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
