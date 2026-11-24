#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 CPCL 任务转成 PDF，供用户自己选纸型、自己打印。

为什么走 PDF 而不是直打 TSPL：
  顺丰发的是 CPCL（203dpi 坐标系，577x798 点 = 72x100mm 小面单），
  而标签机各有各的指令集和 dpi（本机 T300 是 300dpi）。
  PDF 是设备无关的，打印时可以任意缩放、选纸型，绕开了指令集与 dpi 换算。

用法：
  python cpcl_to_pdf.py <任务.bin> [输出.pdf] [--dpi 400] [--paper 100x150]
  python cpcl_to_pdf.py 面单.bin                 # 页面 = 实际尺寸 72x100mm
  python cpcl_to_pdf.py 面单.bin --paper 100x150 # 大页面，面单居中
  python cpcl_to_pdf.py 面单.bin --paper a4      # A4 居中，普通打印机也行
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from render_cpcl import CpclRenderer  # noqa: E402

# CPCL 的 577 点对应 72mm 打印宽度 → 实际分辨率
CPCL_DPI = 577 / (72.0 / 25.4)          # ≈ 203.5
PAPERS = {"a4": (210.0, 297.0), "100x150": (100.0, 150.0),
          "100x100": (100.0, 100.0), "76x100": (76.0, 100.0)}


def decode_cpcl(raw: bytes) -> str:
    """CPCL 正文里的中文是 GBK，不能一律当 UTF-8。"""
    for enc in ("gbk", "utf-8", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("gbk", "replace")


def cpcl_to_pdf(bin_path: str | Path, pdf_path: str | Path | None = None,
                dpi: int = 400, paper: str | None = None) -> Path:
    bin_path = Path(bin_path)
    text = decode_cpcl(bin_path.read_bytes())

    # 关键：按 zoom 真矢量重绘，不是位图放大 —— 否则 16 点小字打出来是糊的
    zoom = dpi / CPCL_DPI
    r = CpclRenderer(text, zoom=zoom)
    img = r.render()

    if paper:
        key = str(paper).lower().replace("*", "x").replace(" ", "")
        if key not in PAPERS:
            raise ValueError(f"不支持的纸型 {paper}，可选：{list(PAPERS)} "
                             f"或自定义 宽x高（mm）")
        if "x" in key and key not in PAPERS:
            w_mm, h_mm = (float(v) for v in key.split("x"))
        else:
            w_mm, h_mm = PAPERS[key]
        pw = max(img.width, int(round(w_mm / 25.4 * dpi)))
        ph = max(img.height, int(round(h_mm / 25.4 * dpi)))
        canvas = Image.new("L", (pw, ph), 255)
        canvas.paste(img, ((pw - img.width) // 2, (ph - img.height) // 2))
        img = canvas

    out = Path(pdf_path) if pdf_path else bin_path.with_suffix(".pdf")
    img.save(out, "PDF", resolution=float(dpi))

    mm = (img.width / dpi * 25.4, img.height / dpi * 25.4)
    print(f"PDF 已生成: {out}")
    print(f"  位图 {img.width}x{img.height} px @{dpi}dpi "
          f"→ 页面 {mm[0]:.1f} x {mm[1]:.1f} mm")
    print(f"  文件 {out.stat().st_size / 1024:.0f} KB")

    over = r.report_overlaps()
    if over:
        print(f"  提示：面单数据本身有 {len(over)} 处元素重叠（真机也一样）")
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    kw: dict = {}
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--dpi":
            kw["dpi"] = int(argv[i + 1])
        elif a == "--paper":
            kw["paper"] = argv[i + 1]
    args = [a for a in argv
            if not a.startswith("--") and a not in (str(kw.get("dpi")),
                                                    str(kw.get("paper")))]
    if not args:
        print(__doc__)
        sys.exit(1)
    cpcl_to_pdf(args[0], args[1] if len(args) > 1 else None, **kw)
