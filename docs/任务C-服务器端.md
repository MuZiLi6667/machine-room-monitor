# 任务C：Linux服务器（Ubuntu + select + 多线程 + MySQL）

> **执行者须知（Agent必读）**：先完整阅读 `docs/接口规范.md` §1、§3、§4 与本任务书。**JSON消息的解析与生成、数据库表结构必须与接口规范一致**（表结构以本文档交付的 `sql/init.sql` 为准，与接口规范§4相同）。并发模型实现方式可自由发挥（每连接一线程亦可，但必须同时体现 select 与多线程，这是课程硬性要求）。调试问题按"现象→定位→原因→解决"记录到 `docs/调试记录-C.md`。

## 0 角色与交付物

- **负责模块**：`server-linux/` — select并发 + MySQL入库 + 统计分析 + 告警推送
- **最终交付**：源码 + Makefile + `sql/init.sql` + 二进制（Ubuntu上编译） + `运行说明.md`（编译/配置/启动） + 调试记录
- **上游**：客户端（任务B）经TCP发JSON；无其他依赖

## 1 环境搭建（预计0.5天）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| C1.1 | VMware 安装 Ubuntu 22.04 LTS（https://releases.ubuntu.com/22.04/ 桌面版或Server版均可），网络设为**桥接模式**（虚拟机与宿主机同网段直连，客户端可直接连） | 虚拟机 `ping` 通宿主机，反之亦通；`ip a` 记录虚拟机IP |
| C1.2 | `sudo apt update && sudo apt install -y build-essential gdb openssh-server mysql-server libmysqlclient-dev` | `gcc --version`、`systemctl status mysql` 正常 |
| C1.3 | MySQL初始化：`sudo mysql_secure_installation`；建库：`sudo mysql < sql/init.sql` | `SHOW DATABASES;` 见 `machine_room`，四张表存在 |
| C1.4 | 防火墙放行：`sudo ufw allow 9000/tcp`（若启用ufw） | 宿主机 `nc -vz <虚拟机IP> 9000` 连通（先跑C2.1后测） |
| C1.5 | 建独立用户并授权（不用root跑程序）：`CREATE USER 'iot'@'localhost' IDENTIFIED BY 'iot123456'; GRANT ALL ON machine_room.* TO 'iot'@'localhost';` | `mysql -uiot -piot123456 machine_room -e "SELECT 1"` 成功 |

## 2 select 服务器骨架（预计1天，核心之一）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| C2.1 | 监听socket（INADDR_ANY:9000）+ `select()` 主循环（超时1s），当前阶段：收到什么回什么（echo） | 宿主机 `nc <IP> 9000` 发任意文本收到回显 |
| C2.2 | 会话结构体（fd、client_id、活跃时间、收发缓冲区）+ 数组管理；select 处理：新连接accept注册、可读读取、异常关闭清理 | 用2个终端同时 `nc` 连接互不干扰；`Ctrl-C`断开一个，另一个继续 |
| C2.3 | 按行切分接收缓冲（`\n`分帧，不完整行留存——接口规范§3），丢弃超4KB的无换行数据并回ERROR | `nc` 分两次发送同一行的两半，服务器按一条处理 |

