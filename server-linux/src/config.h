/*
 * config.h —— 全局常量（本项目唯一改配置的地方）
 *
 * 取值依据 `docs/接口规范.md` §1 全局约定 与 任务C执行规划 §5 快速参考：
 *   端口 TCP 9000；心跳 30s；超时 90s；分析周期 60s；统计窗口 10min；
 *   分帧 JSON + '\n' 且单条 ≤4KB；库 machine_room / 用户 iot。
 * 禁止各模块内散落魔法数字，一律引用本头文件宏。
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ---------- 网络（接口规范 §1 / §3） ---------- */
#define SRV_PORT              9000      /* 监听端口 TCP 9000 */
#define SRV_BACKLOG           64        /* listen 队列长度 */
#define SELECT_TIMEOUT_SEC    1         /* select 主循环超时（秒） */
#define MAX_SESSIONS          64        /* 会话表槽位数（固定数组） */

/* ---------- 分帧规则（接口规范 §3 分帧规则） ---------- */
#define RECV_BUF_SIZE         8192      /* 单连接接收缓冲字节数 */
#define MAX_LINE_SIZE         4096      /* 单条消息上限 4KB，超长无换行则丢弃 */
#define MSG_DELIMITER         '\n'      /* 每条 JSON 以 \n 结尾 */

/* ---------- 心跳与超时（接口规范 §1 / §3.2） ---------- */
#define HEARTBEAT_INTERVAL_SEC 30       /* 客户端心跳周期 30s */
#define SESSION_TIMEOUT_SEC    90       /* 90s 无任何消息判定离线并断开 */

/* ---------- 统计分析（接口规范 §3.1 ANALYSIS） ---------- */
#define ANALYSIS_PERIOD_SEC    60       /* 分析线程周期 60s */
#define ANALYSIS_WINDOW_SEC    600      /* 统计窗口 10min */

/* ---------- 线程池 ---------- */
#define POOL_THREADS           4        /* 工作线程数 */
#define POOL_QUEUE_SIZE        256      /* 任务队列容量（有界） */

/* ---------- 业务字段约束（接口规范 §3.2） ---------- */
#define CLIENT_ID_MIN_LEN      1        /* client_id 长度 1~31 */
#define CLIENT_ID_MAX_LEN      31
#define SENSOR_ID_MIN          1        /* 传感器编号 1~4 */
#define SENSOR_ID_MAX          4

/* ---------- 数据库（接口规范 §4） ---------- */
#define DB_HOST                "localhost"
#define DB_USER                "iot"
#define DB_PASS                "iot123456"
#define DB_NAME                "machine_room"

/* ---------- 日志 ---------- */
#define LOG_TAG                "server" /* 日志行前缀标识 */

#endif /* CONFIG_H */
