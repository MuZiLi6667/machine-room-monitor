# 任务C执行规划：Linux服务器 分步实施框架

> **本文档的定位**：任务C的"施工图纸+进度表"。采用**增量式开发（Incremental Model）+ 验收驱动**方法：整体架构一次设计冻结 → 按垂直切片分 S0~S8 九个增量逐步交付 → 每个增量都是"可运行、可验证、可回退"的完整闭环。
>
> **给执行 Agent 的使用方式**：
> 1. 接到某一步（如"执行 S3"）时，**只需阅读该步骤的【上下文卡】列出的文件/章节 + 本规划 §2 模块接口表**，不需要通读全部文档。
> 2. 严格按【验收标准】逐条自测，全部通过后在该步骤前的复选框打勾，并在 `docs/调试记录-C.md` 记录过程。
> 3. 接口字段（JSON格式、表结构、端口、阈值）**以 `docs/接口规范.md` 为唯一依据，禁止擅改**。
>
> 进度状态：S0 ☑　S1 ☑　S2 ☑　S3 ☑　S4 ☑　S5 ☑　S6 ☑　S7 ☑　S8 ☐

---

## 1 方法论说明（为什么这样拆）

| 原则 | 在本项目的落地方式 |
|---|---|
| **整体设计，分块实现** | §2 先冻结模块划分与头文件接口，后续各步只填实现、不改接口 |
| **垂直切片** | 每一步都切穿"网络→解析→动作"完整链路（如 S4 从收 DATA 到落库一次打通），不做"先写完全部解析再写全部入库"的水平分层半成品 |
| **每步可验证** | 每步验收标准=具体命令+预期输出，用 `nc` / `server_probe.py` / SQL 立即检验 |
| **每步可回退** | 每步完成即 `git commit`（或备份目录），下一步出问题只回退一步 |
| **高内聚低耦合** | 模块间只通过头文件函数交互，禁止跨模块访问内部全局变量；每步只动 1~2 个模块 |

---

## 2 总体架构与模块接口（冻结设计，先读这一节）

### 2.1 目录结构（交付即此结构）

```
server-linux/
├── Makefile
├── sql/
│   └── init.sql            # 建库建表+阈值初始数据（与接口规范§4一致）
├── src/
│   ├── main.c              # 入口：参数解析、初始化、启动主循环
│   ├── config.h            # 全局常量（端口/超时/阈值周期等宏，唯一改配置的地方）
│   ├── log.c / log.h       # 日志模块（带时间戳+级别，printf封装）
│   ├── net.c / net.h       # select主循环、监听、收发、fd管理
│   ├── session.c / session.h # 会话表：fd↔client_id、活跃时间、接收缓冲
│   ├── proto.c / proto.h   # cJSON解析与组包、消息分发
│   ├── pool.c / pool.h     # 线程池（任务队列+互斥锁+条件变量）
│   ├── db.c / db.h         # MySQL连接与增删查封装（唯一包含mysql.h的文件）
│   ├── alarm.c / alarm.h   # 阈值缓存、超标判定、ALARM生成
│   ├── analysis.c / analysis.h # 分析线程：周期统计+ANALYSIS推送
│   └── cJSON.c / cJSON.h   # 第三方库（https://github.com/DaveGamble/cJSON）
└── 运行说明.md              # 编译/配置/启动（S8写）
```

### 2.2 模块职责与依赖规则（低耦合的关键）

```
        main.c（组装者，唯一知道所有模块的文件）
          │
 ┌────────┼─────────────────┐
 ▼        ▼                 ▼
net ──► session        pool（线程池）
 │        ▲                 │
 │        │                 ▼
 │        │              proto（解析/分发）
 │        │              ┌───┼───┐
 │        │              ▼   ▼   ▼
 └────────┴──────────── db  alarm  analysis
                        ▲   ▲      │
                        └───┴──────┘
                        （db 是唯一碰 MySQL 的模块）
```

**铁律（违反即重构）**：
1. 只有 `db.c` 可以 `#include <mysql/mysql.h>`；其他模块要数据就调 `db.h` 的函数。
2. 只有 `net.c` 可以调用 `send()/recv()/close()`；其他模块要回消息就调 `net_send(fd, buf)`。
3. 模块间不共享全局变量，只通过函数参数/返回值交互；会话表由 `session.c` 持有，外界只能调 `session_*()` 函数访问。
4. 每个模块的 `.h` 文件 = 它的"对外契约"，写实现前先写头文件。

### 2.3 头文件接口约定（开工前冻结，后续步骤只填实现）

