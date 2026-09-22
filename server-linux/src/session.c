/*
 * session.c —— 会话表：连接生命周期、接收缓冲分帧（S1 实现，S2 补锁，S3 补登录）
 *
 * 数据结构：定长数组，MAX_SESSIONS 个槽位，fd == -1 表示空槽。
 *   fd ↔ client_id、登录态、最近活跃时间、接收缓冲都挂在这个结构上。
 *
 * 分帧流程：session_feed 累积字节 → session_next_line 按 '\n' 摘行。
 * 超长保护：某一行累积到 MAX_LINE_SIZE 仍无 '\n' → 丢弃已收的这一行并
 *           进入 discarding 态，其后字节一律丢弃，直到下一个 '\n' 才恢复
 *           正常分帧（对应验收 S1-6：超长行不得影响后续正常行）。
 *
 * 登录与踢旧（S3）：
 *   session_login 把已有同 client_id 的旧会话打上 kick 标记（**不在锁内关连接**），
 *   主线程每轮调 session_collect_dead 摘除这些槽位并交给 net_close 关闭。
 *   "关闭 fd"始终只发生在主线程，避免两个线程各自 close 同一 fd 号后被
 *   accept 复用时误关新连接（S1 要点 4、S3 常见坑）。
 *
 * 线程安全（规划 S2 任务 5）：工作线程也会碰会话表（LOGIN 判定、取 client_id），
 *   故**全部公开函数一律加锁**；文件内的 static 辅助函数假定"调用时已持锁"，
 *   不得自行加锁（同一把非递归锁会自死锁）。
 *   锁序：会话表锁 → 发送锁（net_send），任何路径都不得反向。
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "log.h"
#include "session.h"

typedef struct {
    int    fd;                                /* -1 = 空槽 */
    char   client_id[CLIENT_ID_MAX_LEN + 1];  /* LOGIN 后写入（S3） */
    int    logged;                            /* 是否已 LOGIN（S3） */
    int    kick;                              /* 1 = 待踢除（重复 client_id 旧连接，S3） */
    time_t last_active;                       /* 最近活跃时间戳 */
    size_t len;                               /* 接收缓冲已用字节数 */
    size_t line_start;                        /* 当前未完成行在 buf 中的起点 */
    int    discarding;                        /* 1 = 超长行丢弃态 */
    int    timeout_logged;                    /* 超时日志去重（每段空闲只报一次） */
    char   buf[RECV_BUF_SIZE];                /* 单连接接收缓冲 */
} session_t;

static session_t       g_sessions[MAX_SESSIONS];
static int             g_inited = 0;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

/* fd 初值是 0（= stdin），必须先全部置 -1 才可用，故做一次性惰性初始化。
 * 调用方必须已持有 g_session_lock。 */
static void session_ensure_init(void)
{
    int i;

    if (g_inited) {
        return;
    }
    for (i = 0; i < MAX_SESSIONS; i++) {
        g_sessions[i].fd = -1;
    }
    g_inited = 1;
}

/* 按 fd 查会话槽；找不到返回 NULL。调用方必须已持有 g_session_lock。 */
static session_t *session_find(int fd)
{
    int i;

    if (fd < 0) {
        return NULL;
    }
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].fd == fd) {
            return &g_sessions[i];
        }
    }
    return NULL;
}

/* 当前活跃会话数。调用方必须已持有 g_session_lock（故不能直接用 session_count）。 */
static int session_count_locked(void)
{
    int i;
    int n = 0;

    for (i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].fd >= 0) {
            n++;
        }
    }
    return n;
}

/* 清空一个槽位（恢复为空槽）。调用方必须已持有 g_session_lock。 */
static void session_slot_clear(session_t *s)
{
    s->fd             = -1;
    s->client_id[0]   = '\0';
    s->logged         = 0;
    s->kick           = 0;
    s->len            = 0;
    s->line_start     = 0;
    s->discarding     = 0;
    s->timeout_logged = 0;
}

