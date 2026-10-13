# -*- coding: utf-8 -*-
"""对照实验：找出 U1 顶部引脚（VBAT/VDD/VDDA）连不上的原因。

假设 H1: 「U1 位置不在 2.54mm 网格上」导致 KiCad 内部坐标舍入不一致。
假设 H2: 「wire 端点→引脚」这种接法本身没问题，问题在别处。
"""
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kisch import Sch

CLI = r"D:\KiCad\10.0.6\bin\kicad-cli.exe"
TMP = r"C:\Users\15373\AppData\Local\Temp\kicad_exp\pin"
os.makedirs(TMP, exist_ok=True)


def build(tag, ux, uy, wire_end):
    sch = Sch(paper="A4")
    MCU = sch.load("MCU_ST_STM32F1", "STM32F103C8Tx")
    sch.place(MCU, "U1", "STM32F103C8Tx", ux, uy)
    # VDD=pin36 在库坐标 (0,40.64)；世界坐标 = (ux, uy-40.64)
    py = uy - 40.64
    sch.wire((ux, py), (ux, wire_end))
    sch.power("+3V3", ux, wire_end)
    p = os.path.join(TMP, tag + ".kicad_sch")
    sch.save(p)
    net = os.path.join(TMP, tag + ".net")
    r = subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                        "-o", net, p], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return f"LOAD FAIL {(r.stdout or '')[:60]}"
    t = open(net, encoding="utf-8").read()
    i = t.find("(nets")
    body = t[i:]
    for m in re.finditer(r'\(name "([^"]+)"\)(.*?)(?=\(net\b|\Z)', body, re.S):
        nodes = re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', m.group(2))
        if "U1" in [n[0] for n in nodes]:
            return f"接上了! 网络={m.group(1)} 节点={[f'{a}.{b}' for a,b in nodes]}"
    return "U1 未出现在任何网络（未连接）"


print("exp1  U1(150,148)   wire 到 100mm:      ", build("exp1", 150, 148, 100))
print("exp2  U1(149.86,147.32) 网格对齐:       ", build("exp2", 149.86, 147.32, 100))
print("exp3  U1(150.86,146.05)? 149.86:        ", build("exp3", 149.86, 147.32, 94))
print("exp4  U1(150,148)  wire 到 94mm:        ", build("exp4", 150, 148, 94))
print("exp5  U1(152.4,147.32) 2.54网格:        ", build("exp5", 152.4, 147.32, 100))
