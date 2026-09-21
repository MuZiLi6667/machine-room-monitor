# 机房环境多指标远程监测与分析系统

通信系统综合设计（DX702006）· 3人小组 · 2026.9.21 – 10.13

## 系统链路

DHT11/MQ-2/BH1750 → STM32F103C8T6 (UART帧协议) → USB转TTL → Qt客户端(串口+TCP) → VMware Ubuntu服务器(select+多线程+MySQL) → 统计分析/告警回传

## 文档导航（三人协作从这里开始）

| 文档 | 用途 |
|---|---|
| `docs/接口规范.md` | **三端契约（必读）**：串口帧协议+TCP JSON协议+数据库，含冻结的测试向量 |
| `docs/系统设计方案.md` | 总体设计：架构、选型、计划（报告底稿） |
| `docs/任务A-单片机端.md` | 分工任务书：分步任务+验收标准+参考链接 |
| `docs/任务B-客户端.md` | 分工任务书：同上 |
| `docs/任务C-服务器端.md` | 分工任务书：同上 |

> 各自的Agent工具接手时：**第一步读接口规范，第二步读自己的任务书**，接口字段不得擅改。

## 目录结构

```
machine-room-monitor/
├── docs/            文档（见上表）
├── stm32-firmware/  任务A交付：Keil工程 + hex + 接线表
├── client-qt/       任务B交付：Qt源码 + exe
├── server-linux/    任务C交付：源码 + Makefile + sql/init.sql
└── simulator/       共享联调工具（见下）
```

## 联调工具（simulator/）

```bash
# 串口模拟器（模拟STM32发帧，给客户端B联调用；需 pip install pyserial）
python simulator/stm32_sim.py -p COM4                # 随机游走数据，2秒一帧
python simulator/stm32_sim.py -p COM4 --vector v1    # 发送接口规范测试向量V1
python simulator/stm32_sim.py -p COM4 --corrupt      # 注入错误帧测客户端容错

# TCP探测工具（纯标准库）
python simulator/server_probe.py server -p 9000                  # 假服务器（测客户端）
python simulator/server_probe.py client -H <IP> --id PC-01       # 假客户端（测服务器）

# 向量自校验（开发前先跑，确认工具本身正确）
python simulator/verify_vectors.py
```

## 关键配置速查

- 串口：115200-8-N-1，帧协议见接口规范§2
- 服务器：TCP 9000（VMware 桥接）
- MySQL：库 machine_room，用户 iot/iot123456，脚本 server-linux/sql/init.sql
- 阈值：温度 18~26℃ / 湿度 40~60% / 烟雾 ADC>400 / 光照仅记录
- 联调环境：com0com 虚拟串口对（COM3↔COM4）

## 实施顺序建议

1. 各自按任务书独立开发（B/C 用 simulator 自测，不等队友）
2. B+C 先拼合（客户端↔服务器，跑通JSON链路）
3. A 真机接入替换模拟器
4. 按接口规范§6联调清单验收 → 报告