```c
/* log.h */
void log_init(const char *tag);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err (const char *fmt, ...);

/* net.h —— select 网络层 */
int  net_init(int port);                       /* 创建监听socket */
int  net_loop(void);                           /* select主循环（阻塞，内含超时处理） */
int  net_send(int fd, const char *buf);        /* 线程安全的单连接发送 */
void net_close(int fd);                        /* 关闭并从select集合移除 */

/* session.h —— 会话管理 */
int         session_add(int fd);
void        session_remove(int fd);
int         session_login(int fd, const char *client_id); /* 含踢旧逻辑 */
int         session_is_logged(int fd);
const char *session_client_id(int fd);
void        session_touch(int fd);            /* 刷新活跃时间 */
int         session_feed(int fd, const char *data, int len); /* 写入接收缓冲，返回完整行数 */
int         session_next_line(int fd, char *out, int cap);   /* 按\n取一条完整消息 */
void        session_sweep_timeouts(int timeout_sec);         /* 清90s超时 */

/* pool.h —— 线程池 */
void pool_init(int nthreads);
void pool_submit(void (*fn)(void *), void *arg);

/* proto.h —— 协议解析与分发（工作线程内调用） */
void proto_handle(int fd, const char *json_line);  /* 入口：解析type并分发 */

/* db.h —— 数据库 */
int  db_init(const char *user, const char *pass, const char *dbname);
int  db_insert_sensor(const char *client_id, int sensor_type,
                      double value, const char *datetime /*NULL则用NOW()*/);
int  db_insert_alarm(const char *client_id, int sensor_type,
                     double value, double threshold, const char *level);
int  db_load_thresholds(void);                 /* 启动时加载 threshold_cfg 到内存 */
int  db_run_analysis(void);                    /* 聚合近10分钟→写analysis_result，返回生成行数 */

/* alarm.h —— 阈值判定与告警 */
void alarm_check(int fd, const char *client_id, int sensor_type, double value);
/* 超阈值→写alarm_log + 组装ALARM经net_send推送 */

/* analysis.h —— 分析线程 */
void *analysis_thread(void *arg);              /* 每60s：db_run_analysis + 推送ANALYSIS */
```

> 以上为**建议最小接口集**。实现时可增加内部静态函数，但不得删除/修改这些对外签名——后续步骤的验收依赖它们。

---

## 3 分步执行流程（S0 ~ S8）

### ☑ S0 环境与工程脚手架（对应任务书 §1，约0.5天）

**目标**：Ubuntu 虚拟机可用、MySQL 就绪、工程骨架能 `make` 出一个空程序。

**上下文卡（开工前只读这些）**：
- 本规划 §1、§2.1
- `docs/任务C-服务器端.md` §1（C1.1~C1.5 步骤表）
- 本步骤即完成后的 init.sql 内容 = `docs/接口规范.md` §4

**任务内容**：
1. VMware 装 Ubuntu 22.04，网络**桥接模式**，确认与宿主机互 ping 通，`ip a` 记录虚拟机 IP。
2. `sudo apt update && sudo apt install -y build-essential gdb openssh-server mysql-server libmysqlclient-dev`
3. 写 `sql/init.sql`：建库 `machine_room`(utf8mb4) + 四张表（sensor_data / analysis_result / alarm_log / threshold_cfg）+ 阈值初始数据（温度18~26/HIGH、湿度40~60/MID、烟雾>400/HIGH、光照不插行）+ 建用户 `iot`/`iot123456` 并授权。
4. `sudo mysql < sql/init.sql` 初始化。
5. 搭目录骨架（§2.1），写 `config.h`、`log.c`（先做 printf 带时间戳封装）、一个只打印启动日志的 `main.c`、能编译全部 src 的 `Makefile`（链接 `-lmysqlclient -lpthread`）。
6. `sudo ufw allow 9000/tcp`（若 ufw 启用）。
7. `git init && git commit`（或用目录备份）作为基线。

**产出**：`server-linux/` 骨架 + `sql/init.sql` + 可编译的空 `server` 二进制。

**验收标准**：
| # | 命令 | 预期 |
|---|---|---|
| S0-1 | 宿主机 `ping <虚拟机IP>`、虚拟机 `ping <宿主机IP>` | 双向通 |
| S0-2 | `gcc --version`；`systemctl status mysql` | 版本正常输出；mysql active(running) |
| S0-3 | `mysql -uiot -piot123456 machine_room -e "SHOW TABLES;"` | 列出 4 张表，不报错 |
| S0-4 | `mysql -uiot -piot123456 machine_room -e "SELECT * FROM threshold_cfg;"` | 3 行：温度/湿度/烟雾 |
| S0-5 | `cd server-linux && make clean && make` | 零警告零错误，产出 `server` |
| S0-6 | `./server` 后 Ctrl-C | 打印启动日志后正常退出 |

**常见坑**：MySQL 8.0 默认 auth 插件导致 iot 用户登录失败 → 建用户时加 `IDENTIFIED WITH mysql_native_password BY 'iot123456'`；桥接模式不通 → 检查 VMware 虚拟网络编辑器绑定的物理网卡。

---

### ☑ S1 select 回显骨架（对应任务书 C2.1~C2.3，约1天）

**目标**：`net.c` + `session.c` 两个模块成型——监听 9000、select 主循环、多连接管理、按 `\n` 分帧。此时业务=收到一行就原样回发（echo）。

**上下文卡**：
- `docs/接口规范.md` §3 开头"分帧规则"一段（仅此一段）
- 本规划 §2.2、§2.3（net/session/log 接口）
- 参考：Beej's Guide select 章节 https://beej.us/guide/bgnet/html/split/slightly-advanced-techniques.html#select

