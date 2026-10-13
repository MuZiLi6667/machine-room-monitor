# 任务B 客户端 · 资产清单

- **归档目录**：项目仓库 `machine-room-monitor/client-qt/`
- **归档时间**：2026-10-10
- **资产总数**：30 个文件（源码/工程 13 + 交付 exe 2 + 验收数据 8 + 联试日志 7）

> 2026-10-13 结构整理：验收数据与联试日志共 15 个文件移入 `client-qt/验收证据/`；
> 交付 exe 与构建产物不入库（见仓库 .gitignore，重新打包方法见 README「自己编译 / 打包」）。
> 下文各表中不带路径的文件名，源码/工程在 `client-qt/` 根目录，其余在对应子目录。

---

## 一、源码与工程（13 个）

| 文件 | 说明 |
|---|---|
| `client-gui.pro` | GUI 工程文件（串口 + TCP + QCustomPlot） |
| `serialclient.pro` | B2.3 串口联试工程 |
| `main_gui.cpp` | GUI 入口（支持 `--auto` 验收模式） |
| `mainwindow.h` / `mainwindow.cpp` | 主窗口：六区布局、实时曲线、消息区、TCP 接入 |
| `protocol.h` / `protocol.cpp` | 串口帧解析器（帧头 AA55 / SUM 校验 / 大端 int16） |
| `protocol_test.cpp` | 帧解析测试：7 组标准向量、52 条断言 |
| `tcpclient.h` / `tcpclient.cpp` | TCP 模块（LOGIN / 30s 心跳 / 指数退避重连） |
| `serialclient_main.cpp` | B2.3 串口联试客户端 |
| `qcustomplot.h` / `qcustomplot.cpp` | QCustomPlot 2.1.1（第三方绘图库） |

## 二、交付可执行文件（2 个）

| 文件 | 说明 |
|---|---|
| `client.exe` | release 版客户端（B4/B5 已验证版本，约 1.19 MB） |
| `protocol_test.exe` | 帧解析测试程序 |

## 三、验收数据（8 个）

| 文件 | 阶段 | 关键结论 |
|---|---|---|
| `auto_diag.txt` / `auto_diag2.txt` | B3 | 曲线"不滚动"问题修复前后的 Y 轴范围诊断 |
| `auto_result.txt` | B3 | 界面无卡顿验收（uiTicks 30/30） |
| `b4_auto.txt` | B4 | 100s 全场景：frames=50 / dropped=0 / loginOk=2 / reconnects=2 |
| `b4_auto2.txt` | B4 | 消息推送：analysis=8 / alarm=5，与服务器逐条一致 |
| `b5_auto.txt` | B5 | 120s 全链路：dataSent=60，probe 端 2.000s/条 |
| `b5_auto2.txt` | B5 | 断线重连：loginOk=2 / reconnects=2 / dropped=0 |
| `b5_auto3.txt` | B5 | 服务器下发 ERROR，客户端不崩溃、继续发满 15 条 |

## 四、联试日志（7 个）

| 文件 | 说明 |
|---|---|
| `b4_server.log` / `b4_server2.log` | B4 假服务器日志（含 ANALYSIS / ALARM 推送记录） |
| `b5_probe.log` | B5 probe 端带到达时间戳的 DATA 记录（59 条，平均 2.000s） |
| `b5_srv2.log` / `b5_srv3.log` | B5 重连场景、ERROR 场景服务器日志 |
| `b5_longrun_srv.log` | 长跑 probe 收数记录（433 条 DATA） |
| `b5_longrun_mem.log` | 长跑内存采样（采样项为空，说明见下） |

---

## 五、未归档 / 遗留

- **未归档**：`Makefile*`、`*.o`、`moc_*`、`.qmake.stash`、`debug/`、`release/*.o` 等构建中间产物；`sim_*.log`（0 字节空文件）；`b5_longrun.txt`（长跑结果文件未生成）。
- **遗留项**：DoD 要求的"连续运行 30 分钟无崩溃、无内存暴涨"尚未取得完整证据——本次后台长跑实际运行 14.4 分钟（433 帧、严格 2 秒节奏、无断流）后进程被环境回收，且内存采样因 `tasklist` 参数在沙箱 shell 中未生效而未取到数据，建议分轮次或在本机补测。
