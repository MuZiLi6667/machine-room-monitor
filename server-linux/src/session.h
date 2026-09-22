/*
 * session.h —— 会话表对外契约（规划 §2.3，S1 实现分帧部分）
 *
 * 会话表由本模块独占持有（固定数组，MAX_SESSIONS 个槽位），外界只能通过
 * session_*() 函数访问，不得直接触碰内部结构（规划 §2.2 铁律 3）。
 *
 * 接收缓冲与分帧规则（接口规范 §3）：
 *   每条 JSON 消息以 '\n' 结尾，单条消息 ≤ 4KB；
 *   接收方按 '\n' 切分缓冲，不完整行留存等待下一批数据；
 *   某行累积到 4KB 仍未出现 '\n' → 整条丢弃，其后字节丢弃至下一个 '\n'。
 *
 * 线程安全（S2 起）：本模块全部公开函数内部持会话表锁，**任意线程可直接调用**，
 * 调用方无需额外同步；但不得在持有会话表锁的状态下再回调本模块（非递归锁）。
 */
#ifndef SESSION_H
#define SESSION_H

/* 登记一个新连接（accept 成功后调用）。成功返回 0；会话表满返回 -1。 */
int  session_add(int fd);

/* 注销该连接并清空其会话数据（net_close 时调用，幂等）。 */
void session_remove(int fd);

/* ---- 以下三个属于 S3（LOGIN）实现，S1 仅冻结签名、不填实现 ---- */
int         session_login(int fd, const char *client_id); /* 绑定 client_id，含踢旧连接逻辑 */
int         session_is_logged(int fd);                    /* 是否已完成 LOGIN */
const char *session_client_id(int fd);                    /* 未登录返回 NULL */

/* 刷新最近活跃时间（收到任何消息后调用）。 */
void session_touch(int fd);

/* 把 recv 到的数据追加进接收缓冲。
 * 返回本次追加后缓冲中新形成的完整行条数；-1 表示 fd 无效。 */
int  session_feed(int fd, const char *data, int len);

/* 从缓冲头部摘出一条完整行：写入 out（不含结尾 '\n'）并以 '\0' 结束。
 * 返回该消息的字节数（可能为 0，表示取到一个空行）；
 * 返回 -1 表示当前没有完整行可取，或 fd 无效。
 * 注意：0 与 -1 语义不同，调用方须用 `>= 0` 判断"取到了"。 */
int  session_next_line(int fd, char *out, int cap);

/* 巡检会话活跃时间：超过 timeout_sec 未收到任何消息的会话打印告警日志。
 * S1：只打印不断开；S3 起在此处执行 net_close 真正回收。 */
void session_sweep_timeouts(int timeout_sec);

/* ---- S1 新增（未改动 §2.3 冻结签名），供 net.c 构造 select 集合 ---- */
/* 取出全部活跃 fd 写入 fds（最多 cap 个），返回实际个数。 */
int  session_get_fds(int *fds, int cap);
/* 当前活跃会话数量（日志用）。 */
int  session_count(void);

#endif /* SESSION_H */
