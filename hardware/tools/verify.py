# -*- coding: utf-8 -*-
"""自动校验：把导出的 netlist 与「接线表」期望的连接逐项比对。

发现任何"该连没连"或"不该连的连上了"（坐标冲突导致的短路）都会报出来。
"""
import re
import sys

NET = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:\Users\15373\WorkBuddy\2026-09-21-15-10-23\machine-room-monitor\hardware\output\machine-room-monitor-taskA.net"

# ---- 期望连接（依据 stm32-firmware/接线表.md）----
EXPECTED = [
    ("+3V3 电源", ["U1.1", "U1.9", "U1.24", "U1.36", "U1.48",      # VBAT/VDDA/VDD×3
                   "U2.1", "U4.1",                                  # DHT11/BH1750 供电
                   "R1.1", "R3.1",                                  # 复位/DHT11 上拉
                   "J2.3",                                          # ST-Link 3V3
                   "C4.1", "C5.1", "C6.1", "C7.1"]),                # 去耦
    ("+5V 电源", ["U3.1"]),                                          # MQ-2 加热丝
    ("GND 地", ["U1.8", "U1.23", "U1.35", "U1.47",                  # VSSA/VSS×3
                "U2.4", "U3.2", "U4.2", "U4.5",                     # 模块地 + BH1750 ADDR
                "J1.4", "J2.4",                                     # 串口/SWD 地
                "R2.2", "R5.2", "SW1.1",                            # BOOT0 下拉/分压下臂/按键
                "C1.2", "C2.2", "C3.2", "C4.2", "C5.2", "C6.2", "C7.2"]),
    ("PA0 ← MQ-2 分压中点", ["U1.10", "R4.2", "R5.1"]),
    ("PB5 ← DHT11 DATA", ["U1.41", "U2.2", "R3.2"]),
    ("PB6 → BH1750 SCL", ["U1.42", "U4.3"]),
    ("PB7 ←→ BH1750 SDA", ["U1.43", "U4.4"]),
    ("PA9 → USB-TTL RXD", ["U1.30", "J1.1"]),
    ("PA10 ← USB-TTL TXD", ["U1.31", "J1.2"]),
    ("PA13 → SWDIO", ["U1.34", "J2.1"]),
    ("PA14 → SWCLK", ["U1.37", "J2.2"]),
    ("NRST 复位", ["U1.7", "R1.2", "C3.1", "SW1.2"]),
    ("BOOT0 下拉", ["U1.44", "R2.1"]),
    ("PD0 晶振", ["U1.5", "Y1.2", "C2.1"]),
    ("PD1 晶振", ["U1.6", "Y1.1", "C1.1"]),
    ("MQ-2 AO → R4", ["U3.3", "R4.1"]),
]

EXPECTED_UNCONNECTED = [
    "J1.3",     # USB-TTL VCC 不接
    "U2.3",     # DHT11 NC
    "U3.4",     # MQ-2 DO
] + [f"U1.{n}" for n in (2, 3, 4, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                         25, 26, 27, 28, 29, 32, 33, 38, 39, 40, 45, 46)]   # 未用 IO


def parse_nets(path):
    t = open(path, encoding="utf-8").read()
    body = t[t.find("(nets"):]
    nets = {}
    cur = None
    lastref = None
    for line in body.splitlines():
        m = re.search(r'\(name "([^"]+)"\)', line)
        if m:
            cur = m.group(1)
            nets[cur] = set()
        m2 = re.search(r'\(ref "([^"]+)"\)', line)
        if m2:
            lastref = m2.group(1)
        m3 = re.search(r'\(pin "([^"]+)"\)', line)
        if m3 and cur:
            nets[cur].add(f"{lastref}.{m3.group(1)}")
    return nets


def main():
    nets = parse_nets(NET)
    pin2net = {}
    for name, pins in nets.items():
        for p in pins:
            pin2net[p] = name

    errors = []
    for label, pins in EXPECTED:
        groups = {}
        for p in pins:
            n = pin2net.get(p, "(不存在)")
            groups.setdefault(n, []).append(p)
        if len(groups) > 1:
            errors.append(f"[X] {label}: 引脚被拆到多个网络 {groups}")
            continue
        net = list(groups)[0]
        if net == "(不存在)":
            errors.append(f"[X] {label}: 引脚 {pins} 不在任何网络")
            continue
        actual = nets[net]
        extra = actual - set(pins)
        if extra:
            errors.append(f"[X] {label}: 网络 {net} 混入额外引脚 {sorted(extra)}")

    for p in EXPECTED_UNCONNECTED:
        n = pin2net.get(p)
        if n and "unconnected" not in n:
            errors.append(f"[X] {p} 应为未连接，实际在 {n}")

    n_uc = sum(1 for k in nets if "unconnected" in k)
    print(f"网络总数 {len(nets)}；未连接引脚 {n_uc}")
    if errors:
        print(f"\n发现 {len(errors)} 处问题：")
        for e in errors:
            print("  " + e)
        sys.exit(1)
    print("\n[OK] 所有连接与接线表一致，无短路、无断路、无多余连接")


if __name__ == "__main__":
    main()
