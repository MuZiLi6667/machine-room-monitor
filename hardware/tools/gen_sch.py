# -*- coding: utf-8 -*-
r"""生成「机房环境监测系统 · 任务A 单片机端」原理图 (.kicad_sch)。

依据: stm32-firmware/接线表.md + docs/任务A-单片机端.md + User/*.c 源码
布局: A3 图纸，裸芯片 + 完整最小系统 + 三个传感器模块 + 串口 + SWD。
规范: 所有坐标对齐 1.27mm 连接网格；电源网络配 PWR_FLAG。
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kisch import Sch, module_symbol

OUT = r"C:\Users\15373\WorkBuddy\2026-09-21-15-10-23\machine-room-monitor\hardware\output"
os.makedirs(OUT, exist_ok=True)

G = 1.27            # KiCad 连接网格


def g(v):
    """吸附到 1.27mm 网格。"""
    return round(round(v / G) * G, 4)


sch = Sch(paper="A3",
          title="机房环境监测系统 · 任务A 单片机端原理图",
          date="2026-10-09", rev="A",
          comments=(
              "主控 STM32F103C8T6（实物为 Blue Pill 开发板，板上已集成晶振/复位/稳压）",
              "USART1 PA9/PA10：115200-8-N-1，2s 周期发送传感器帧",
              "传感器：DHT11 温湿度 + MQ-2 烟雾 + BH1750 光照",
          ))

# ============ 加载官方库符号 ============
R = sch.load("Device", "R")
C = sch.load("Device", "C")
XTAL = sch.load("Device", "Crystal")
SW = sch.load("Switch", "SW_Push")
DHT = sch.load("Sensor", "DHT11")
MCU = sch.load("MCU_ST_STM32F1", "STM32F103C8Tx")
PWR_FLAG = sch.load("power", "PWR_FLAG")

# ============ 自定义模块符号 ============
MQ2_BLK, MQ2_PINS = module_symbol(
    "module:MQ-2", "U", "MQ-2", [
        ("VCC", "1", "t", 0, "power_in"),
        ("GND", "2", "b", 0, "power_in"),
        ("AO", "3", "r", 2.54, "output"),
        ("DO", "4", "r", -2.54, "output"),
    ], w=10.16, h=7.62, desc="MQ-2 烟雾传感器模块 (5V 供电)")
MQ2 = sch.add_custom("module:MQ-2", MQ2_BLK, MQ2_PINS)

BH_BLK, BH_PINS = module_symbol(
    "module:BH1750", "U", "BH1750", [
        ("VCC", "1", "t", 0, "power_in"),
        ("GND", "2", "b", 0, "power_in"),
        ("SCL", "3", "r", 2.54, "input"),
        ("SDA", "4", "r", 0, "bidirectional"),
        ("ADDR", "5", "l", -2.54, "input"),
    ], w=12.7, h=10.16, desc="BH1750 光照传感器模块 (地址 0x23)")
BH = sch.add_custom("module:BH1750", BH_BLK, BH_PINS)

TTL_BLK, TTL_PINS = module_symbol(
    "module:USB-TTL", "J", "USB-TTL (CH340)", [
        ("RXD", "1", "l", 0, "input"),
        ("TXD", "2", "l", -2.54, "output"),
        ("VCC", "3", "t", 0, "power_in"),
        ("GND", "4", "b", 0, "power_in"),
    ], w=12.7, h=10.16, desc="USB 转 TTL 串口模块 (CH340)")
TTL = sch.add_custom("module:USB-TTL", TTL_BLK, TTL_PINS)

SWD_BLK, SWD_PINS = module_symbol(
    "module:SWD", "J", "ST-Link V2 (SWD)", [
        ("SWDIO", "1", "l", 2.54, "bidirectional"),
        ("SWCLK", "2", "l", 0, "input"),
        ("3V3", "3", "t", 0, "power_in"),
        ("GND", "4", "b", 0, "power_in"),
    ], w=12.7, h=7.62, desc="SWD 下载调试接口")
SWD = sch.add_custom("module:SWD", SWD_BLK, SWD_PINS)

# ============ 元件放置 ============
# U1 落在 2.54mm 网格上，故全部引脚坐标自动落在 1.27mm 网格：
#   左引脚 x=132.08 右引脚 x=167.64 顶 y=106.68 底 y=187.96
U1 = sch.place(MCU, "U1", "STM32F103C8T6", 149.86, 147.32,
               ref_at=(172.72, 190.5, 0), val_at=(172.72, 193.04, 0))


def P(num):
    """U1 引脚的世界坐标。"""
    return U1.pin(num)


# 常用引脚（便于核对接线表）
NRST = P("7")
BOOT0 = P("44")
PD0, PD1 = P("5"), P("6")
PB5, PB6, PB7 = P("41"), P("42"), P("43")
PA0, PA9, PA10 = P("10"), P("30"), P("31")
PA13, PA14 = P("34"), P("37")

# --- 复位电路 ---
R1 = sch.place(R, "R1", "10k", 104.14, 110.49, ref_at=(106.68, 108.2, 0), val_at=(106.68, 110.7, 0))
C3 = sch.place(C, "C3", "100nF", 114.3, 118.11, ref_at=(116.84, 115.8, 0), val_at=(116.84, 118.3, 0))
SW1 = sch.place(SW, "SW1", "RESET", 76.2, 114.3, ref_at=(76.2, 108.8, 0), val_at=(76.2, 111.3, 0))

# --- BOOT0 下拉 ---
R2 = sch.place(R, "R2", "10k", 127.0, 123.19, ref_at=(129.54, 120.9, 0), val_at=(129.54, 123.4, 0))

# --- 8MHz 晶振 ---
Y1 = sch.place(XTAL, "Y1", "8MHz", 99.06, 121.92, ref_at=(99.06, 116.84, 0), val_at=(99.06, 119.3, 0))
C1 = sch.place(C, "C1", "22pF", 88.9, 125.73, ref_at=(91.44, 123.4, 0), val_at=(91.44, 125.9, 0))
C2 = sch.place(C, "C2", "22pF", 106.68, 125.73, ref_at=(109.22, 123.4, 0), val_at=(109.22, 125.9, 0))

# --- 去耦电容组（VDD×3 / VDDA / VBAT） ---
CAP_X = [166.37, 173.99, 181.61, 189.23]
CAPS = []
for i, cx in enumerate(CAP_X, start=4):
    CAPS.append(sch.place(C, f"C{i}", "100nF", cx, 97.79,
                          ref_at=(cx + 2.54, 95.5, 0), val_at=(cx + 2.54, 98.0, 0)))

# --- DHT11 ---
U2 = sch.place(DHT, "U2", "DHT11", 85.09, 152.4, ref_at=(85.09, 141.2, 0), val_at=(85.09, 143.7, 0))
R3 = sch.place(R, "R3", "4.7k", 107.95, 151.13, ref_at=(110.49, 148.8, 0), val_at=(110.49, 151.3, 0))

# --- MQ-2 + 分压（整体上移，让 PA0 走线从模块框下方通过） ---
U3 = sch.place(MQ2, "U3", "MQ-2", 248.92, 96.52, ref_at=(248.92, 83.2, 0), val_at=(248.92, 85.7, 0))
R4 = sch.place(R, "R4", "10k", 284.48, 100.33, ref_at=(287.02, 98.0, 0), val_at=(287.02, 100.5, 0))
R5 = sch.place(R, "R5", "20k", 284.48, 107.95, ref_at=(287.02, 105.6, 0), val_at=(287.02, 108.1, 0))

# --- BH1750 ---
U4 = sch.place(BH, "U4", "BH1750", 81.28, 172.72, ref_at=(81.28, 156.8, 0), val_at=(81.28, 159.3, 0))

# --- USB 转 TTL ---
J1 = sch.place(TTL, "J1", "USB-TTL (CH340)", 228.6, 137.16,
               ref_at=(228.6, 123.2, 0), val_at=(228.6, 125.7, 0))

# --- SWD 调试口 ---
J2 = sch.place(SWD, "J2", "ST-Link V2 (SWD)", 228.6, 175.26,
               ref_at=(228.6, 162.6, 0), val_at=(228.6, 165.1, 0))

# ============ 连线 ============
W = sch.wire_path
J = sch.junction

# --- 复位: NRST -> SW1.pin2 ---
W([NRST, (81.28, 114.3)])
J(104.14, 114.3)        # R1 接点
J(114.3, 114.3)         # C3 接点
sch.power("+3V3", 104.14, 106.68)              # R1 上拉到 3V3
sch.power("GND", 114.3, 121.92)                # C3 下到 GND
W([(71.12, 114.3), (71.12, 119.38)])           # SW1 另一端到 GND
sch.power("GND", 71.12, 119.38)

# --- BOOT0 下拉 ---
W([BOOT0, (127.0, 119.38)])
sch.power("GND", 127.0, 127.0)

# --- 晶振 ---
W([PD0, (127.0, 124.46), (127.0, 121.92), (102.87, 121.92)])                   # PD0 -> Y1.pin2
W([PD1, (124.46, 127.0), (124.46, 132.08), (95.25, 132.08), (95.25, 121.92)])  # PD1 -> Y1.pin1
W([(95.25, 121.92), (88.9, 121.92)])           # Y1.pin1 -> C1
J(106.68, 121.92)                              # C2 接点
J(95.25, 121.92)                               # Y1.pin1 三通
sch.power("GND", 88.9, 129.54)
sch.power("GND", 106.68, 129.54)

# --- 去耦母线: 5 个电源脚上拉到 y=93.98 ---
for px in (144.78, 147.32, 149.86, 152.4, 154.94):
    W([(px, 106.68), (px, 93.98)])
    J(px, 93.98)                               # T 型交点必须有 junction
W([(139.7, 93.98), (193.04, 93.98)])
sch.power("+3V3", 139.7, 93.98)
for cx in CAP_X:
    J(cx, 93.98)
    sch.power("GND", cx, 101.6)
# PWR_FLAG: 声明 +3V3 是电源源（母线右端）
sch.place(PWR_FLAG, "#FLG01", "PWR_FLAG", 193.04, 93.98,
          hide_value=True, hide_ref=True, in_bom=False, on_board=False)

# --- DHT11: PB5 -> DATA ---
W([PB5, (96.52, 154.94), (96.52, 152.4), (92.71, 152.4)])
J(107.95, 154.94)                              # R3 接点
sch.power("+3V3", 107.95, 147.32)              # R3 上拉
sch.power("+3V3", 85.09, 144.78)               # DHT11 VDD
sch.power("GND", 85.09, 160.02)                # DHT11 GND

# --- BH1750: PB6->SCL, PB7->SDA ---
W([PB6, (104.14, 157.48), (104.14, 170.18), (96.52, 170.18)])     # SCL
W([PB7, (109.22, 160.02), (109.22, 172.72), (96.52, 172.72)])     # SDA
sch.power("+3V3", 81.28, 160.02)               # BH1750 VCC
W([(66.04, 175.26), (62.23, 175.26)])          # ADDR -> GND
sch.power("GND", 62.23, 175.26)
sch.power("GND", 81.28, 185.42)                # BH1750 GND

# --- MQ-2 供电与分压 ---
W([(248.92, 86.36), (248.92, 82.55)])          # VCC 上引出
W([(248.92, 82.55), (256.54, 82.55)])          # 到 PWR_FLAG
sch.power("+5V", 248.92, 82.55)
sch.place(PWR_FLAG, "#FLG02", "PWR_FLAG", 256.54, 82.55,
          hide_value=True, hide_ref=True, in_bom=False, on_board=False)
sch.power("GND", 248.92, 106.68)
sch.no_connect(261.62, 99.06)                  # DO 不用
W([(261.62, 93.98), (274.32, 93.98), (274.32, 96.52), (284.48, 96.52)])     # AO -> R4.pin1
W([PA0, (279.4, 114.3), (279.4, 104.14), (284.48, 104.14)])                 # PA0 -> 分压中点
J(284.48, 104.14)                              # R4/R5 中点
sch.power("GND", 284.48, 111.76)

# --- USB 转 TTL ---
W([PA9, (213.36, 137.16)])                     # PA9 -> RXD
W([PA10, (213.36, 139.7)])                     # PA10 -> TXD
sch.no_connect(228.6, 124.46)                  # 模块 VCC 不接(板子自供电)
W([(228.6, 149.86), (238.76, 149.86)])         # 模块 GND
sch.power("GND", 238.76, 149.86)

# --- SWD ---
W([PA13, (200.66, 147.32), (200.66, 172.72), (213.36, 172.72)])   # PA13 -> SWDIO
W([PA14, (195.58, 149.86), (195.58, 175.26), (213.36, 175.26)])   # PA14 -> SWCLK
W([(228.6, 165.1), (228.6, 160.02), (248.92, 160.02)])            # 3V3 引出
sch.power("+3V3", 248.92, 160.02)
sch.power("GND", 228.6, 185.42)

# --- 底部 VSS/VSSA + GND 的 PWR_FLAG ---
W([(144.78, 187.96), (152.4, 187.96)])
J(149.86, 187.96)
sch.power("GND", 149.86, 187.96)
sch.place(PWR_FLAG, "#FLG03", "PWR_FLAG", 144.78, 187.96,
          hide_value=True, hide_ref=True, in_bom=False, on_board=False)

# ============ 未用引脚 no_connect ============
NC_LEFT = [132.08, 134.62, 137.16, 142.24, 144.78, 147.32, 149.86, 152.4,
           162.56, 165.1, 167.64, 170.18, 172.72, 175.26, 177.8, 180.34]
for y in NC_LEFT:
    sch.no_connect(132.08, y)
NC_RIGHT = [116.84, 119.38, 121.92, 124.46, 127.0, 129.54, 132.08, 134.62,
            142.24, 144.78, 152.4]
for y in NC_RIGHT:
    sch.no_connect(167.64, y)

# ============ 注释文本 ============
notes = [
    (38.1, 203.2, "【说明】"),
    (38.1, 208.28, "1. U1 为 STM32F103C8T6 裸片 + 完整最小系统（8MHz 晶振 / 复位 / BOOT0 下拉 / 去耦）。实物使用 Blue Pill 开发板，板上已集成上述电路，"),
    (38.1, 212.09, "   板上引脚丝印与本图 PA/PB 编号一致，接线按本图即可。"),
    (38.1, 217.17, "2. MQ-2 必须 5V 供电（加热丝），AO 输出满幅可达 5V，经 R4(10k)/R5(20k) 分压为 2/3 后接 PA0(ADC1_IN0)，直接连接会损坏 ADC 引脚。"),
    (38.1, 220.98, "   上电后需预热 2~3 分钟读数才稳定。"),
    (38.1, 226.06, "3. DHT11：模块版自带 4.7k 上拉；裸元件必须在 DATA 与 3V3 之间外接 R3(4.7k)。两次读取间隔 ≥1s，失败时固件保持上次有效值。"),
    (38.1, 231.14, "4. BH1750：模块自带 3.3V 稳压与 I2C 上拉，ADDR 接地 → 器件地址 0x23；SCL/SDA 走 PB6/PB7 软件 I2C。"),
    (38.1, 236.22, "5. 串口交叉连接：J1 的 RXD ← PA9(USART1_TX)，J1 的 TXD → PA10(USART1_RX)；所有模块必须共地，否则串口乱码。"),
    (38.1, 241.3, "6. Proteus 仿真：MQ-2/BH1750 库中无模型，可用 POT-HG 模拟电压输出；CH340 用 COMPIM(接 PC) 或 VIRTUAL TERMINAL(只看输出)；DHT11 有模型可拖滑条设温湿度。"),
    (38.1, 245.11, "   仿真时 STM32 模型需设 Program File 指向 Keil 生成的 .hex，Crystal Frequency 设为 8MHz。"),
]
for x, y, s in notes:
    sch.text(s, x, y)

# ============ 输出 ============
path = os.path.join(OUT, "machine-room-monitor-taskA.kicad_sch")
sch.save(path)
print("saved:", path)