## 3 多线程 + 业务处理（预计1.5天，核心之二）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| C3.1 | 工作线程池（4线程）+ 任务队列（互斥锁+条件变量）；主线程select后把"某fd有完整消息"投递给工作线程；fd_set/会话表的读写用互斥锁保护 | 加日志验证：主线程只收发，工作线程处理业务 |
| C3.2 | cJSON库集成（https://github.com/DaveGamble/cJSON ，下载源码放入工程编译即可）：解析消息type并分发 | 收到 `{bad` 回 `{"type":"ERROR","code":1,"msg":"malformed json"}`；未知type回code=2 |
| C3.3 | LOGIN处理：注册会话、回LOGIN_ACK；**同client_id重复登录：关闭旧fd、新连接应答成功**；未LOGIN就发DATA → 忽略或回ERROR | 用 `nc` 按接口规范§3.3三条测试向量逐条发送，应答全部符合§3.1表格 |
| C3.4 | DATA入库：一条DATA拆n个传感器值 → `INSERT INTO sensor_data` 每值一行（ts缺省/非法时用NOW()） | `server_probe.py` 连续发DATA，`SELECT * FROM sensor_data ORDER BY id DESC LIMIT 4;` 每次新增4行且数值正确 |
| C3.5 | 心跳：收到HEARTBEAT回HEARTBEAT_ACK；**90秒无任何消息** → 关闭连接回收会话 | 连上后不发任何数据，观察约90秒后被服务器断开（nc显示Connection closed） |

## 4 分析与告警（预计1.5天，核心之三）

| 步骤 | 内容 | 验收标准 |
|---|---|---|
| C4.1 | 阈值模块：启动时从 `threshold_cfg` 加载到内存；每条DATA的每个值做范围检查 | 用probe发 温度35（>26）的DATA，收到 `{"type":"ALARM","level":"HIGH","sensor":1,...}`，`alarm_log` 新增一行 |
| C4.2 | 告警推送与入库（接口规范§3.1 ALARM格式，ts用服务器时间） | 客户端（或probe）界面能收到；重复超标**每条DATA都告警**（不额外做去重，保持简单） |
| C4.3 | 分析线程：独立线程每60s运行一次 → SQL聚合统计**最近10分钟**每个在线client每个sensor的 avg/max/min/std（标准差）/over_count（超阈值次数）→ 写`analysis_result` → 组装ANALYSIS推送给对应在线客户端 | 跑10分钟后：表中有统计行；probe端每分钟收到一条ANALYSIS，数值与手工SQL核对一致 |
| C4.4 | （可选加分）命令行参数：`./server -p 9000 -t 60 -w 600`（端口/分析周期/统计窗口），默认值与规范一致 | 参数生效 |

## 5 测试与联调（预计0.5天）

- 独立测试工具：`simulator/server_probe.py`（模拟客户端：登录→2秒发DATA→收消息打印），也可 `nc` 手测
- 并发测试：3个probe实例（不同client_id）同时跑10分钟，数据互不串、无崩溃、无fd泄漏（`ls /proc/<pid>/fd | wc -l` 稳定）
- 异常测试：客户端直接断电式断开（kill -9）→ 90s内会话回收；发送半行JSON→断开→重连正常
- 三端联调：接任务B的客户端 + 任务A的单片机（见接口规范§6联调清单）

## 6 参考资料

- 接口规范：`docs/接口规范.md` §3、§4（唯一依据）
- 设计背景：`docs/系统设计方案.md` §5.3
- select/多线程教学（经典）：Beej's Guide to Network Programming https://beej.us/guide/bgnet/ （select章节）
- 多线程IoT网关参考：https://github.com/HowieXue/Linux_Gateway-ioT （多线程分工、SQLite→MySQL思路迁移）
- 线程池/MySQL参考：https://github.com/Tugbars/Threadpool-Epoll-Based-Server （epoll，其线程池与MySQL连接管理可迁移；报告可对比select与epoll差异作为亮点）
- MySQL C API手册：https://dev.mysql.com/doc/c-api/en/
- cJSON：https://github.com/DaveGamble/cJSON

## 7 完成定义（DoD）

- [ ] 接口规范§3.3三条测试向量应答正确；错误JSON回ERROR不断连
- [ ] 3客户端并发10小时（可睡眠挂起）无崩溃、无fd泄漏
- [ ] DATA入库准确；90s超时断开；重复登录踢旧
- [ ] 告警实时推送且入库；分析线程每60s产出正确统计
- [ ] `sql/init.sql` + `运行说明.md` 齐全（新人按文档5分钟内跑起来）