**任务内容**：
1. `net.c`：`net_init`（socket/bind/listen/设 SO_REUSEADDR）、`net_loop`（select 超时 1s；监听 fd 可读→accept 注册；客户端 fd 可读→recv；recv=0 或异常→关闭清理）。
2. `session.c`：固定数组（如 64 槽位）会话表，含 fd / client_id / 最近活跃时间 / 接收缓冲（8KB）；`session_feed` 追加缓冲 + `session_next_line` 按 `\n` 切完整行；单行 >4KB 无换行 → 丢弃该缓冲并标记。
3. 主循环内：每取到一条完整行 → 暂时直接 `net_send` 回发（echo 占位，S2 替换为投递线程池）。
4. 每 1s select 超时顺带调 `session_sweep_timeouts(90)`（本轮先只打印日志，不断开，S3 启用）。

**产出**：`src/net.c/h`、`src/session.c/h`，可编译运行。

**验收标准**：
| # | 操作 | 预期 |
|---|---|---|
| S1-1 | `./server`；宿主机 `nc <虚拟机IP> 9000`，输入 `hello` 回车 | 收到回显 `hello` |
| S1-2 | 两个终端同时 `nc`，各自发消息 | 互不干扰，各收各的回显 |
| S1-3 | 一个终端 Ctrl-C 断开 | 另一个继续正常回显；服务器日志打印连接关闭，不崩溃 |
| S1-4 | 用 `printf 'hel' \| nc` 与 `printf 'lo\n' \| nc`（或python分两次写同一行） | 服务器按一条 `hello` 处理（半行留存） |
| S1-5 | 一次发送 `a\nb\nc\n` | 收到 3 条独立回显（粘包正确切分） |
| S1-6 | 发送一行 >4KB 无换行的数据，再发正常行 | 服务器不崩溃，后续正常行仍能处理 |

**常见坑**：select 的 fd_set 每轮都要重新 FD_SET（select 会修改它）；`nc` 到虚拟机不通先查 ufw 与桥接。

---

### ☑ S2 线程池 + JSON 解析分发（对应任务书 C3.1~C3.2，约0.5天）

**目标**：`pool.c` + `proto.c`（含 cJSON 集成）。把 S1 的"echo"替换为"投递任务到线程池 → 解析 JSON → 按 type 分发"。

**上下文卡**：
- `docs/接口规范.md` §3.2 的"ERROR"条目、§3.3
- 本规划 §2.3（pool/proto 接口）
- cJSON：https://github.com/DaveGamble/cJSON （下载 cJSON.c/h 放入 src/）

**任务内容**：
1. `pool.c`：4 个工作线程 + 有界任务队列（互斥锁+条件变量，经典生产者-消费者）。任务 = `{fd, 消息字符串副本}`（**必须深拷贝消息**，指针生命周期陷阱）。
2. `net_loop` 内取到完整行后：`pool_submit(worker_fn, task)`，主线程从此不再处理业务（打日志区分主线程/工作线程验证）。
3. `proto.c`：`proto_handle(fd, line)` → `cJSON_Parse`；失败 → 回 `{"type":"ERROR","code":1,"msg":"malformed json"}`；解析成功但 `type` 缺失/未知 → 回 code=2 `unknown type`；已知 type（LOGIN/DATA/HEARTBEAT）先打日志占位（S3/S4 填实现）。
4. 回包统一封装 `proto_reply(fd, cJSON_obj)`：组 JSON 字符串 + 追加 `\n` + `net_send`。
5. **线程安全**：`net_send` 内加全局发送锁（同一 fd 的多次 send 不交错）；会话表的增删查加互斥锁。

**产出**：`src/pool.c/h`、`src/proto.c/h`、`src/cJSON.c/h`，echo 逻辑移除。

> 实现记录：`src/cJSON.c` 保留了**一处 8 行本地补丁**——把上游的进程级全局 `global_error` 改成 `__thread`。原因见 `docs/调试记录-C.md` 问题 24（Helgrind 实测：4 个工作线程并发解析时对该全局产生数据竞争）。**后续不要再从上游整份覆盖 cJSON.c**，否则竞争会复活。

**验收标准**：
| # | 输入（经 nc 或 probe） | 预期 |
|---|---|---|
| S2-1 | `{bad` | 回 `{"type":"ERROR","code":1,"msg":"malformed json"}`，**连接不断** |
| S2-2 | `{"type":"PING"}` | 回 code=2 `unknown type`，连接不断 |
| S2-3 | `{"foo":1}`（无type字段） | 回 code=2 |
| S2-4 | 服务器日志 | 主线程只做 accept/recv/投递，解析日志来自工作线程（线程id不同） |
| S2-5 | 两个 nc 并发各发 50 条消息 | 无崩溃、无乱序回包、valgrind（可选）无数据竞争报警 |

**常见坑**：任务里的消息指针指向了会话缓冲区（被后续 recv 覆盖）→ 必须 `strdup`；cJSON_Parse 后记得 `cJSON_Delete`。

---

### ☑ S3 会话协议：LOGIN / HEARTBEAT / 超时（对应任务书 C3.3、C3.5，约0.5天）

**目标**：连接生命周期完整——登录、重复登录踢旧、心跳应答、90s 超时断开。

