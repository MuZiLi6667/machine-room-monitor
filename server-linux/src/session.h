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

#include "config.h"     /* CLIENT_ID_MAX_LEN：下面的会话快照结构体要用 */

/* 登记一个新连接（accept 成功后调用）。成功返回 0；会话表满返回 -1。 */
int  session_add(int fd);

/* 注销该连接并清空其会话数据（net_close 时调用，幂等）。 */
void session_remove(int fd);

/* ---- 以下三个属于 S3（LOGIN）实现，S1 仅冻结签名、不填实现 ---- */

/* 绑定 client_id 并置为已登录；同 client_id 的其它已登录会话被标记为"待踢除"。
 * 成功返回 0；client_id 非法（非 1~31 个 [A-Za-z0-9-_]）或 fd 不在会话表中返回 -1。
 *
 * 踢旧不在本函数内关闭连接：本函数由工作线程（proto）调用，持会话表锁期间
 * 调 net_close 会因 session_remove 再取同一把非递归锁而自死锁；且"关闭 fd"
 * 必须与 accept/recv 串行（同号 fd 复用会误关新连接）。故这里只打标记，
 * 由主线程经 session_collect_dead() 统一回收（见下）。 */
int         session_login(int fd, const char *client_id);

/* 是否已完成 LOGIN。 */
int         session_is_logged(int fd);

/* 已登录会话的 client_id；未登录或 fd 无效返回 NULL。
 * 返回的是**会话槽内部指针**，出锁即可能因连接被踢/回收而失效，
 * 调用方必须立刻拷贝使用——需要稳定副本请用 session_client_id_copy()。 */
const char *session_client_id(int fd);

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

/* 巡检会话活跃时间：超过 timeout_sec 未收到任何消息的会话打印告警日志
 * （同一段空闲只报一次）。真正的断开由 session_collect_dead() 完成。 */
void session_sweep_timeouts(int timeout_sec);

/* ---- S1/S3 新增（未改动 §2.3 冻结签名） ---- */
/* 取出全部活跃 fd 写入 fds（最多 cap 个），返回实际个数。 */
int  session_get_fds(int *fds, int cap);

/* S3：摘除"待踢除"（session_login 标记的重复 client_id 旧连接）与"已超过
 * timeout_sec 无消息"的会话槽，把它们的 fd 写入 fds（最多 cap 个）并返回个数。
 * 槽位在返回前即清空，故这些 fd 不会再进入 select 集合、不会再被 recv。
 *
 * 只由 select 主循环（net.c）调用：槽位摘除后紧接着 net_close(fd)，
 * 使"关闭连接"这一个动作始终只发生在主线程——多线程各自 close 会在 fd
 * 号被新连接复用后误关新连接。 */
int  session_collect_dead(int timeout_sec, int *fds, int cap);

/* S3：把已登录会话的 client_id 拷进调用方缓冲（锁内 memcpy，规避
 * session_client_id() 裸指针出锁失效的问题）。成功返回 0，失败返回 -1。 */
int  session_client_id_copy(int fd, char *out, int cap);
/* 当前活跃会话数量（日志用）。 */
int  session_count(void);

/* ---- S6 新增 ---- */
/* 一条"已登录、且未被标记踢除"的会话快照（fd 与 client_id 配套）。 */
typedef struct {
    int  fd;
    char client_id[CLIENT_ID_MAX_LEN + 1];
} session_snapshot_t;

/* 取当前已登录会话的快照（最多 cap 个），返回实际个数。
 * 空槽、未登录、已被 session_login 标记待踢的槽位都不会出现——分析线程据此
 * 决定"把统计推给谁"。
 *
 * fd 与 client_id 在**同一把锁内一次取齐**：分两次（先取 fd 列表、再逐个查
 * client_id）会在两次调用之间被踢旧/超时回收插空，取回来的 fd 可能已经属于
 * 另一个 client。即便如此，返回后的 fd 仍可能在推送前被主线程回收并被新连接
 * 复用，故调用方**推送前**还要用 session_client_id_copy() 复核一次
 * （见 analysis.c：这是"禁止把 A 的统计发给 B"的最后一道闸）。 */
int  session_snapshot_logged(session_snapshot_t *out, int cap);

#endif /* SESSION_H */
