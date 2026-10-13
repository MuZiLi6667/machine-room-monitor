# -*- coding: utf-8 -*-
"""把 KiCad 导出的 SVG 渲染成 PNG（用 PIL 画 path/text 的几何）。
用法: python render_svg.py <in.svg> <out.png> [scale] [x0 y0 x1 y1(mm)]
"""
import re
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\arial.ttf",
]


def load_font(size):
    for p in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(p, int(size))
        except Exception:
            continue
    return ImageFont.load_default()


def parse_path(d):
    """把 SVG path 的 d 拆成线段列表 [(x1,y1,x2,y2), ...]。"""
    segs = []
    cur = None
    start = None
    # 拆分命令
    for cmd in re.finditer(r'([MLZmlzAHVahv])\s*([^MLZAHVmlzahv]*)', d):
        op = cmd.group(1)
        nums = [float(x) for x in re.findall(r'-?\d+\.?\d*(?:e-?\d+)?', cmd.group(2))]
        if op in "Mm":
            pts = list(zip(nums[0::2], nums[1::2]))
            if pts:
                cur = pts[0]
                start = cur
                for p in pts[1:]:
                    segs.append((cur[0], cur[1], p[0], p[1]))
                    cur = p
        elif op in "Ll":
            pts = list(zip(nums[0::2], nums[1::2]))
            for p in pts:
                if cur:
                    segs.append((cur[0], cur[1], p[0], p[1]))
                cur = p
        elif op in "Aa":
            # 椭圆弧：取末尾两个数当终点，用直线近似
            if len(nums) >= 7 and cur:
                ex, ey = nums[-2], nums[-1]
                segs.append((cur[0], cur[1], ex, ey))
                cur = (ex, ey)
        elif op in "Zz":
            if cur and start:
                segs.append((cur[0], cur[1], start[0], start[1]))
                cur = start
        elif op in "Hh":
            for v in nums:
                if cur:
                    segs.append((cur[0], cur[1], v, cur[1]))
                    cur = (v, cur[1])
        elif op in "Vv":
            for v in nums:
                if cur:
                    segs.append((cur[0], cur[1], cur[0], v))
                    cur = (cur[0], v)
    return segs


def render(svg_path, out_png, scale=4.0, crop=None, draw_text=True):
    src = open(svg_path, encoding="utf-8").read()
    x0, y0, x1, y1 = crop if crop else (0.0, 0.0, 419.989, 297.0022)
    W = max(1, int((x1 - x0) * scale))
    H = max(1, int((y1 - y0) * scale))
    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)

    def tx(x, y):
        return ((x - x0) * scale, (y - y0) * scale)

    n = 0
    for m in re.finditer(r'<path[^>]*?d="([^"]*)"', src):
        for (ax, ay, bx, by) in parse_path(m.group(1)):
            d.line([tx(ax, ay), tx(bx, by)], fill=(20, 20, 20), width=1)
            n += 1
    for m in re.finditer(r'<rect x="([-\d.]+)" y="([-\d.]+)" width="([-\d.]+)" height="([-\d.]+)"[^>]*/?>', src):
        rx, ry, rw, rh = (float(m.group(i)) for i in range(1, 5))
        if rw > 400 and rh > 250:
            continue        # 跳过整张纸的底色矩形
        d.rectangle([tx(rx, ry), tx(rx + rw, ry + rh)], outline=(20, 20, 20), width=1)
    for m in re.finditer(r'<circle cx="([-\d.]+)" cy="([-\d.]+)" r="([-\d.]+)"', src):
        cx, cy, r = float(m.group(1)), float(m.group(2)), float(m.group(3))
        px, py = tx(cx, cy)
        rr = max(1.0, r * scale)
        d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=(20, 20, 20))
    if draw_text:
        for m in re.finditer(r'<text([^>]*)>([^<]*)</text>', src):
            attrs, content = m.group(1), m.group(2)
            if 'opacity="0"' in attrs or not content.strip():
                continue
            mx = re.search(r'x="([-\d.]+)"', attrs)
            my = re.search(r'y="([-\d.]+)"', attrs)
            ms = re.search(r'font-size="([-\d.]+)"', attrs)
            if not (mx and my):
                continue
            px, py = tx(float(mx.group(1)), float(my.group(1)))
            size = float(ms.group(1)) * scale if ms else 3.0
            if size < 5:
                continue
            try:
                f = load_font(size * 1.25)
                d.text((px, py - size * 1.1), content, fill=(150, 0, 0), font=f)
            except Exception:
                pass
    img.save(out_png)
    print(f"{out_png}  {W}x{H}  lines={n}")
    return out_png


if __name__ == "__main__":
    svg = sys.argv[1]
    o = sys.argv[2]
    sc = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
    cp = tuple(float(v) for v in sys.argv[4:8]) if len(sys.argv) > 7 else None
    render(svg, o, sc, cp)