**上下文卡**：
- `docs/接口规范.md` §3.1 表格（LOGIN/LOGIN_ACK/HEARTBEAT/HEARTBEAT_ACK 四行）、§3.2 容错条目、§3.3
- 本规划 §2.3（session/proto 接口）

**任务内容**：
1. LOGIN：`session_login` 注册 client_id（校验 1~31 字符 `[A-Za-z0-9-_]`，非法回 ERROR 不注册）；回 `{"type":"LOGIN_ACK","status":0}`；**同 client_id 已有旧连接 → 关旧 fd、新连接应答 status=0**。
2. 未 LOGIN 先发 DATA/HEARTBEAT → 回 ERROR（code=3, msg="not logged in"，自定义码即可，不断连）。
3. HEARTBEAT → 回 `{"type":"HEARTBEAT_ACK"}`。
4. 任何合法消息（LOGIN/DATA/HEARTBEAT）都 `session_touch` 刷新活跃时间；S1 里占位的 `session_sweep_timeouts(90)` 现在真正执行：超时→日志→`net_close` 回收。

**产出**：proto/session 增量代码。

> 实现记录：**踢旧连接不在工作线程里关**。规划任务 1 写"关旧 fd"、S1 要点 4 也写"net_close 会被工作线程调用（踢旧连接）"，但"工作线程踢旧"与"主线程发现 recv=0"这两条路径会 close 同一个 fd 号，第二次 close 时该 fd 号可能已被新 accept 复用 → **误关一个刚接入的新连接**。改为：`session_login` 只在锁内给旧会话打 `kick` 标记（锁内绝不调 `net_close`，非递归锁会自死锁），主线程每轮经 `session_collect_dead` 摘槽后统一 `net_close`——**close 始终只发生在主线程**。详见 `docs/调试记录-C.md` 问题 26。
>
> 自定义错误码（接口规范 §3.1 未定义的路径，规范字段零改动）：`code=3` `not logged in`（未登录先发 DATA/HEARTBEAT）、`code=4` `invalid client_id`（缺失/超长/含非法字符）。

**验收标准**（对照接口规范 §3.3 三条测试向量）：
| # | 操作 | 预期 |
|---|---|---|
| S3-1 | nc 发 `{"type":"LOGIN","client_id":"PC-01"}` | 回 `{"type":"LOGIN_ACK","status":0}` |
| S3-2 | 发 §3.3 的 DATA 向量 | 服务器正常接收（日志打印4个传感器值），不回 ERROR |
| S3-3 | 发 `{"type":"HEARTBEAT"}` | 回 `{"type":"HEARTBEAT_ACK"}` |
| S3-4 | 终端A登录 `PC-01`；终端B也用 `PC-01` 登录 | A 被断开，B 收到 LOGIN_ACK status=0 |
| S3-5 | 登录后静置不发任何消息 | 约 90s 被服务器断开（nc 显示连接关闭），日志有超时回收记录 |
| S3-6 | 未登录直接发 DATA | 回 ERROR 且不断连；随后 LOGIN 成功 |

**常见坑**：踢旧连接时要从 select 的 fd_set 中移除旧 fd（走 `net_close`，不能直接 close）；超时扫描与 accept/close 并发 → 会话表全程持锁。

---

### ☑ S4 DATA 入库（对应任务书 C3.4，约0.5天）

**目标**：`db.c` 成型，DATA 消息拆包写 `sensor_data`。

**上下文卡**：
- `docs/接口规范.md` §3.1 DATA 行、§3.2 的 ts/sensors 容错条目、§4 sensor_data 表
- 本规划 §2.3（db 接口）
- MySQL C API：https://dev.mysql.com/doc/c-api/en/ （只需 mysql_init/real_connect/query/escape_string 等少数函数）

**任务内容**：
1. `db.c`：`db_init` 建立**每工作线程一个连接**（线程局部 `__thread MYSQL *`，配合 `mysql_thread_init()`，避免连接竞争——比加全局锁简单且是报告亮点）。
2. DATA 处理（proto.c 分发到 `proto_handle_data`）：
   - 遍历 `sensors[]`：`id` 限 1~4，`v` 接受 int 或 float；
   - `ts` 缺省或格式非法 → SQL 用 `NOW()`；合法 → 用该时间；
   - 每个传感器值一行 `INSERT INTO sensor_data(client_id, sensor_type, sensor_value, collect_time)`；
   - 字符串一律 `mysql_real_escape_string` 防注入。
3. DATA 不回业务应答（规范未定义），只在日志记录入库条数。

**产出**：`src/db.c/h` + DATA 处理分支。

**验收标准**：
| # | 操作 | 预期 |
|---|---|---|
| S4-1 | `python simulator/server_probe.py client -H <IP> --id PC-01` 跑 30s | `SELECT COUNT(*) FROM sensor_data;` 持续增长，约每 2s 增 4 行 |
| S4-2 | `SELECT * FROM sensor_data ORDER BY id DESC LIMIT 4;` | 数值与 probe 发送一致（温度1位小数、烟雾/光照整数） |
| S4-3 | 手工发无 `ts` 的 DATA | 该行 collect_time ≈ 服务器当前时间（误差<3s） |
| S4-4 | 发 `ts` 为 `"not-a-time"` 的 DATA | 不报错，fallback 到 NOW() |
| S4-5 | DATA 里 `v` 分别给 `25`（int）和 `25.6`（float） | 两种都正确入库 |

