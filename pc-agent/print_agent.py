#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
============================================================
 云打印盒 · 电脑端打印代理
============================================================
 ESP32 把小程序发来的打印数据整票（或分段）POST 到这里，
 这里负责：存盘 → 识别类型 → 交给本机打印机执行。

 启动：  python print_agent.py
 测试：  浏览器打开 http://127.0.0.1:8765/printers  查看打印机列表

 依赖：  pip install pywin32 pillow
============================================================
"""
import os
import sys
import time
import threading
import subprocess
import shutil
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 面单渲染器在上一级 tools/ 里
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
try:
    from cpcl_to_pdf import cpcl_to_pdf
except Exception as _e:      # tools 缺失时不影响代理其他功能
    cpcl_to_pdf = None
    print(f"[warn] CPCL 渲染模块不可用: {_e}")

# ============================ 配置 ============================
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8765

# 打印机名字。留空 = 用系统默认打印机。
# 用 http://127.0.0.1:8765/printers 可以看到准确名字
PRINTER_NAME = ""

# 打印模式：
#   auto   自动判断（推荐）
#   raw    原始字节直接塞进打印队列 —— 只有热敏小票机/标签机才这么干
#   pdf    PDF 静默打印
#   image  图片按页面适配打印
MODE = "auto"

# 只解析、不真打（调试链路用，避免浪费纸或弹对话框）
DRY_RUN = False

# 面单尺寸策略：
#   fit     等比缩放到页面内（A4 打印机用这个）
#   native  按原始物理尺寸打（标签机用这个）
SCALE = "fit"

SAVE_DIR = Path(__file__).resolve().parent / "jobs"

# ------------------ 快递面单（CPCL）转 PDF ------------------
# 顺丰/快递小程序发来的是 CPCL 指令流（203dpi 坐标，577x798 点 = 72x100mm）。
# 本机标签机是 TSPL + 300dpi，指令不能直投，所以先渲染成 PDF 存档，
# PDF 是设备无关的，打印时自己选纸型、缩放都行。
#   PDF_PRINT = False → 只存 PDF 不打印（自己回头打）
#   PDF_PRINT = True  → 存完顺带送打印机
PDF_PRINT = False
PDF_DPI = 400            # 重绘分辨率，400dpi 打 300dpi 机器够用且文件不大
PDF_PAPER = None         # None=面单实际尺寸(72x100mm)；可填 "100x150" / "a4"

# PDF 存到哪里 —— 还没定就留空，默认跟 .bin 一起放在 jobs/ 里。
# 三种改法，任选一种（优先级从高到低）：
#   1) 临时命令行：  python print_agent.py --pdf-dir "D:\面单"
#   2) 系统环境变量：set PRINTBOX_PDF_DIR=D:\面单
#   3) 直接改下面这行：PDF_DIR = r"D:\面单"
# 想按月份自动分文件夹（D:\面单\2026-11\xxx.pdf）就把 DATE_TREE 改成 True
PDF_DIR = os.environ.get("PRINTBOX_PDF_DIR", "")
PDF_DATE_TREE = False

# 收到最后一段后，等这么久还没新数据就认定"这一票发完了"
IDLE_FLUSH_SEC = 5.0

SUMATRA_CANDIDATES = [
    r"C:\Program Files\SumatraPDF\SumatraPDF.exe",
    r"C:\Program Files (x86)\SumatraPDF\SumatraPDF.exe",
]

# =============================================================

SAVE_DIR.mkdir(exist_ok=True)


def pdf_path_for(base: Path) -> Path:
    """决定 PDF 落在哪。PDF_DIR 为空 → 与 .bin 同目录。"""
    if not PDF_DIR:
        return base.with_suffix(".pdf")
    d = Path(PDF_DIR).expanduser()
    if PDF_DATE_TREE:
        d = d / time.strftime("%Y-%m")
    try:
        d.mkdir(parents=True, exist_ok=True)
    except Exception as e:
        log(f"[warn] PDF 目录不可写({d}): {e}，退回 jobs/")
        return base.with_suffix(".pdf")
    return d / f"{base.name}.pdf"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def log(*a):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}]", *a, flush=True)


# -------------------------- 依赖探测 --------------------------
def _try_import():
    mods = {}
    try:
        import win32print
        mods["win32print"] = win32print
    except Exception:
        mods["win32print"] = None
    try:
        import win32api
        mods["win32api"] = win32api
    except Exception:
        mods["win32api"] = None
    try:
        from PIL import Image
        mods["Image"] = Image
    except Exception:
        mods["Image"] = None
    return mods


DEPS = _try_import()


def list_printers():
    if not DEPS["win32print"]:
        return []
    wp = DEPS["win32print"]
    try:
        flags = wp.PRINTER_ENUM_LOCAL | wp.PRINTER_ENUM_CONNECTIONS
        return [p[2] for p in wp.EnumPrinters(flags)]
    except Exception as e:
        log("枚举打印机失败:", e)
        return []


def default_printer():
    if not DEPS["win32print"]:
        return ""
    try:
        return DEPS["win32print"].GetDefaultPrinter()
    except Exception:
        return ""


def resolve_printer():
    return PRINTER_NAME or default_printer()


# -------------------------- 类型识别 --------------------------
def sniff(data: bytes) -> str:
    if data[:4] == b"%PDF":
        return "pdf"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:2] == b"\xff\xd8":
        return "jpeg"
    # CPCL 指令流（顺丰/快递面单、标签机）。必须在 gbk_text 之前判断 ——
    # 它是纯文本 + GBK 中文，否则会被当成文本把指令源码排版成 PDF。
    head = data[:4096]
    if b"PAGE-WIDTH" in head or (b"SETMAG" in head and b"TEXT" in head):
        return "cpcl"
    # ESC/POS 标志：初始化 / 切纸 / 光栅位图
    if b"\x1b\x40" in data[:4096] or b"\x1d\x56" in data or b"\x1d\x76\x30" in data:
        return "escpos"
    try:
        data.decode("utf-8")
        return "text"
    except Exception:
        pass
    try:
        data.decode("gbk")
        return "gbk_text"
    except Exception:
        return "binary"


# -------------------------- 打印动作 --------------------------
def raw_print(data: bytes, printer: str | None = None) -> bool:
    """RAW 直投：字节原样进打印队列，只有热敏机/标签机认这套"""
    if DRY_RUN:
        log(f"[dry-run] 跳过 RAW 直投 {len(data)} 字节")
        return True
    wp = DEPS["win32print"]
    if not wp:
        log("✗ 缺 pywin32，无法 RAW 直投")
        return False
    printer = printer or resolve_printer()
    if not printer:
        log("✗ 没有可用打印机")
        return False
    try:
        h = wp.OpenPrinter(printer)
    except Exception as e:
        log(f"✗ 打开打印机 [{printer}] 失败:", e)
        return False
    try:
        wp.StartDocPrinter(h, 1, ("CloudPrintBox", None, "RAW"))
        wp.StartPagePrinter(h)
        wp.WritePrinter(h, data)
        wp.EndPagePrinter(h)
        wp.EndDocPrinter(h)
        log(f"✓ RAW 已投递 {len(data)} 字节 -> {printer}")
        return True
    except Exception as e:
        log("✗ RAW 投递失败:", e)
        return False
    finally:
        try:
            wp.ClosePrinter(h)
        except Exception:
            pass


def find_sumatra():
    for p in SUMATRA_CANDIDATES:
        if os.path.isfile(p):
            return p
    exe = shutil.which("SumatraPDF")
    return exe


def pdf_print(path: Path, printer: str | None = None) -> bool:
    printer = printer or resolve_printer()
    if DRY_RUN:
        log(f"[dry-run] 不送打印，成品已存: {path.name}")
        return True
    sp = find_sumatra()
    if sp:
        cmd = [sp, "-print-to", printer, "-silent", str(path)]
        try:
            subprocess.run(cmd, check=False, timeout=60)
            log(f"✓ PDF 已送打 -> {printer}")
            return True
        except Exception as e:
            log("✗ SumatraPDF 打印失败:", e)
    # 降级：交给系统默认关联程序
    if DEPS["win32api"]:
        try:
            DEPS["win32api"].ShellExecute(0, "print", str(path), None, ".", 0)
            log("✓ 已调用系统默认程序打印（可能弹窗）")
            return True
        except Exception as e:
            log("✗ ShellExecute 打印失败:", e)
    else:
        try:
            os.startfile(str(path), "print")
            return True
        except Exception as e:
            log("✗ os.startfile 打印失败:", e)
    return False


def escpos_to_image(data: bytes):
    """把 ESC/POS 流里的 GS v 0 光栅块抽出来，纵向拼成一张图"""
    Image = DEPS["Image"]
    if not Image:
        return None

    blocks = []
    i = 0
    n = len(data)
    while i < n - 8:
        if data[i] == 0x1D and data[i + 1] == 0x76 and data[i + 2] == 0x30:
            xL, xH = data[i + 4], data[i + 5]
            yL, yH = data[i + 6], data[i + 7]
            wb = xL | (xH << 8)          # 每行字节数
            h = yL | (yH << 8)           # 行数
            start = i + 8
            need = wb * h
            if wb <= 0 or h <= 0 or start + need > n:
                i += 1
                continue
            blocks.append((wb, h, data[start:start + need]))
            i = start + need
        else:
            i += 1

    if not blocks:
        return None

    total_w = max(b[0] for b in blocks) * 8
    total_h = sum(b[1] for b in blocks)
    img = Image.new("1", (total_w, total_h), 1)   # 1 = 白

    y_off = 0
    for wb, h, payload in blocks:
        for row in range(h):
            line = payload[row * wb:(row + 1) * wb]
            for bx, byte in enumerate(line):
                if byte == 0:
                    continue
                for bit in range(8):
                    if byte & (0x80 >> bit):
                        x = bx * 8 + bit
                        if x < total_w:
                            img.putpixel((x, y_off + row), 0)
        y_off += h

    log(f"  解析到位图 {total_w}x{total_h} 点，{len(blocks)} 个光栅块")
    return img


def image_print(img, out_stem: Path, printer: str | None = None) -> bool:
    """把 PIL 图存成 PDF 再静默打印（DPF 保持位图尺寸，不糊）"""
    Image = DEPS["Image"]
    if not Image:
        log("✗ 缺 pillow，无法处理图片")
        return False

    if img.mode != "1":
        img = img.convert("1")

    if SCALE == "fit":
        # 按 203dpi 还原实际尺寸（快递面单通常是 203dpi）
        pdf_path = out_stem.with_suffix(".pdf")
        img.save(pdf_path, "PDF", resolution=203.0)
        log(f"  位图 -> PDF: {pdf_path.name}")
        return pdf_print(pdf_path, printer)
    else:
        png_path = out_stem.with_suffix(".png")
        img.save(png_path)
        log(f"  位图 -> PNG: {png_path.name}")
        return pdf_print(png_path, printer)


# -------------------------- 主处理 --------------------------
_job_lock = threading.Lock()
_job_buf: dict[str, bytearray] = {}
_job_ts: dict[str, float] = {}


def is_empty_job(data: bytes) -> bool:
    """全 0 / 全 FF / 几乎没有可打印字符 —— 这些是 BLE 分段传输里的填充包，
    不是真内容。不拦掉的话会走文本分支打出一张白纸。"""
    if not data:
        return True
    s = data[:65536]
    if s.count(0) / len(s) > 0.98 or s.count(0xFF) / len(s) > 0.98:
        return True
    ok = sum(1 for b in s if 32 <= b < 127 or b in (9, 10, 13) or b >= 0x81)
    return ok / len(s) < 0.05


def handle_job(job_id: str, data: bytes):
    if is_empty_job(data):
        log(f"[job {job_id}] {len(data)} 字节全为填充数据，丢弃（不存盘不打印）")
        return
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = SAVE_DIR / f"{stamp}_{job_id}"

    kind = sniff(data)
    bin_path = base.with_suffix(".bin")
    bin_path.write_bytes(data)
    log(f"[job {job_id}] {len(data)} 字节，识别为 {kind}，已存 {bin_path.name}")

    printer = resolve_printer()
    mode = MODE if MODE != "auto" else None

    # 快递面单：CPCL 指令流 → 高分辨率重绘 → PDF
    if kind == "cpcl":
        if cpcl_to_pdf is None:
            log("✗ 缺 CPCL 渲染模块（tools/cpcl_to_pdf.py），仅存 .bin")
            return
        pdf_path = pdf_path_for(base)
        try:
            cpcl_to_pdf(bin_path, pdf_path, dpi=PDF_DPI, paper=PDF_PAPER)
        except Exception as e:
            log("✗ CPCL 转 PDF 失败:", e)
            return
        if PDF_PRINT:
            pdf_print(pdf_path, printer)
        else:
            log(f"  面单已存 PDF，未打印: {pdf_path}")
        return

    if mode == "raw" or (mode is None and kind in ("escpos",)):
        if kind == "escpos":
            # 如果是纯位图流且本机是 A4/普通打印机，RAW 直投会出乱码，
            # 所以这里先把位图抽出来渲染成页面；抽不出来才退回 RAW
            img = escpos_to_image(data)
            if img is not None:
                log("  含光栅位图，按页面渲染打印")
                image_print(img, base, printer)
                return
            log("  未找到光栅位图，按 RAW 直投（需要本机就是热敏机）")
        raw_print(data, printer)
        return

    if kind == "pdf":
        pdf_print(bin_path, printer)
        return

    if kind in ("png", "jpeg"):
        p = bin_path.with_suffix(".png" if kind == "png" else ".jpg")
        p.write_bytes(data)
        pdf_print(p, printer)
        return

    if kind in ("text", "gbk_text"):
        enc = "utf-8" if kind == "text" else "gbk"
        txt = data.decode(enc, errors="replace")
        log("  文本内容预览:", txt[:120].replace("\n", " / "))
        # 文本 -> PDF（用 PIL 排版）
        Image = DEPS["Image"]
        if Image:
            from PIL import ImageDraw, ImageFont
            W, line_h = 1240, 44
            lines = []
            for raw_line in txt.splitlines() or [""]:
                lines.append(raw_line)
            H = max(line_h * (len(lines) + 4), 400)
            im = Image.new("RGB", (W, H), "white")
            dr = ImageDraw.Draw(im)
            try:
                font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 32)
            except Exception:
                font = ImageFont.load_default()
            y = 40
            for ln in lines:
                dr.text((40, y), ln, fill="black", font=font)
                y += line_h
            pdf_path = base.with_suffix(".pdf")
            im.save(pdf_path, "PDF", resolution=203.0)
            pdf_print(pdf_path, printer)
        else:
            log("✗ 缺 pillow，无法排版文本")
        return

    log(f"  未知类型 {kind}，仅存盘，未打印")


def flush_job(job_id: str, reason: str):
    with _job_lock:
        buf = _job_buf.pop(job_id, None)
        _job_ts.pop(job_id, None)
    if buf is None:
        return
    log(f"[job {job_id}] 收工（{reason}），共 {len(buf)} 字节")
    try:
        handle_job(job_id, bytes(buf))
    except Exception as e:
        log(f"✗ 处理 {job_id} 异常:", repr(e))


def idle_watcher():
    while True:
        time.sleep(1.0)
        now = time.time()
        with _job_lock:
            expired = [k for k, t in _job_ts.items() if now - t > IDLE_FLUSH_SEC]
        for k in expired:
            flush_job(k, "空闲超时")


# -------------------------- HTTP --------------------------
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code: int, body: str):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.startswith("/printers"):
            ps = list_printers()
            dflt = default_printer()
            lines = [f"默认打印机: {dflt or '(无)'}", ""]
            for p in ps:
                lines.append(("* " if p == dflt else "  ") + p)
            self._send(200, "\n".join(lines))
        elif self.path.startswith("/health"):
            self._send(200, "ok")
        else:
            self._send(404, "use POST /print , GET /printers , GET /health")

    def do_POST(self):
        if not self.path.startswith("/print"):
            self._send(404, "not found")
            return

        length = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(length) if length else b""

        job_id = self.headers.get("X-Job-Id", "0")
        seq = self.headers.get("X-Chunk-Seq", "0")
        final = self.headers.get("X-Final", "0") in ("1", "true", "True")
        device = self.headers.get("X-Device", "?")

        log(f"<- {device} job={job_id} seq={seq} final={final} {len(body)}B")

        if final:
            with _job_lock:
                buf = _job_buf.setdefault(job_id, bytearray())
                buf += body
            flush_job(job_id, f"收到终结段 seq={seq}")
        else:
            with _job_lock:
                _job_buf.setdefault(job_id, bytearray()).extend(body)
                _job_ts[job_id] = time.time()

        self._send(200, "ok")

    def log_message(self, fmt, *args):
        pass    # 自己打日志，关掉默认的


def main():
    global PRINTER_NAME, MODE, SCALE, LISTEN_PORT
    global DRY_RUN, PDF_DIR, PDF_DATE_TREE

    import argparse
    ap = argparse.ArgumentParser(description="云打印盒 · 电脑端打印代理")
    ap.add_argument("--printer", default=PRINTER_NAME,
                    help="打印机名字，留空用系统默认。名字见 /printers")
    ap.add_argument("--mode", default=MODE, choices=["auto", "raw", "pdf", "image"])
    ap.add_argument("--scale", default=SCALE, choices=["fit", "native"],
                    help="fit=缩放到页面(A4)  native=原尺寸(标签机)")
    ap.add_argument("--port", type=int, default=LISTEN_PORT)
    ap.add_argument("--pdf-dir", default=PDF_DIR,
                    help="面单 PDF 存哪个目录，留空=跟 .bin 一起放 jobs/")
    ap.add_argument("--pdf-month", action="store_true",
                    help="PDF 按月份分文件夹（2026-11/）")
    ap.add_argument("--dry-run", action="store_true", help="只解析存盘，不真打")
    a, _ = ap.parse_known_args()
    PRINTER_NAME, MODE, SCALE, LISTEN_PORT = a.printer, a.mode, a.scale, a.port

    DRY_RUN = a.dry_run
    PDF_DIR = a.pdf_dir
    PDF_DATE_TREE = a.pdf_month

    log("=" * 52)
    log(" 云打印盒 · 电脑端打印代理")
    log("=" * 52)
    log(f" 监听      : {LISTEN_HOST}:{LISTEN_PORT}")
    log(f" 打印机    : {PRINTER_NAME or '(系统默认) ' + str(default_printer())}")
    log(f" 模式      : {MODE}   缩放: {SCALE}")
    log(f" 存盘目录  : {SAVE_DIR}")
    log(f" PDF 目录  : {PDF_DIR or '(与上面相同)'}"
        f"{'  按月分文件夹' if PDF_DATE_TREE else ''}")
    log(f" pywin32   : {'OK' if DEPS['win32print'] else '缺失 -> pip install pywin32'}")
    log(f" pillow    : {'OK' if DEPS['Image'] else '缺失 -> pip install pillow'}")
    log(f" Sumatra   : {find_sumatra() or '未安装（会降级到系统打印）'}")
    if not DEPS["win32print"]:
        log(" ⚠ 没有 pywin32 什么都打不出来，先装依赖")
    log("=" * 52)

    t = threading.Thread(target=idle_watcher, daemon=True)
    t.start()

    srv = ThreadingHTTPServer((LISTEN_HOST, LISTEN_PORT), Handler)
    log(" 就绪，等 ESP32 发数据过来\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("收到 Ctrl+C，退出")
        srv.shutdown()


if __name__ == "__main__":
    main()
