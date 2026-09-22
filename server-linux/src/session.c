/*
 * session.c —— 会话表与接收缓冲分帧（S1 实现，S2 补会话表锁）
 *
 * 数据结构：定长数组，MAX_SESSIONS 个槽位，fd == -1 表示空槽。
 *   fd ↔ client_id、最近活跃时间、接收缓冲都挂在这个结构上。
 *
 * 分帧流程：session_feed 累积字节 → session_next_line 按 '\n' 摘行。
 * 超长保护：某一行累积到 MAX_LINE_SIZE 仍无 '\n' → 丢弃已收的这一行并
 *           进入 discarding 态，其后字节一律丢弃，直到下一个 '\n' 才恢复
 *           正常分帧（对应验收 S1-6：超长行不得影响后续正常行）。
 *
 * 线程安全（规划 S2 任务 5）：S2 起线程池的工作线程也会碰会话表（回包、S3 的
 *   踢旧连接），故**全部公开函数一律加锁**；文件内的 static 辅助函数假定
 *   "调用时已持锁"，不得自行加锁（同一把非递归锁会自死锁）。
 *   锁序：会话表锁 → 发送锁（net_send），任何路径都不得反向。
 */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "log.h"
#include "session.h"

typedef struct {
    int    fd;                                /* -1 = 空槽 */
    char   client_id[CLIENT_ID_MAX_LEN + 1];  /* LOGIN 后写入（S3） */
    int    logged;                            /* 是否已 LOGIN（S3） */
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
            /* 复用槽位时字段必须逐个重置，避免残留上一个连接的数据 */
            s->fd             = fd;
            s->client_id[0]   = '\0';
            s->logged         = 0;
            s->last_active    = time(NULL);
            s->len            = 0;
            s->line_start     = 0;
            s->discarding     = 0;
            s->timeout_logged = 0;
            rc                = 0;
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
        s->fd             = -1;
        s->client_id[0]   = '\0';
        s->logged         = 0;
        s->len            = 0;
        s->line_start     = 0;
        s->discarding     = 0;
        s->timeout_logged = 0;
    }

    pthread_mutex_unlock(&g_session_lock);
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
        if (idle < (long)timeout_sec || s->timeout_logged) {
            continue;                 /* 未超时，或该段空闲已报过警 */
        }

        /* S1 只告警不断开（便于观察）；S3 起在此处执行 net_close 回收 */
        log_warn("会话超时：fd=%d 已 %lds 无任何消息（阈值 %ds）——S1 仅告警，S3 起断开回收",
                 s->fd, idle, timeout_sec);
        s->timeout_logged = 1;
    }

    pthread_mutex_unlock(&g_session_lock);
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
    int i;
    int n = 0;

    pthread_mutex_lock(&g_session_lock);
    session_ensure_init();

    for (i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].fd >= 0) {
            n++;
        }
    }

    pthread_mutex_unlock(&g_session_lock);
    return n;
}