**常见坑**：libmysqlclient 默认单连接不可跨线程并发 query——每线程独立连接即可绕开；程序退出前 `mysql_library_end()`。

> 实现记录：
> 1. **ts 的合法性判定放在 proto.c**（`proto_datetime_ok`），两道校验——按 `"%Y-%m-%d %H:%M:%S"` 用 `strptime` 解析且要求整串被消费完，再用 `mktime` + `strftime` 做一次往返比对。第二道专拦"格式对但日期不存在"的输入（如 2 月 31 日会被归一化成 3 月 3 日），否则这种 ts 会原样拼进 SQL，在 MySQL 严格模式下报错、白丢一行。非法/缺省一律落 `NOW()`。
> 2. **db_init 必须排在 pool_init 之前**：`mysql_library_init()` 不是线程安全的，必须在创建任何线程之前调用（main.c 里已按此顺序）；数据库连不上直接退出，不带着"收得到数据、存不下来"的状态运行。
> 3. **db 模块额外加了一个 `db_shutdown()`**（§2.3 未列，属新增而非改动）：退出前关掉主线程那条连接并调 `mysql_library_end()`。工作线程的连接随进程退出由内核回收（线程池没有 shutdown/join 机制，见调试记录 §九 的同一取舍）。
> 4. **`db_exec` 带一次重连重试**：连接被服务端断开（`wait_timeout` 到期、MySQL 重启过）时重连后重试一次，否则服务器会从那一秒起每条数据都写不进去，直到人工重启——S7 长跑压测最容易撞上这个。
> 5. 新增错误码/字段：**无**。接口规范 §3.1/§3.2/§4 零改动（DATA 仍然不回包）。
>
> 验收 S4-1~S4-5 全过，命令与实测输出见 `docs/调试记录-C.md` §十五。

---

### ☑ S5 告警：阈值判定 + ALARM 推送与入库（对应任务书 C4.1~C4.2，约0.5天）

**目标**：`alarm.c`——每条 DATA 的每个值过一遍阈值检查，超标即推送 ALARM 并写 `alarm_log`。

**上下文卡**：
- `docs/接口规范.md` §3.1 ALARM 行、§3.2 level 语义、§4 threshold_cfg/alarm_log 表
- 本规划 §2.3（alarm 接口）；阈值初始值：温度18~26(HIGH)、湿度40~60(MID)、烟雾>400(HIGH)、光照不告警

**任务内容**：
1. 启动时 `db_load_thresholds()` 把 `threshold_cfg` 读进内存数组（min/max/level，min 或 max 可为 NULL=不检查该侧）。
2. `alarm_check(fd, client_id, sensor_type, value)`：无该传感器阈值配置（光照）→直接返回；越界 → 组 ALARM 消息（格式严格按 §3.1：level/sensor/value/threshold/msg/ts，ts 用服务器当前时间）→ `net_send(fd, ...)` → `db_insert_alarm(...)`。
3. 在 `proto_handle_data` 入库循环里顺带调用 `alarm_check`（同一条 DATA 先入库再告警）。
4. msg 文案：超上限 `"温度超上限"` 这类（按传感器名+方向），超下限 `"温度低于下限"`。
5. 不去重——每条超标 DATA 都告警（任务书明确要求保持简单）。

**产出**：`src/alarm.c/h` + DATA 分支接入。

**验收标准**：
| # | 操作 | 预期 |
|---|---|---|
| S5-1 | probe 或 nc 发 DATA 含 `{"id":1,"v":35.2}` | 立即收到 ALARM：level=HIGH、sensor=1、value=35.2、threshold=26、msg 含"温度"；`alarm_log` 新增一行且字段一致 |
| S5-2 | 发 `{"id":1,"v":10.0}`（低于下限） | ALARM 推送，msg 为低于下限方向 |
| S5-3 | 发 `{"id":3,"v":500}`（烟雾超400） | level=HIGH 告警；发 `{"id":4,"v":10000}`（光照）→ **无**告警 |
| S5-4 | 连续发 3 条同样的超标 DATA | 收到 3 条 ALARM，alarm_log 增 3 行（不去重） |
| S5-5 | 正常值 DATA（全在阈值内） | 无 ALARM、alarm_log 无新行 |

**常见坑**：DECIMAL 从 MySQL 读出来是字符串，用 `strtod` 转；threshold 字段在 ALARM 里是被越的那个边界（超上限→threshold=max）。

