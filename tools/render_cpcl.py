#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CPCL 面单渲染器：把 CPCL 指令流还原成位图，用于预览 / 打印。

为什么需要它：
  顺丰小程序发给打印机的不是位图，是 CPCL 文本指令（TEXT / B / LINE / EG）。
  电脑端如果按纯文本打印，出来是一堆指令源码；如果 RAW 直投，只有认 CPCL 的
  打印机才能出图。所以这里自己解一遍，任何打印机都能打。

支持的指令子集（覆盖顺丰/快递面单实际用到的全部）：
  ! hres vres height qty      页面定义
  PAGE-WIDTH / PAGE-HEIGHT    页面尺寸
  LEFT / CENTER / RIGHT       对齐
  SETBOLD 0|1                 粗体
  SETMAG x y                  文字放大倍数
  TEXT / TEXT90 / TEXT270     文字（横向 / 逆时针90 / 顺时针90）
  LINE x1 y1 x2 y2 w          线条
  B 128 w r h x y data        Code128 条码
  B QR x y M m U u ... ENDQR  二维码
  EG byteW h x y hex          位图
  FORM / PRINT                结束
"""
from __future__ import annotations

import io
import math
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_CJK = r"C:\Windows\Fonts\simhei.ttf"
FONT_CJK_BOLD = r"C:\Windows\Fonts\simhei.ttf"

# CPCL 字体号 → 点阵高（dots @200dpi）
# 重要：55 和 24 不是"55 比 24 大"。汉印/芝柯的中文点阵库里，
#   55 = 16x16 小号中文（用于提示语、客服热线这类小字）
#   24 = 24x24 标准中文（用于地址、姓名这类正文字）
# 判断依据来自实测行距：4 行提示语 y=3/21/39/57（步长 18），
# 只有 16 高才排得下且不重叠；而地址行步长 25~30，对应 24。
FONT_SIZES = {55: 16, 24: 24, 7: 21, 4: 24, 3: 16, 2: 12, 0: 12, 1: 8}

# Code128 的 stop 符是 13 模块，barcode 库的 CODES 表只到 105，stop 单独补
_C128_STOP = "1100011101011"


def _code128b_bits(data: str) -> str:
    """Code128B 编成模块位串（1 = 黑条）。
    只做 B 模式直编、不做数字对压缩 —— 与 Zebra / 芝柯固件的行为一致，
    也与 CPCL 的 narrow bar 宽度语义对得上（15 位运单号 = 200 模块）。"""
    from barcode.charsets.code128 import CODES

    vals = [104]                        # START B
    for c in data:
        o = ord(c)
        if 32 <= o <= 126:
            vals.append(o - 32)
    if len(vals) < 2:
        return ""
    checksum = (vals[0] + sum(i * v for i, v in enumerate(vals[1:], 1))) % 103
    return "".join(CODES[v] for v in vals) + CODES[checksum] + _C128_STOP


class CpclRenderer:
    def __init__(self, text: str, scale: int = 3, dpi: int = 200,
                 zoom: float = 1.0):
        self.lines = text.split("\n")
        self.scale = scale
        self.dpi = dpi
        self.width = 577
        self.height = 798
        self.bold = False
        self.mag_x = 1
        self.mag_y = 1
        self.align = "LEFT"
        self._pending_qr: dict | None = None
        # zoom：按倍数重绘整张画布（坐标、字号一起放大），
        # 用于输出高分辨率 PDF。不能用位图放大 —— 16 点小字放大后是糊的。
        self.zoom = max(0.1, float(zoom))

        # 先扫一遍拿页面尺寸
        for L in self.lines:
            t = L.strip()
            if t.startswith("! "):
                p = t.split()
                if len(p) >= 5:
                    try:
                        self.height = int(float(p[4]))
                    except ValueError:
                        pass
            elif t.upper().startswith("PAGE-WIDTH"):
                p = t.split()
                if len(p) >= 2:
                    self.width = int(float(p[1]))
            elif t.upper().startswith("PAGE-HEIGHT"):
                p = t.split()
                if len(p) >= 2:
                    self.height = int(float(p[1]))

        self.img = Image.new("L", (self.W, self.H), 255)
        self.d = ImageDraw.Draw(self.img)
        # EG 位图的十六进制数据换行时会续行，先拼回一条
        self.lines = self._prejoin_hex(self.lines)
        # 记录所有实体元素的包围盒（CPCL 原始坐标），用于事后检测重叠
        self.elements: list[tuple] = []

    @property
    def W(self) -> int:
        return max(1, int(round(self.width * self.zoom)))

    @property
    def H(self) -> int:
        return max(1, int(round(self.height * self.zoom)))

    @staticmethod
    def _prejoin_hex(lines: list[str]) -> list[str]:
        """CPCL 的 EG 数据过长时会折行，折行部分单独占一行纯 hex。
        不拼回来的话位图只会画出前一段（表现为图案缺角/错位）。"""
        out: list[str] = []
        i = 0
        while i < len(lines):
            t = lines[i].strip()
            m = re.match(r"^EG\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+"
                         r"([0-9A-Fa-f]*)$", t)
            if m:
                need = int(m.group(1)) * int(m.group(2)) * 2
                hx = m.group(5)
                j = i + 1
                while len(hx) < need and j < len(lines):
                    nxt = lines[j].strip()
                    if re.fullmatch(r"[0-9A-Fa-f]+", nxt):
                        hx += nxt
                        j += 1
                    else:
                        break
                out.append(f"EG {m.group(1)} {m.group(2)} {m.group(3)} "
                           f"{m.group(4)} {hx}")
                i = j
                continue
            out.append(lines[i])
            i += 1
        return out
    def _record(self, kind: str, desc: str, x0, y0, x1, y1):
        """坐标一律换算回 CPCL 原始点坐标，报告才看得懂。"""
        z = self.zoom
        self.elements.append((kind, desc, int(x0 / z), int(y0 / z),
                              int(x1 / z), int(y1 / z)))

    def report_overlaps(self, min_px: int = 6) -> list[tuple]:
        """找出互相压住的实体元素（文字/条码/二维码/位图）。
        面单里线条穿文字是正常的，所以只报实体之间的重叠。"""
        out = []
        els = self.elements
        for i in range(len(els)):
            for j in range(i + 1, len(els)):
                a, b = els[i], els[j]
                ox = min(a[4], b[4]) - max(a[2], b[2])
                oy = min(a[5], b[5]) - max(a[3], b[3])
                if ox > min_px and oy > min_px:
                    out.append((a, b, ox, oy))
        return out

    # ---------------- 基础绘图 ----------------
    def _font(self, dot_h: int, bold: bool):
        path = FONT_CJK_BOLD if bold else FONT_CJK
        try:
            return ImageFont.truetype(path, dot_h)
        except Exception:
            return ImageFont.load_default()

    def _draw_text(self, font_no: int, x: int, y: int, data: str,
                   rotate: int = 0):
        z = self.zoom
        x = int(round(x * z))
        y = int(round(y * z))
        base = FONT_SIZES.get(font_no, 24)
        dot_h = max(6, int(round(base * self.mag_y * z)))
        f = self._font(dot_h, self.bold)
        sw = 1 if self.bold else 0

        # 用「字格基线」定位，而不是墨迹外框 —— 这是对齐的关键。
        # CPCL 点阵字的 (x,y) 是字格左上角，同一 y 的多个 TEXT 片段必须
        # 共用同一基线，否则中英文混排时墨迹顶高低不一，看起来就是错位。
        asc, desc = f.getmetrics()
        baseline = int(round(dot_h * 0.88))

        probe = ImageDraw.Draw(Image.new("L", (1, 1)))
        tw = int(math.ceil(probe.textlength(data, font=f))) + 2 * sw + 4
        th = dot_h + desc + 4
        tw = max(tw, 1)
        th = max(th, 1)
        tmp = Image.new("L", (tw, th), 255)
        td = ImageDraw.Draw(tmp)
        td.text((2 + sw, baseline), data, font=f, fill=0, anchor="ls",
                stroke_width=sw, stroke_fill=0)

        # SETMAG 横向倍数：CPCL 允许 x/y 不等比放大，此时按比例拉伸
        if self.mag_y and self.mag_x != self.mag_y:
            nw = max(1, int(round(tw * self.mag_x / self.mag_y)))
            tmp = tmp.resize((nw, th), Image.LANCZOS)
            tw = nw

        if rotate:
            tmp = tmp.rotate(rotate, expand=True)
            tw, th = tmp.size

        # 水平定位
        px = x
        if self.align == "CENTER":
            px = (self.W - tw) // 2
        elif self.align == "RIGHT":
            px = self.W - tw

        # 垂直：CPCL 的 y 指字格顶部
        py = y

        # 旋转文字的定位点语义不同：
        #   TEXT270（顺时针 90°，右侧竖排）→ x 是文字块的【右】边缘
        #   TEXT90 （逆时针 90°，左侧竖排）→ x 是文字块的【左】边缘
        # 旋转后 tmp.size = (字格高, 原文字宽)，即 tw=字高、th=字宽。
        if rotate == 270:
            px = x - tw
        elif rotate == 90:
            px = x

        # 记录实际墨迹范围（不是字格框）—— 字格底部是空白，
        # 用字格判重叠会把「535WA-KE-021」和「NA」这种差几个点的误报成压字。
        from PIL import ImageOps
        mask = ImageOps.invert(tmp)
        ink = mask.getbbox()
        if ink:
            self._record("text", data[:18], px + ink[0], py + ink[1],
                         px + ink[2], py + ink[3])
        else:
            self._record("text", data[:18], px, py, px + tw, py + th)

        # 只把黑字叠加上去，不能用白底 paste ——
        # 白底会把下方的框线、条码、logo 擦掉（面单里文字和线条大量交叠）
        self.img.paste(tmp, (px, py), mask)

    def _line(self, x1, y1, x2, y2, w):
        z = self.zoom
        w = max(1, int(round(w * z)))
        self.d.line([(x1 * z, y1 * z), (x2 * z, y2 * z)], fill=0, width=w)

    def _barcode128(self, x, y, height, data, narrow, ratio):
        """Code128：按模块数 × 窄条宽 精确绘制。
        B 128 2 2 70 75 108 → 窄条 2 点、高 70、左下角 (75,108)。
        运单号 15 位按 Code128B 编码是 200 模块 → 宽 400 点（占面单大半），
        而不是老代码按图片高度比例缩放出来的 199 点（只有一半宽）。"""
        try:
            bits = _code128b_bits(data)
            if not bits:
                raise ValueError("Code128B 编码失败")
            z = self.zoom
            nw = max(1, int(round(narrow * z)))
            w = len(bits) * nw
            h = max(1, int(round(height * z)))
            bar = Image.new("1", (w, h), 1)
            bd = ImageDraw.Draw(bar)
            for i, ch in enumerate(bits):
                if ch == "1":
                    bd.rectangle([i * nw, 0, i * nw + nw - 1, h - 1], fill=0)
            bx, by = int(round(x * z)), int(round(y * z))
            self.img.paste(bar.convert("L"), (bx, by))
            self._record("barcode", data[:18], bx, by, bx + w, by + h)
            return True
        except Exception as e:
            self.d.rectangle([x, y, x + 300, y + height], outline=0, width=2)
            self.d.text((x + 4, y + 4), f"[BARCODE] {data}", fill=0)
            print(f"      条码降级为占位框: {e}")
            return False

    def _qrcode(self, x, y, unit, data):
        try:
            import qrcode

            q = qrcode.QRCode(version=None, box_size=1, border=2,
                              error_correction=qrcode.constants.ERROR_CORRECT_M)
            q.add_data(data)
            q.make(fit=True)
            qr = q.make_image(fill_color="black", back_color="white").convert("L")
            # unit 是每个模块的点数
            size = q.modules_count + q.border * 2
            px = size * max(1, int(unit))
            qr = qr.resize((px, px), Image.NEAREST)
            z = self.zoom
            if z > 1:
                px = int(round(px * z))
                qr = qr.resize((px, px), Image.NEAREST)
            gx, gy = int(round(x * z)), int(round(y * z))
            self.img.paste(qr, (gx, gy))
            self._record("qr", f"QR({size}模块)", gx, gy, gx + px, gy + px)
            return True
        except Exception as e:
            self.d.rectangle([x, y, x + 200, y + 200], outline=0, width=2)
            self.d.text((x + 4, y + 4), "[QR]", fill=0)
            print(f"      二维码降级为占位框: {e}")
            return False

    def _eg(self, byte_w, height, x, y, hexdata):
        """EG 位图：每行 byte_w 个字节，1 = 黑"""
        try:
            raw = bytes.fromhex(hexdata)
        except ValueError:
            return
        row_bytes = byte_w
        if row_bytes <= 0:
            return
        rows = len(raw) // row_bytes
        if rows <= 0:
            return
        bits = Image.new("1", (row_bytes * 8, rows), 1)
        px = bits.load()
        for r in range(rows):
            for c in range(row_bytes):
                b = raw[r * row_bytes + c]
                for k in range(8):
                    if b & (0x80 >> k):
                        px[c * 8 + k, r] = 0
        z = self.zoom
        target_h = max(1, int(round(height * z)))
        target_w = max(1, int(round(row_bytes * 8 * z)))
        bits = bits.convert("L").resize((target_w, target_h), Image.NEAREST)
        gx, gy = int(round(x * z)), int(round(y * z))
        self.img.paste(bits, (gx, gy))
        from PIL import ImageOps
        ink = ImageOps.invert(bits).getbbox()
        if ink:
            self._record("eg", f"位图{target_w}x{target_h}",
                         gx + ink[0], gy + ink[1], gx + ink[2], gy + ink[3])
        else:
            self._record("eg", f"位图{target_w}x{target_h}", gx, gy,
                         gx + target_w, gy + target_h)

    # ---------------- 主解析循环 ----------------
    def render(self) -> Image.Image:
        for idx, L in enumerate(self.lines):
            t = L.strip()
            if not t:
                continue
            u = t.upper()

            if t.startswith("!"):
                continue
            if u.startswith(("PAGE-WIDTH", "PAGE-HEIGHT", "GAP-SENSE",
                             "FORM", "PRINT", "BAR-SENSE", "SETSP",
                             "COUNTRY", "TONE", "CONTRAST", "JOURNAL",
                             "PREFEED", "POSTFEED", "ON-FEED", "NO-PACE",
                             "PACE", "SET-TOF", "AT", "POSTFEED", "SPEED")):
                # 二维码数据行可能是任意文本，这里只跳过已知控制指令
                if u.startswith("FORM") or u.startswith("PRINT"):
                    continue
                continue

            if u.startswith("LEFT"):
                self.align = "LEFT"; continue
            if u.startswith("CENTER"):
                self.align = "CENTER"; continue
            if u.startswith("RIGHT"):
                self.align = "RIGHT"; continue

            if u.startswith("SETBOLD"):
                p = t.split()
                self.bold = len(p) > 1 and p[1] == "1"
                continue

            if u.startswith("SETMAG"):
                p = t.split()
                if len(p) >= 3:
                    try:
                        self.mag_x, self.mag_y = int(p[1]), int(p[2])
                    except ValueError:
                        pass
                continue

            m = re.match(r"^TEXT(90|270)?\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s*(.*)$",
                         t)
            if m:
                rot = int(m.group(1)) if m.group(1) else 0
                font_no = int(m.group(2))
                x, y = int(m.group(4)), int(m.group(5))
                data = m.group(6)
                if data:
                    self._draw_text(font_no, x, y, data, rotate=rot)
                continue

            m = re.match(r"^LINE\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)", t)
            if m:
                self._line(*[int(g) for g in m.groups()])
                continue

            # QR 码起始：B QR x y M m U u
            m = re.match(r"^B\s+QR\s+(-?\d+)\s+(-?\d+)\s+M\s+(\d+)\s+U\s+(\d+)", t, re.I)
            if m:
                self._pending_qr = {"x": int(m.group(1)), "y": int(m.group(2)),
                                    "unit": int(m.group(4)), "data": []}
                continue
            if self._pending_qr is not None:
                if u.startswith("ENDQR"):
                    d = "\n".join(self._pending_qr["data"])
                    self._qrcode(self._pending_qr["x"], self._pending_qr["y"],
                                 self._pending_qr["unit"], d)
                    self._pending_qr = None
                else:
                    self._pending_qr["data"].append(t)
                continue

            # 条码：B 128 w r h x y data
            m = re.match(r"^B\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(.+)$", t)
            if m:
                btype, nw, ratio, h, x, y = (int(m.group(i)) for i in range(1, 7))
                data = m.group(7)
                if btype in (128, 1280, 129):
                    self._barcode128(x, y, h, data, nw, ratio)
                continue

            m = re.match(r"^EG\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+([0-9A-Fa-f]+)", t)
            if m:
                self._eg(int(m.group(1)), int(m.group(2)),
                         int(m.group(3)), int(m.group(4)), m.group(5))
                continue

            # 其余未识别指令静默跳过（不影响出图）
        return self.img


def render_file(path: str, out: str | None = None, scale: int = 3) -> Path:
    raw = Path(path).read_bytes()
    for enc in ("gbk", "utf-8", "latin-1"):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = raw.decode("gbk", "replace")

    r = CpclRenderer(text, scale=scale)
    img = r.render()
    if scale != 1:
        img = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    out_p = Path(out) if out else Path(path).with_suffix(".cpcl.png")
    img.save(out_p, dpi=(r.dpi, r.dpi))
    print(f"渲染完成: {out_p}")
    print(f"  画布 {r.width}x{r.height} dots @{r.dpi}dpi "
          f"({r.width / r.dpi * 25.4:.1f} x {r.height / r.dpi * 25.4:.1f} mm)")
    print(f"  输出 {img.width}x{img.height} px (scale={scale})")

    over = r.report_overlaps()
    out_of_page = [e for e in r.elements
                   if e[2] < 0 or e[3] < 0 or e[4] > r.width or e[5] > r.height]
    if over:
        print(f"  [重叠] {len(over)} 处实体元素互相压住（真机打印结果相同）：")
        for a, b, ox, oy in over:
            print(f"    - {a[0]}「{a[1]}」({a[2]},{a[3]}) 压 {b[0]}「{b[1]}」"
                  f"({b[2]},{b[3]})  重叠 {ox}x{oy} 点")
    if out_of_page:
        print(f"  [出界] {len(out_of_page)} 处超出页面，真机会被裁掉：")
        for e in out_of_page:
            print(f"    - {e[0]}「{e[1]}」({e[2]},{e[3]})-({e[4]},{e[5]})")
    if not over and not out_of_page:
        print("  [检查] 无重叠、无出界")
    return out_p


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python render_cpcl.py <文件.bin> [输出.png] [放大倍数]")
        sys.exit(1)
    sc = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    render_file(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None, sc)
