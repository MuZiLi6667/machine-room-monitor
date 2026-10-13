# -*- coding: utf-8 -*-
"""生成「机房环境监测系统 · 单片机端」Proteus 原理图 (.pdsprj)。

坐标: 100000 = 1 mm，Y 向上；U1 原点在 (0,0)。
依据: stm32-firmware/接线表.md

布线约束（实测）: 导线必须沿引脚「朝外方向」离开引脚，水平引脚要从同高度
水平接入，竖直引脚要从同竖直方向接入。因此:
  - 左侧元件旋转 180° 让引脚朝右正对 U1；
  - 同一引脚的多路分支，元件排在垂直于引线方向的一列上。

依赖: pip install proteus-automatic-api  (配套 compat.py 适配本机 Proteus 8.16 库格式)
运行: python gen_proteus.py
"""
import io
import os
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compat

from proteus_automatic_api import Circuit

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', '..', 'output', 'machine-room-monitor-schematic.pdsprj')
OUT = os.path.abspath(OUT)

MM = 100000
c = Circuit(template_project=compat.TEMPLATE)
c._title = "机房环境监测系统 - 单片机端"

# ---------- 导入器件 ----------
for dev, lib in [('STM32F103C8', 'CM3_STM32'), ('DHT11', 'TRXD'),
                 ('POT-HG', 'ACTIVE'), ('CRYSTAL', 'DEVICE'), ('BUTTON', 'ACTIVE')]:
    c.import_device(dev, library=lib)

# ---------- 放置 ----------
U1 = c.add('STM32F103C8', 'U1', 'STM32F103C8', 0, 0)

# 左侧：传感器（旋转 180° 让引脚朝右正对 U1 左侧引脚）
U2 = c.add('DHT11', 'U2', 'DHT11', -70 * MM, -65 * MM)
c.rotate('U2', 180)                                          # DATA 落在 y=-59，与 PB5 同高
R3 = c.add('RESISTOR', 'R3', '4.7k', -45 * MM, -59 * MM)
c.rotate('R3', 180)                                          # DHT11 上拉，与 PB5 同高
RV1 = c.add('POT-HG', 'RV1', '10k', -70 * MM, -3 * MM)       # 模拟 MQ-2 分压输出

# 右侧：最小系统
Y1 = c.add('CRYSTAL', 'Y1', '8MHz', 75 * MM, -55 * MM)
C1 = c.add('CAPACITOR', 'C1', '22pF', 55 * MM, -66 * MM)     # pin2 朝上接 Y1.1
C2 = c.add('CAPACITOR', 'C2', '22pF', 95 * MM, -66 * MM)     # pin2 朝上接 Y1.2
R1 = c.add('RESISTOR', 'R1', '10k', 55 * MM, -3 * MM)
SW1 = c.add('BUTTON', 'SW1', 'RESET', 55 * MM, -22 * MM)
C3 = c.add('CAPACITOR', 'C3', '100nF', 55 * MM, -42 * MM)    # pin1 朝下接 NRST
R2 = c.add('RESISTOR', 'R2', '10k', 55 * MM, -84 * MM)       # BOOT0 下拉
C4 = c.add('CAPACITOR', 'C4', '100nF', 75 * MM, -78 * MM)    # VDDA 去耦

# ---------- 连线（信号） ----------
LINKS = [
    # 复位
    ('R1.1', 'U1.NRST'), ('SW1.1', 'U1.NRST'), ('C3.1', 'U1.NRST'),
    # BOOT0
    ('R2.1', 'U1.BOOT0'),
    # 晶振
    ('Y1.1', 'U1.OSCIN_PD0'), ('Y1.2', 'U1.OSCOUT_PD1'),
    ('C1.2', 'Y1.1'), ('C2.2', 'Y1.2'),
    # DHT11
    ('U2.DATA', 'U1.PB5'), ('R3.1', 'U1.PB5'),
    # MQ-2 模拟：电位器滑端 -> PA0
    ('RV1.3', 'U1.PA0-WKUP'),
]
print('元件:', [p['ref'] for p in c._parts])
for a, b in LINKS:
    c.connect(a, b)
    print(f'  连线 {a} <-> {b}')

# ---------- 电源 / 地 端子 ----------
VCC_PINS = ['U1.VDD', 'U1.VDDA', 'U1.VBAT', 'R1.2', 'R3.2', 'U2.VDD', 'RV1.2', 'C4.1']
GND_PINS = ['U1.VSS', 'U1.VSSA', 'C1.1', 'C2.1', 'C3.2', 'C4.2', 'R2.2',
            'RV1.1', 'U2.GND', 'SW1.2']
for pin in VCC_PINS:
    c.add_terminal('power', 'VCC', pin)
for pin in GND_PINS:
    c.add_terminal('ground', '', pin)

# 对外信号用同名 label 端子引出（USART1 / SWD / BH1750 的 I2C）
SIG_PINS = [('TXD', 'U1.PA9'), ('RXD', 'U1.PA10'),
            ('SWDIO', 'U1.PA13'), ('SWCLK', 'U1.PA14'),
            ('SCL', 'U1.PB6'), ('SDA', 'U1.PB7')]
for name, pin in SIG_PINS:
    c.add_terminal('label', name, pin)

c.configure_power({'GND': 0, 'VCC': 3.3}, {'VDD': 'VCC', 'VSS': 'GND'})
print('端子:', len(c.terminals()), '个')

# ---------- 保存 ----------
p = c.save(OUT, overwrite=True)
print('已保存:', p)

# ---------- 自检 ----------
print('\n=== 网络（物理导线连接）===')
for members in c.nets(physical=True):
    if len(members) > 1:
        print('  ', sorted(members))