> 实现记录：
> 1. **阈值缓存放在 db 模块**：只有 `db.c` 能 `#include <mysql/mysql.h>`（铁律 1），所以 `db_load_thresholds()` 读表后把每行转成普通 C 结构（`db_threshold_t`）存进 db.c 内部的静态数组，`alarm.c` 经**新增**的 `db_threshold_get()` 读取——§2.3 冻结的三个签名一字未改，只新增了一个 getter 和一个结构体。
> 2. **min/max 为 NULL = 不检查该侧**：DECIMAL 列取回来是字符串，为 NULL 时拿到的是 C 的 NULL 指针，据此置 `has_min/has_max = 0`。烟雾 `min_value` 是 NULL，所以"烟雾低于下限"这条判定天然不成立（附加验证里烟雾 0 不告警）；光照不插行 → 没有槽位 → `db_threshold_get` 返回 -1 → 不告警。
> 3. **`db_insert_alarm` 的 alarm_time 用 SQL 的 `NOW()`**：§2.3 冻结的签名没有时间参数。它与 ALARM 推送里的 `ts`（`alarm.c` 用 `strftime` 生成，本步任务 2）同为服务器时钟，实测同一秒。
> 4. **ALARM 由 alarm.c 自己组包**（cJSON），JSON 文本与 `'\n'` 仍在**同一次** `net_send` 内发出。没有复用 `proto_reply`：它是 proto.c 内部的 static，跨模块复用等于改 proto 契约；重复十行同构代码换取模块边界清晰。字段与顺序严格按 §3.1：type/level/sensor/value/threshold/msg/ts。
> 5. **判定用严格不等号**（`value > max` / `value < min`）：值恰好等于边界不算超标。实测烟雾 400（=上限）不告警。
> 6. **DATA 依然不回包**：S5 只新增了规范 §3.1 定义的 ALARM 主动推送，没有给 DATA 加 ACK（调试记录 §十六 要点 4）。
> 7. 新增错误码/字段：**无**。接口规范 §3.1/§3.2/§4 零改动。
>
> 验收 S5-1~S5-5 全过，命令与实测输出见 `docs/调试记录-C.md` §十八。

---

### ☑ S6 分析线程 + ANALYSIS 推送（对应任务书 C4.3，可选 C4.4，约0.5~1天）

**目标**：`analysis.c`——独立线程每 60s 统计最近 10 分钟数据，写 `analysis_result` 并向在线客户端推送 ANALYSIS。

**上下文卡**：
- `docs/接口规范.md` §3.1 ANALYSIS 行、§4 analysis_result 表
- 本规划 §2.3（analysis/db 接口）

**任务内容**：
1. `analysis_thread`：循环 `{ sleep(60); 跑统计; 推送; }`（线程在 main 中创建，随进程退出）。
2. `db_run_analysis()`：一条 SQL 聚合最近 10 分钟——
   ```sql
   SELECT client_id, sensor_type,
          AVG(sensor_value), MAX(sensor_value), MIN(sensor_value), STDDEV_POP(sensor_value)
   FROM sensor_data
   WHERE collect_time >= NOW() - INTERVAL 10 MINUTE
   GROUP BY client_id, sensor_type;
   ```
   over_count 用 `SUM(sensor_value > ? OR sensor_value < ?)` 结合内存阈值计算（或对每组逐值统计）。结果逐组 `INSERT INTO analysis_result`（period_start/end/created_time 填服务器时间）。
3. 对每个**当前在线**的 client_id，组装 ANALYSIS（字段严格按 §3.1：period_start/period_end/sensors[{id,avg,max,min,std,over_count}]）→ 找到对应 fd 推送。**不在线的 client 只入库不推送**。
4. （可选加分 C4.4）`main.c` 加 getopt：`-p 端口 -t 分析周期秒 -w 统计窗口秒`，默认 9000/60/600。
5. 调试技巧：测试时可用 `-t 15 -w 60` 缩短周期快速验证，验收再改回默认。

**产出**：`src/analysis.c/h` + main 接入。

**验收标准**：
| # | 操作 | 预期 |
|---|---|---|
| S6-1 | probe 持续发 DATA ≥ 一个分析周期 | probe 每周期收到一条 ANALYSIS，JSON 字段与 §3.1 逐一对齐 |
| S6-2 | `SELECT * FROM analysis_result ORDER BY id DESC;` | 每周期每个 client×sensor 一行；avg/max/min 与手工 SQL（同窗口同 GROUP BY 查询）比对误差 < 0.05 |
| S6-3 | 人为发 3 条超阈值 DATA 后等一个周期 | 该组 over_count = 3 |
| S6-4 | 无数据的周期 | 不崩溃；无统计行、不推送（或推送空 sensors 数组，二选一并在运行说明中注明） |
| S6-5 | （做了C4.4时）`./server -t 15 -w 60` | 15 秒即产出统计，窗口为 1 分钟 |

**常见坑**：std=0 或单样本时 STDDEV_POP 返回 0/NULL，JSON 里要给 0 而非 null；多 client 在线时逐 client 组包，别把 A 的统计发给 B。

