# -*- coding: utf-8 -*-
"""变体实验：t_35 基础上换不同的 wire 内容，定位失败原因。"""
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bisect_sch import top_blocks

CLI = r"D:\KiCad\10.0.6\bin\kicad-cli.exe"
TMP = r"C:\Users\15373\AppData\Local\Temp\kicad_exp\bisect"
SRC = os.path.join(TMP, "t_35.kicad_sch")

text = open(SRC, encoding="utf-8").read()
blocks = top_blocks(text)
head = blocks[:-1]      # t_35 = head + 最后一块是 power GND? 直接取除最后一块
# 实际上 t_35 的最后一个块就是 block 35 的内容；我们保留它
head = blocks

orig = top_blocks(open(os.path.join(TMP, "t_36.kicad_sch"), encoding="utf-8").read())[-1]


def make_wire(pts, tag):
    lines = ["\t(wire", "\t\t(pts"]
    for (x, y) in pts:
        lines.append(f"\t\t\t(xy {x} {y})")
    lines.append("\t\t)")
    lines.append("\t\t(stroke")
    lines.append("\t\t\t(width 0)")
    lines.append("\t\t\t(type default)")
    lines.append("\t\t)")
    lines.append(f'\t\t(uuid "00000000-0000-4000-8000-{abs(hash(tag)) % 10**12:012d}")')
    lines.append("\t)")
    return "\n".join(lines)


variants = {
    "v1_2pts":   [(132.22, 125.14), (128, 125.14)],
    "v2_3pts":   [(132.22, 125.14), (128, 125.14), (128, 122.6)],
    "v3_4pts":   [(132.22, 125.14), (128, 125.14), (128, 122.6), (103.81, 122.6)],
    "v4_int":    [(132.22, 125.14), (128, 125.14), (128, 122.6), (104, 122.6)],
}

for tag, pts in variants.items():
    p = os.path.join(TMP, tag + ".kicad_sch")
    with open(p, "w", encoding="utf-8") as f:
        f.write("(kicad_sch\n")
        f.write("\n".join(head + [make_wire(pts, tag)]))
        f.write("\n)\n")
    r = subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                        "-o", os.path.join(TMP, tag + ".net"), p],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    print(f"{tag}: exit={r.returncode} {out[:120]}")

# 再单独看 original 块是否是最后一块
print("\n[t_36 最后一块]")
print(orig[:200])