/* client_id 合法性：1~31 个字符，限 [A-Za-z0-9-_]（接口规范 §3.2）。 */
static int client_id_valid(const char *s)
{
    size_t i;
    size_t n;

    if (s == NULL) {
        return 0;
    }
    n = strlen(s);
    if (n < CLIENT_ID_MIN_LEN || n > CLIENT_ID_MAX_LEN) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!(isalnum(c) || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int session_add(int fd)
{
    session_t *s = NULL;
    int        i;
    int        rc = -1;

    if (fd < 0) {
        return -1;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    if (session_find(fd) != NULL) {
        rc = 0;                       /* 已在表中：幂等 */
    } else {
        for (i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].fd == -1) {
                s = &g_sessions[i];
                break;
            }
        }
        if (s != NULL) {
            /* 复用槽位时字段必须全部重置，避免残留上一个连接的数据 */
            session_slot_clear(s);
            s->fd          = fd;
            s->last_active = time(NULL);
            rc             = 0;
        }
        /* s == NULL：会话表满，rc 保持 -1 */
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

void session_remove(int fd)
{
    session_t *s;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL) {
        session_slot_clear(s);
    }

    pthread_mutex_unlock(&g_session_lock);
}

/*
 * 登录（S3）：校验 client_id → 标记同 id 旧会话待踢 → 写本会话登录态。
 *
 * 踢旧只打标记不在这里关连接：本函数可能跑在工作线程里，持会话表锁时调
 * net_close 会经 session_remove 再取同一把非递归锁而自死锁；关闭动作统一
 * 由主线程的 session_collect_dead + net_close 完成。
 * 被标记的旧连接在真正关闭前（最多一轮主循环）仍可收发，属可接受的延迟。
 */
int session_login(int fd, const char *client_id)
{
    session_t *s;
    int        i;
    int        rc = -1;

    if (!client_id_valid(client_id)) {
        log_warn("fd=%d LOGIN 的 client_id 非法（须 %d~%d 个 [A-Za-z0-9-_]）：\"%s\"",
                 fd, CLIENT_ID_MIN_LEN, CLIENT_ID_MAX_LEN,
                 client_id != NULL ? client_id : "(缺失)");
        return -1;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s == NULL) {
        log_warn("fd=%d LOGIN 失败：该连接不在会话表中", fd);
    } else {
        for (i = 0; i < MAX_SESSIONS; i++) {
            session_t *old = &g_sessions[i];

            /* 跳过空槽、自己、未登录的、以及已经标记过的 */
            if (old->fd < 0 || old->fd == fd || !old->logged || old->kick) {
                continue;
            }
            if (strcmp(old->client_id, client_id) == 0) {
                old->kick = 1;
                log_warn("client_id=%s 重复登录：旧连接 fd=%d 标记待踢除（新连接 fd=%d）",
                         client_id, old->fd, fd);
            }
        }

        /* 重复 LOGIN 视为刷新：直接覆盖自己的登录态并清掉自身的待踢标记 */
        snprintf(s->client_id, sizeof(s->client_id), "%s", client_id);
        s->logged         = 1;
        s->kick           = 0;
        s->last_active    = time(NULL);
        s->timeout_logged = 0;
        log_info("fd=%d 登录成功：client_id=%s（当前会话 %d 个）",
                 fd, s->client_id, session_count_locked());
        rc = 0;
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

int session_is_logged(int fd)
{
    session_t *s;
    int        rc = 0;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL && s->logged) {
        rc = 1;
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

const char *session_client_id(int fd)
{
    session_t *s;
    const char *rc = NULL;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL && s->logged) {
        rc = s->client_id;          /* 内部指针：出锁即可能失效，调用方须立刻拷贝 */
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

int session_client_id_copy(int fd, char *out, int cap)
{
    session_t *s;
    int        rc = -1;

    if (out == NULL || cap <= 0) {
        return -1;
    }
    out[0] = '\0';

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL && s->logged) {
        snprintf(out, (size_t)cap, "%s", s->client_id);
        rc = 0;
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

void session_touch(int fd)
{
    session_t *s;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL) {
        s->last_active    = time(NULL);
        s->timeout_logged = 0;        /* 重新活跃 → 允许下一段空闲再报一次超时 */
    }

    pthread_mutex_unlock(&g_session_lock);
}

int session_feed(int fd, const char *data, int len)
{
    session_t *s;
    int        i;
    int        lines = 0;
    int        rc    = -1;

    if (data == NULL || len <= 0) {
        return -1;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s == NULL) {
        pthread_mutex_unlock(&g_session_lock);
        return -1;
    }

    for (i = 0; i < len; i++) {
        char c = data[i];

        /* 丢弃态：吞掉一切字节，遇到 '\n' 表示超长行结束，恢复分帧 */
        if (s->discarding) {
            if (c == MSG_DELIMITER) {
                s->discarding = 0;
            }
            continue;
        }

        /* 兜底：缓冲物理写满（正常不会发生——调用方每轮取空，且下面的
         * 超长判定会先一步触发）。丢弃未完成行，保留其中已切出的完整行。 */
        if (s->len + 1 > sizeof(s->buf)) {
            log_warn("fd=%d 接收缓冲已满（%u 字节），丢弃未完成行", fd,
                     (unsigned)sizeof(s->buf));
            s->len        = s->line_start;
            s->discarding = 1;
            continue;
        }

        s->buf[s->len++] = c;

        if (c == MSG_DELIMITER) {
            lines++;                      /* 又切出一条完整行 */
            s->line_start = s->len;
        } else if (s->len - s->line_start >= MAX_LINE_SIZE) {
            /* 当前行已达 4KB 仍无换行：整条丢弃（不留在缓冲里占位） */
            log_warn("fd=%d 单行超过 %d 字节仍无换行，丢弃该行（后续字节丢弃至下一个换行）",
                     fd, MAX_LINE_SIZE);
            s->len        = s->line_start;
            s->discarding = 1;
        }
    }
    rc = lines;

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

int session_next_line(int fd, char *out, int cap)
{
    session_t *s;
    char      *nl;
    size_t     mlen;
    size_t     consumed;
    int        n;
    int        rc = -1;

    if (out == NULL || cap <= 0) {
        return -1;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    s = session_find(fd);
    if (s != NULL) {
        nl = memchr(s->buf, MSG_DELIMITER, s->len);
        if (nl != NULL) {
            mlen = (size_t)(nl - s->buf);     /* 消息长度，不含结尾 '\n' */
            n    = ((int)mlen < cap - 1) ? (int)mlen : cap - 1;
            if ((size_t)n < mlen) {
                log_warn("fd=%d 单条消息 %u 字节超出输出缓冲 %d，已截断", fd,
                         (unsigned)mlen, cap);
            }
            memcpy(out, s->buf, (size_t)n);
            out[n] = '\0';

            /* 摘掉已消费的这条（消息 + 结尾 '\n'），剩余字节前移 */
            consumed = mlen + 1;
            memmove(s->buf, s->buf + consumed, s->len - consumed);
            s->len       -= consumed;
            s->line_start = (s->line_start > consumed) ? (s->line_start - consumed) : 0;

            rc = (int)mlen;               /* >= 0 才算取到了（0 = 空行） */
        }
        /* nl == NULL：尚无完整行，留存等待下一批数据（S1-4），rc 保持 -1 */
    }

    pthread_mutex_unlock(&g_session_lock);
    return rc;
}

void session_sweep_timeouts(int timeout_sec)
{
    time_t now = time(NULL);
    int    i;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    for (i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &g_sessions[i];
        long       idle;

        if (s->fd < 0) {
            continue;
        }
        idle = (long)(now - s->last_active);
        if (idle < (long)timeout_sec || s->timeout_logged || s->kick) {
            continue;                 /* 未超时、该段空闲已报过警、或已被标记待踢 */
        }

        log_warn("会话超时：fd=%d 已 %lds 无任何消息（阈值 %ds）——本轮巡检回收该连接",
                 s->fd, idle, timeout_sec);
        s->timeout_logged = 1;
    }

    pthread_mutex_unlock(&g_session_lock);
}

int session_collect_dead(int timeout_sec, int *fds, int cap)
{
    time_t now = time(NULL);
    int    i;
    int    n = 0;

    if (fds == NULL || cap <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    for (i = 0; i < MAX_SESSIONS && n < cap; i++) {
        session_t *s = &g_sessions[i];
        long       idle;

        if (s->fd < 0) {
            continue;
        }
        idle = (long)(now - s->last_active);
        if (!s->kick && idle < (long)timeout_sec) {
            continue;                 /* 既未被踢，也没超时 */
        }

        if (s->kick) {
            log_info("回收被顶替的旧连接：fd=%d client_id=%s（同一 client_id 已在新连接登录）",
                     s->fd, s->client_id);
        } else {
            log_warn("会话超时断开：fd=%d 已 %lds 无任何消息（阈值 %ds），回收连接",
                     s->fd, idle, timeout_sec);
        }

        fds[n++] = s->fd;
        /* 先摘除槽位：该 fd 立刻退出 select 集合，不会再被 recv；
         * 真正的 close(fd) 由主线程调用 net_close 完成（见 session.h 说明） */
        session_slot_clear(s);
    }

    pthread_mutex_unlock(&g_session_lock);
    return n;
}

int session_get_fds(int *fds, int cap)
{
    int i;
    int n = 0;

    if (fds == NULL || cap <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    for (i = 0; i < MAX_SESSIONS && n < cap; i++) {
        if (g_sessions[i].fd >= 0) {
            fds[n++] = g_sessions[i].fd;
        }
    }

    pthread_mutex_unlock(&g_session_lock);
    return n;
}

int session_count(void)
{
    int n;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();
    n = session_count_locked();
    pthread_mutex_unlock(&g_session_lock);
    return n;
}