> 实现记录：
> 1. **统计明细经 db 模块的内存缓存交给 analysis.c**：`db_run_analysis()` 的冻结签名只返回行数，带不出每组的明细，于是在 db.c 内部留一份只读副本，用**新增**的 `db_analysis_count()` / `db_analysis_row()` / `db_analysis_period()` 读出来组包——与 S5 的 `db_threshold_get()` 同一套路（新增而非改动冻结签名）。写者与读者都是分析线程，无需加锁。
> 2. **窗口两端点同时是 SQL 字面量与推送字段**：`db_run_analysis()` 取服务器当前时刻生成 `period_start/period_end`，既作为 `WHERE collect_time >= '…' AND collect_time <= '…'` 的字面量，又原样进入 ANALYSIS。推送里写的窗口与实际统计的窗口逐字节一致，S6-2 的手工比对可以原样复现同一期。
> 3. **over_count 用 SQL 的 CASE 条件求和**，条件由内存阈值缓存生成（与 `alarm_check` 同源同口径：严格不等号、NULL 侧不检查、无配置行的传感器不参与），一条聚合查询搞定，不额外拉行、也不再查一次库。
> 4. **"禁止把 A 的统计发给 B"设了两道闸**：组包时只挑 client_id 与本连接**逐字符相等**的组；发送前再用 `session_client_id_copy()` 复核该 fd 现在仍属于这个 client_id（快照到推送之间 fd 可能被回收复用）。另新增 `session_snapshot_logged()`——fd 与 client_id 在同一把锁内一次取齐。
> 5. **推送值按 `DECIMAL(10,2)` 规整**：cJSON 对无法用 15 位有效数字往返的 double 打 `%1.17g`，首轮实测推送出 `"std":3.8810187113933439`，而库里存的是 `3.88`。新增 `analysis_round2()` 后两边逐位相同，客户端也不必显示一长串尾数。
> 6. **退出用 `pthread_cond_timedwait`，不用"分段 sleep + volatile 标志"**：volatile 不做线程间同步，helgrind 实测报 2 处数据竞争（堆栈直指该标志，见问题 35）；改互斥锁 + 条件变量后归零，且 `analysis_stop()` 叫醒立即生效。main 收尾顺序是 `analysis_stop()` → `pthread_join()` → `db_shutdown()`（分析线程有自己的 MySQL 连接，必须走在 `mysql_library_end()` 之前）。
> 7. **db.c 新增一把建连锁 `g_conn_lock`**：helgrind 在"分析线程与工作线程并发首次建连"时报告 libmysqlclient 内部的数据竞争（堆栈全在库内），把建连/关闭串行化即可——这是每线程一生一次的罕见事件，已有连接的快路径完全不碰锁（问题 36）。
> 8. **S6-4 的口径选"无数据不推送"**（不是推空 `sensors` 数组），已写进 `运行说明.md` §5.5。
> 9. getopt（可选加分 C4.4）：`-p 端口 -t 周期 -w 窗口`，默认 **9000 / 60 / 600**；参数**整串校验**（拒绝 `abc`、越界、未知选项），非法即打印用法退出，不会带着坏配置启动。
> 10. 新增错误码/字段：**无**。接口规范 §3.1/§3.2/§4 零改动，ANALYSIS 的字段名与顺序逐字对齐 §3.1。
>
> 验收 S6-1~S6-5 全过（另有"不把 A 的统计发给 B""离线只入库不推送""helgrind 零竞争"三项附加验证），命令与实测输出见 `docs/调试记录-C.md` §二十一。

---

### ☑ S7 压测与异常测试（对应任务书 §5，约0.5天）

**目标**：证明稳定性，暴露并修掉泄漏/崩溃。

**上下文卡**：
- `docs/任务C-服务器端.md` §5
- `README.md` §联调工具（server_probe.py 用法）

**任务内容**：
1. **并发**：3 个 probe 实例（PC-01/PC-02/PC-03）同时跑 10 分钟 → 各自数据入库互不串、ANALYSIS 各发各的、无崩溃。
2. **fd 泄漏**：压测中两次执行 `ls /proc/$(pgrep server)/fd | wc -l`，差值应稳定（±2 内）；`lsof -p <pid> | wc -l` 同理。
3. **异常断开**：probe 跑到一半 `kill -9` → 服务器 90s 内回收会话（日志可见），其余客户端不受影响。
4. **半行+重连**：发半行 JSON 后断开 → 重连重新 LOGIN 正常（缓冲随会话销毁不残留）。
5. （可选）`valgrind --leak-check=full ./server` 跑一轮收发 → 无 definitely lost。
6. 所有问题按"现象→定位→原因→解决"记入 `docs/调试记录-C.md`。

**验收标准**：
| # | 检查 | 预期 |
|---|---|---|
| S7-1 | 3 客户端并发 10 分钟 | 无崩溃；`sensor_data` 三 client 行数大致相等、数值各自正确 |
| S7-2 | fd 计数 | 稳定不增长 |
| S7-3 | kill -9 一个 probe | ≤90s 回收；其他 probe 正常 |
| S7-4 | 半行断开重连 | 重连后 LOGIN_ACK，后续 DATA 正常入库 |

