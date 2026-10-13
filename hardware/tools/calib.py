# -*- coding: utf-8 -*-
"""标定实验：验证 kisch 的旋转/坐标变换公式与 KiCad 一致。

4 个电阻，角度 0/90/180/270，每个的 pin1 接 GND、pin2 接 +3V3。
导出 netlist 后，若每个电阻的 1 脚都在 GND、2 脚都在 +3V3，则公式正确。
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kisch import Sch

OUT = r"C:\Users\15373\AppData\Local\Temp\kicad_exp"
os.makedirs(OUT, exist_ok=True)

sch = Sch(paper="A4", title="calib")
r_lib = sch.load("Device", "R")
gnd = sch.load("power", "GND")
p3v3 = sch.load("power", "+3V3")

for i, ang in enumerate([0, 90, 180, 270]):
    x = 60 + i * 40
    y = 100
    c = sch.place(r_lib, f"R{i + 1}", "10k", x, y, ang)
    p1 = c.pin("1")
    p2 = c.pin("2")
    print(f"R{i + 1} angle={ang}: pin1={p1} pin2={p2}")
    sch.place(gnd, f"#PWRG{i + 1}", "GND", p1[0], p1[1], hide_value=True)
    sch.place(p3v3, f"#PWRV{i + 1}", "+3V3", p2[0], p2[1], hide_value=True)

sch.save(os.path.join(OUT, "calib.kicad_sch"))
print("saved")
