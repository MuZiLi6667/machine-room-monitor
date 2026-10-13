# 机房环境监测系统（单片机端）原理图

本目录是「单片机端」的原理图交付，包含 **Proteus** 和 **KiCad** 两种格式，
电气连接均与 `stm32-firmware/接线表.md` 一致。

## 交付文件一览

| 文件 | 说明 |
| --- | --- |
| `output/machine-room-monitor-schematic.pdsprj` | **Proteus 工程**（双击用 Proteus 打开） |
| `output/machine-room-monitor-schematic.PDF` | 原理图 PDF（由 Proteus 导出，可直接看/打印/交作业） |
| `output/machine-room-monitor-schematic.png` | 原理图图片版（200dpi，手机/微信预览用） |
| `output/machine-room-monitor-schematic.kicad_sch` | KiCad 10 原理图（备用格式，可继续编辑/改 PCB） |
| `output/machine-room-monitor-schematic.net` | KiCad 导出的网表（校验用，不必打开） |
| `output/erc.rpt` | KiCad 的 ERC 电气规则检查报告（0 个错误） |
| `output/svg/machine-room-monitor-schematic.svg` | KiCad 版矢量图（浏览器可直接看） |

## 怎么打开（一步一步来）

### 看 Proteus 版

1. 打开「文件资源管理器」，进到
   `C:\Users\15373\WorkBuddy\2026-09-21-15-10-23\machine-room-monitor\hardware\output`
2. **双击** `machine-room-monitor-schematic.pdsprj`
3. 应该看到：Proteus 启动，标题栏写着
   `machine-room-monitor-schematic - Proteus 8 Professional - 原理图绘制`
4. 如果只看到局部：按一下 **F8**（查看整张图纸），或菜单「视图(V) → 查看整张图纸」

> 双击没反应时：先打开 Proteus，菜单「文件(F) → 打开工程(O)」，选这个 `.pdsprj` 文件。

### 看 PDF 版

直接双击 `machine-room-monitor-schematic.PDF`，任何 PDF 阅读器都能打开，
内容和 Proteus 版一模一样，适合打印和交作业。

### 看 KiCad 版

双击 `machine-room-monitor-schematic.kicad_sch`（需已装 KiCad 10）。
快速浏览用浏览器打开 `output/svg/machine-room-monitor-schematic.svg` 即可。

## 电路内容（对照接线表）

### 核心：U1 = STM32F103C8T6（Blue Pill 最小系统）

| 部分 | 元件 | 接到哪里 |
| --- | --- | --- |
| 复位电路 | R1 10k 上拉 + SW1 按键 + C3 100nF | U1 的 NRST(7) |
| BOOT0 | R2 10k 下拉 | U1 的 BOOT0(44) |
| 主晶振 | Y1 8MHz + C1/C2 22pF | U1 的 OSCIN(5) / OSCOUT(6) |
| 去耦 | C4 100nF | U1 的 VDDA / VCC |
| DHT11 上拉 | R3 4.7k | U1 的 PB5，与 DHT11 的 DATA 并联 |

### 传感器与信号

| 功能 | 图上表示 | U1 引脚 |
| --- | --- | --- |
| DHT11 温湿度 | 器件 U2（DATA/VDD/GND） | PB5（数据） |
| MQ-2 烟雾 | 电位器 RV1 模拟分压输出 | PA0-WKUP（ADC） |
| BH1750 光照 | 端子 SCL / SDA（I2C，模块外接） | PB6 / PB7 |
| USB-TTL 串口 | 端子 TXD / RXD | PA9 / PA10（USART1） |
| SWD 下载调试 | 端子 SWDIO / SWCLK | PA13 / PA14 |
| 电源 | VCC 端子 = 3.3V，GND 端子 | VDD/VDDA/VBAT、VSS/VSSA |

### 小提示

- **DHT11 符号上的文字是倒的**：因为要让引脚朝右正对 PB5，整个符号旋转了 180°，
  Proteus 的符号内标注会跟着转 —— 这是 Proteus 库的固有行为，不影响电气连接。
  介意的话可以在 Proteus 里把 U2 的属性文字（U2 / DHT11）拖到旁边正立显示。
- **个别标注文字（如 C1、R2 的值）可能与导线挤在一起**：在 Proteus 里用鼠标拖开即可，
  不影响连接。
- **仿真注意**：这是原理图设计。要用 Proteus 仿真跑固件时，把 `stm32-firmware` 编译出的
  `.hex` 加载到 U1（双击 U1 → Program File）。DHT11 在 Proteus 中通常没有仿真模型，
  仿真时可用 RV1 那种电位器方式模拟，或直接看串口输出。

## 改图 / 重新生成

两个版本都有生成脚本，改完接线表后重跑即可（本机已配好环境）：

| 版本 | 脚本 | 说明 |
| --- | --- | --- |
| KiCad | `tools/gen_sch.py` | 生成后可用 `tools/verify.py` 自动核对网络 |
| Proteus | `tools/proteus/gen_proteus.py` | 依赖 `proteus-automatic-api`（装在临时 venv 里） |

KiCad 命令行检查（在 `hardware` 目录里敲）：

```
kicad-cli sch erc output/machine-room-monitor-schematic.kicad_sch -o output/erc.rpt
```

期望输出：`0 Errors`（4 个 Warning 是“module 库未配置”的提示，可忽略）。
