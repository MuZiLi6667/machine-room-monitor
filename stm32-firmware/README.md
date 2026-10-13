# stm32-firmware — 机房监测 STM32 端固件

任务A交付物：传感器采集（DHT11 / MQ-2 / BH1750）+ 串口帧协议发送（USART1, 115200-8-N-1, 2s 周期）。
帧格式与 `docs/接口规范.md` §2 **逐字节一致**。

## 文件结构

```
stm32-firmware/
├── User/
│   ├── main.c      主循环：2s 采集→组帧→发送；SELF_TEST 宏切换测试向量自检模式
│   ├── frame.c/h   组帧与校验和（接口规范 §2 的唯一契约，勿改字节内容）
│   ├── usart1.c/h  USART1 PA9/PA10，115200-8-N-1
│   ├── dht11.c/h   DHT11 单总线驱动（PB5），失败保持上次有效值
│   ├── mq2.c/h     MQ-2 ADC 驱动（PA0），8 次采样中值滤波
│   ├── bh1750.c/h  BH1750 光照（PB6/PB7 软件 I2C），lux 换算
│   └── delay.c/h   SysTick 微秒/毫秒延时
├── 接线表.md       引脚对照 + MQ-2 分压说明（必看）
└── README.md
```

## Keil 工程搭建（MDK5 + STM32F1xx DFP）

1. 新建工程，器件选 **STM32F103C8**，勾选 CMSIS-CORE 与 StdPeriph Drivers 的 GPIO/RCC/USART/ADC/Misc（或手动加入 STM32F10x_StdPeriph_Lib 源文件）；
2. 把 `User/` 下全部 .c 加入工程，Include 路径加入 `User/` 与标准库 `Libraries/STM32F10x_StdPeriph_Driver/inc`、`Libraries/CMSIS/...`；
3. 预定义宏：`STM32F10X_MD,USE_STDPERIPH_DRIVER`；
4. **Options → C/C++ → Encoding 选 UTF-8**（源文件含中文注释）；
5. 编译应为 0 Error 0 Warning，生成 hex 用 ST-Link（SWD）烧录。

## 验收自测步骤（对应任务书）

| 步骤 | 操作 | 通过标准 |
|---|---|---|
| A2.1 | 串口助手 115200，HEX 显示 | 上电能收到数据 |
| A2.2/A2.3 | `main.c` 中 `SELF_TEST` 置 1 烧录 | 收到的三帧与接口规范 V1/V2/V3 **逐字节一致** |
| A3.1 | 恢复正常模式，手捂 DHT11 | 温湿度随环境变化，读数±3℃内；拔下 DHT11 数值保持不跳变 |
| A3.2 | 打火机放气靠近 MQ-2（预热后） | 烟雾值明显上升，洁净空气 < 400 |
| A3.3 | 手电照射 / 遮挡 BH1750 | > 1000 lux / < 50 lux |
| §4 | 连续观察 10 分钟 | 每 2s 一帧，帧长固定，抽查 3 帧校验和正确 |

测试向量（硬编码于自检模式）：
- V1: `AA 55 01 0D 04 01 01 00 02 02 26 03 01 38 04 01 64 E3`
- V2: `AA 55 01 07 02 01 00 C8 02 01 C2 98`
- V3: `AA 55 01 07 02 01 FF CC 02 02 58 32`

## 数值缩放约定（依接口规范测试向量）

| 传感器 | 帧内数值 | 例 |
|---|---|---|
| 温度(1) / 湿度(2) | 真实值 × 10（int16 大端，负值用补码） | 25.6℃ → 0x0100；-5.2℃ → 0xFFCC |
| 烟雾(3) / 光照(4) | 原始整数（不缩放） | 312 → 0x0138；356 lux → 0x0164 |