> 实现记录：
> 1. **本步零业务代码改动**（S7 是测试步，不碰 `src/`），`make clean && make` 仍零警告零错误，接口规范零改动。新增的压测脚本 `s7_probe.py` 放在仓库外，不入库。
> 2. **S7-1~S7-4 四条验收标准全过**。三客户端并发 10 分钟：每个 client 1200 行、12 组计数全部相等，跨域查询 0 行（数值互不串），PC-03 的 900 条 ALARM 一条都没漏到 PC-01/PC-02 头上——这靠的是三个 profile **取值域互不重叠**（p1/p2 全在阈值内，收到任何 ALARM 即为串包）。fd 两次计数 `12→12`、`lsof 28→28`、RSS `8504KB→8504KB`，差值全 0；三连接断开后回落到压测前基线 10。
> 3. **kill -9 实测是"1 秒内回收"而非"90 秒"**：Windows 强杀进程时内核关掉 socket 发的是 **RST**，服务器 `recv()` 立刻返回 `Connection reset by peer`。真正的 90s 超时路径另用 `kill -STOP` 冻结虚拟机内客户端（socket 保持打开、进程不再响应）单独验证：最后一条 DATA 19:55:45 → 回收 19:57:15，**正好 90 秒**。
> 4. **memcheck 抓到一处真实缺陷**（问题 40）：分析线程退出时其 `__thread` MySQL 连接从未 `mysql_close`，`definitely lost: 1,192 bytes` + `indirectly lost: 27,786 bytes`。S6 给分析线程加了优雅退出（`pthread_join`），线程真的会退出、TLS 随之销毁，S4 时代"连接随进程退出由内核回收"的策略在这一条路径上失效了。**S7 按约定未修**，修法已写进问题 40：db.h 新增 `db_close_this_thread()`，`analysis_thread` 返回前调用（新增接口、不动冻结签名）。**S8 收尾前必须修掉**，否则 `definitely lost` 不为 0 这条附加标准不达标。
> 5. 定位该泄漏用的是**对照实验**：`./server -t 3600` 让分析线程整轮不建连 → `definitely/indirectly lost` 双双归零，而 `possibly lost: 117,192 bytes in 92 blocks` 与实验组**逐字节相同**——因果链闭合。那 117,192 字节是 4 个常驻工作线程的 TLS + 各自一条 MySQL 连接（每条记录都写着 "in 4 blocks"），按既定口径不是泄漏。
> 6. 两处**判据自身**的教训已记入问题 41/42：排队等候的启动命令会在旧实例退出后"补位"（误以为服务器自己重启了）；"半行残留"类判据不能用回包码（`25.5}]}` 是"合法 JSON 值 + 尾部垃圾"，cJSON 默认不要求整行消费完 → 回 code=2 而非 code=1）。判据要用**服务端日志里的原始行文本**。
>
> 验收 S7-1~S7-4 全过（另有真静默 90s 超时、memcheck 对照实验两项附加验证），命令与实测输出见 `docs/调试记录-C.md` §二十四。

---

### ☐ S8 交付收尾（约0.5天）

**目标**：交付物齐套，新人 5 分钟跑起来。

**任务内容**：
1. 写 `server-linux/运行说明.md`：环境依赖、编译（make）、初始化（mysql < sql/init.sql）、启动（./server [-p -t -w]）、自测方法（probe 命令）、常见问题。
2. 补齐 `docs/调试记录-C.md`（S0~S7 中遇到的每个问题：现象→定位→原因→解决）。
3. Makefile 加 `make clean`、`-Wall` 零警告。
4. 对照任务书 §7 DoD 逐条打勾：
   - [ ] §3.3 三条测试向量应答正确；错误 JSON 回 ERROR 不断连（S2/S3 已验）
   - [ ] 3 客户端并发无崩溃、无 fd 泄漏（S7-1/S7-2）
   - [ ] DATA 入库准确；90s 超时断开；重复登录踢旧（S3/S4）
   - [ ] 告警实时推送且入库；分析线程周期产出正确统计（S5/S6）
   - [ ] `sql/init.sql` + `运行说明.md` 齐全
5. 最终 `git commit`（或打包目录备份）。

**验收标准**：把工程拷到一台干净 Ubuntu（或新建用户目录），**只按运行说明.md 操作**，5 分钟内服务器跑起来且 probe 能通。

---

### ☐ S9（联调阶段，等队友，不占开发时间）

与任务 B 客户端拼合（B+C 先行），再接任务 A 真机；按 `docs/接口规范.md` §6 联调清单逐条验收。此步只需本规划无需再读，清单在接口规范里。

---

## 4 执行 Agent 开工自检模板（每一步开始照抄回答）

```
1. 我要执行的步骤：S__，前置步骤 S__ 的验收是否已全部打勾？
2. 我已阅读：本规划 §2 + 本步骤上下文卡列出的章节（列出实际读了的）
3. 本步只修改的模块文件：____（若发现需要动其他模块的对外接口，停下来先报告）
4. 本步验收标准共 __ 条，我将在完成后逐条给出命令+实际输出
5. 完成后：打勾本步骤、git commit、把问题记入 docs/调试记录-C.md
```

## 5 快速参考（免翻文档）

- 端口：TCP **9000**；心跳 **30s**；超时 **90s**；分析周期 **60s**；统计窗口 **10min**
- 分帧：JSON + `\n`，单条 ≤4KB
- MySQL：库 `machine_room`，用户 `iot`/`iot123456`，表 4 张（sensor_data / analysis_result / alarm_log / threshold_cfg）
- 阈值：温度 18~26(HIGH) / 湿度 40~60(MID) / 烟雾 >400(HIGH) / 光照不告警
- 错误码：1=malformed json，2=unknown type
- 测试工具：`python simulator/server_probe.py client -H <IP> --id PC-01`（假客户端）；`nc <IP> 9000`（手测）
- 传感器：1温度℃ 2湿度% 3烟雾ADC(0~4095) 4光照lux
