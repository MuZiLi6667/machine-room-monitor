# 任务B：Windows客户端（Qt 5）

> **执行者须知（Agent必读）**：先完整阅读 `docs/接口规范.md` §1、§2、§3 与本任务书。**串口帧解析和JSON消息字段必须与接口规范逐项一致**。UI美观度可自由发挥，但下方"必备界面元素"不可缺。调试问题按"现象→定位→原因→解决"记录到 `docs/调试记录-B.md`。

## 0 角色与交付物

- **负责模块**：`client-qt/` — 串口接收解析 + TCP转发 + 界面显示
- **最终交付**：Qt源码工程 + release目录exe（含所需dll） + 调试记录
- **上游**：单片机（任务A）经串口发帧；**下游**：服务器（任务C）收TCP
- **两大外部接口**：串口（接口规范§2）、TCP（接口规范§3）

## 1 环境搭建（预计0.5天）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| B1.1 | 安装 Qt 5.12（官方开源版 https://www.qt.io/download-open-source ，国内可走清华镜像 https://mirrors.tuna.tsinghua.edu.cn/qt/ 下 `qt/5.12/5.12.12/` 的 offline 安装包），组件勾选 MinGW 或 MSVC 任一套件 | Qt Creator 新建工程可编译运行 |
| B1.2 | 下载 QCustomPlot（https://www.qcustomplot.com/ ，只需 qcustomplot.cpp/.h 两个文件）放入工程 | `#include "qcustomplot.h"` 编译通过 |
| B1.3 | .pro 中加 `QT += serialport network` | 无报错 |
| B1.4 | 安装虚拟串口对 com0com（https://sourceforge.net/projects/com0com/ ，安装后默认生成 COM3↔COM4 一对） | 设备管理器出现两个 com0com 端口 |
| B1.5 | Python 环境 + `pip install pyserial`（运行模拟器用） | `python simulator/stm32_sim.py -h` 正常显示帮助 |

## 2 串口帧解析器（预计1天，核心，先用测试驱动）

实现 `protocol.cpp`：输入字节流（可分次到达），输出解析结果。**建议做成纯C++类（不依赖Qt），便于单测**。

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| B2.1 | 写一个测试main（或Qt单元测试），灌入接口规范 §2.3 全部向量 | V1→4个传感器值(25.6/55.0/312/356)；V2→2个值；V3→-5.2/60.0 |
| B2.2 | 灌入异常向量E1~E4 | 错校验丢弃不崩溃；粘包两帧都解出；半帧缓冲后补齐解出；噪声前缀跳过后解出V1 |
| B2.3 | 打开COM口（QSerialPort，115200-8-N-1），与 `simulator/stm32_sim.py`（对端接虚拟串口COM3，程序连COM4）联试 | 模拟器每2秒一帧，解析器连续输出正确值，值在合理区间随机波动 |
| B2.4 | 模拟器加 `--corrupt` 参数发错帧流 | 客户端不崩溃、不错位，日志有丢帧计数 |

## 3 界面（预计1天）

必备元素（布局自定）：

1. **串口配置区**：COM口下拉（自动枚举）、打开/关闭按钮（115200-8-N-1固定或可配）
2. **连接配置区**：服务器IP、端口（默认9000）、client_id（默认主机名）、连接/断开按钮、连接状态指示
3. **实时数据区**：4路传感器当前值（大字号），异常/离线状态用颜色区分
4. **曲线区**：QCustomPlot 4条实时曲线（温度/湿度/烟雾/光照各一或分区显示），滚动窗口显示最近10分钟，横轴时间
5. **消息区**：服务器下发的 ANALYSIS 统计结果表格化展示；ALARM 告警列表（时间/级别/内容），并弹窗提示HIGH级
6. **日志区**：原始HEX（可开关）、解析结果、网络收发，可导出文件

验收：模拟器数据驱动下，曲线滚动刷新、无卡顿（UI线程不做耗时操作）。

## 4 TCP 网络模块（预计1天）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| B4.1 | QTcpSocket 连接服务器；连接成功后发 `LOGIN`，收到 `LOGIN_ACK`(status=0) 才进入"已登录"态 | 用 `nc -l 9000` 手工回 `{"type":"LOGIN_ACK","status":0}` 验证 |
| B4.2 | DATA转发：每收到一帧串口数据 → 组装JSON DATA（含ts，格式见接口规范§3.1）→ 发送 | `nc` 端能收到完整一行JSON且以\n结尾 |
| B4.3 | 心跳30s；断线检测（socket error）→ 指数退避重连 5/10/20/30s封顶 → 重连后重新LOGIN | 手工断开 `nc`，观察自动重连并重新登录 |
| B4.4 | 接收处理：解析 `ANALYSIS`/`ALARM`/`HEARTBEAT_ACK`/`ERROR`，分别驱动界面3、5区 | 手工向客户端发送§3.1中的ANALYSIS/ALARM示例，界面正确显示并弹窗 |

## 5 端到端自测（预计0.5天，不依赖A/C）

用 `simulator/server_probe.py` 充当"假服务器"（它只回LOGIN_ACK/HEARTBEAT_ACK并打印收到的数据）：
- 全链路：stm32_sim.py（COM3）→ 客户端（COM4）→ server_probe.py → 观察
- 验收：probe端2秒收到一条DATA；杀掉probe → 客户端自动重连；重启probe → 自动恢复登录并继续发数

## 6 三端联调（0.5天，与A/C合体）

1. 真机：STM32替换模拟器、真服务器替换probe
2. 联调验收（对照接口规范§6）：曲线连续；服务器发ALARM客户端弹窗；ANALYSIS表格每分钟刷新
3. 异常项：拔串口（自动检测并可重开）、断网重连、服务器发ERROR客户端不死

## 7 参考资料

- 接口规范：`docs/接口规范.md` §2、§3（唯一依据）
- 设计背景：`docs/系统设计方案.md` §5.2
- QSerialPort文档：Qt助手自带（搜索 QSerialPort）
- QCustomPlot官方示例：https://www.qcustomplot.com/index.php/support/documentation （实时数据曲线看 examples）
- 串口编程参考开源：https://github.com/Tyill/SVisual （Qt串口+曲线）、https://github.com/rymcu/RYCOM （Qt串口收发）
- Qt+TCP客户端参考：https://github.com/DXT-parsifal/Embedded-Integrated-Project （Qt端TCP部分）

## 8 完成定义（DoD）

- [ ] 解析器通过 §2.3 全部7个测试向量（V1/V2/V3/E1/E2/E3/E4）
- [ ] com0com + stm32_sim.py 连续跑30分钟无崩溃无内存暴涨
- [ ] 断线自动重连（退避正确）、重连后自动重新登录
- [ ] ALARM弹窗、ANALYSIS表格正常
- [ ] release exe 交付（含dll），调试记录完成
