#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32 串口帧模拟器 —— 按接口规范 §2 生成串口数据帧
用途：客户端在没有单片机硬件时联调；单片机端可对照输出字节是否一致
用法：
    python stm32_sim.py -p COM4                      # 正常模式：随机游走数据，2秒一帧
    python stm32_sim.py -p COM4 --corrupt            # 注入15%错误帧（测客户端容错）
    python stm32_sim.py -p COM4 --vector v1 --loop   # 周期发送接口规范测试向量V1
    python stm32_sim.py -p COM4 --vector v1          # 发送一次V1后退出
依赖：pip install pyserial
"""
import argparse
import random
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：请先执行  pip install pyserial")

FRAME_HEAD = b"\xAA\x55"
FRAME_TYPE_DATA = 0x01

VECTORS = {
    "v1": bytes.fromhex("AA 55 01 0D 04 01 01 00 02 02 26 03 01 38 04 01 64 E3"),
    "v2": bytes.fromhex("AA 55 01 07 02 01 00 C8 02 01 C2 98"),
    "v3": bytes.fromhex("AA 55 01 07 02 01 FF CC 02 02 58 32"),
}


def build_frame(sensors):
    """sensors: [(sid, value_x10_int), ...] -> bytes，与接口规范§2逐字节一致"""
    data = bytes([len(sensors)])
    for sid, v in sensors:
        data += bytes([sid]) + struct.pack(">h", v)
    body = bytes([FRAME_TYPE_DATA, len(data)]) + data
    return FRAME_HEAD + body + bytes([sum(body) & 0xFF])


def hx(b):
    return " ".join(f"{x:02X}" for x in b)


def corrupt(frame):
    """生成错误帧：坏校验 或 噪声前缀（客户端都必须能容忍）"""
    if random.random() < 0.5:
        b = bytearray(frame)
        b[-1] ^= 0xFF
        return bytes(b)
    return b"\xFF\x12" + frame


def main():
    ap = argparse.ArgumentParser(description="STM32 串口帧模拟器")
    ap.add_argument("-p", "--port", default="COM4", help="串口号，如 COM4 或 /dev/ttyUSB0")
    ap.add_argument("-i", "--interval", type=float, default=2.0, help="发帧间隔秒（默认2.0）")
    ap.add_argument("--corrupt", action="store_true", help="15%%概率注入错误帧")
    ap.add_argument("--vector", choices=list(VECTORS), help="发送接口规范测试向量(v1/v2/v3)")
    ap.add_argument("--loop", action="store_true", help="向量模式下周期发送（默认发一次退出）")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=1)
    print(f"[sim] {args.port} @115200-8-N-1, interval={args.interval}s"
          + (" [corrupt ON]" if args.corrupt else ""))

    if args.vector:
        while True:
            frame = VECTORS[args.vector]
            ser.write(frame)
            print(f"[sim] vector {args.vector}: {hx(frame)}")
            if not args.loop:
                break
            time.sleep(args.interval)
        ser.close()
        return

    # 随机游走状态（温度范围故意覆盖告警阈值26℃，便于观察告警链路）
    st = {"temp": 25.0, "hum": 55.0, "smoke": 200, "light": 350}
    n = 0
    while True:
        st["temp"] = max(20.0, min(32.0, st["temp"] + random.uniform(-0.3, 0.3)))
        st["hum"] = max(40.0, min(70.0, st["hum"] + random.uniform(-0.5, 0.5)))
        st["smoke"] = max(80, min(450, st["smoke"] + int(random.uniform(-15, 15))))
        st["light"] = max(100, min(600, st["light"] + int(random.uniform(-25, 25))))
        sensors = [
            (1, round(st["temp"] * 10)),
            (2, round(st["hum"] * 10)),
            (3, int(st["smoke"])),
            (4, int(st["light"])),
        ]
        frame = build_frame(sensors)
        if args.corrupt and random.random() < 0.15:
            frame = corrupt(frame)
        ser.write(frame)
        n += 1
        print(f"[sim #{n}] T={st['temp']:.1f} H={st['hum']:.1f} S={st['smoke']} L={st['light']}"
              f"  |  {hx(frame)}")
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[sim] stopped")
