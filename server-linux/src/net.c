/*
 * net.c —— select 网络层（S1 实现，S2 接入线程池）
 *
 * 结构：net_init 建监听 socket → net_loop 里 select 循环，四件事：
 *   ① 监听 fd 可读 → accept + session_add 登记；
 *   ② 客户端 fd 可读 → recv + session_feed 入缓冲，再逐条取完整行交给
 *      net_on_message → 深拷贝后 pool_submit 投递线程池（S2 起主线程不再
 *      做任何业务处理，业务在工作线程里跑）；
 *   ③ select 超时（每 1s）→ session_sweep_timeouts 巡检空闲会话；
 *   ④ recv 返回 0 / 出错 → net_close 回收连接。
 *
 * 易错点：select 会就地修改 fd_set，所以每轮必须重新 FD_ZERO/FD_SET。
 * 信号：accept/recv/select 都可能被 Ctrl-C 打断返回 EINTR，按"回到循环
 *       顶部重新判断退出标志"处理，不当作错误。
 *
 * 线程模型（S2）：主线程 = accept/recv/分帧/投递；工作线程 = 解析 + 分发 +
 *       回包。因此本文件里 net_send 必须加发送锁（多条消息不能在同一个 fd 上
 *       交错），net_close / session_* 的线程安全由 session 模块的会话表锁兜住。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "pool.h"
#include "proto.h"
#include "session.h"

static int                     g_listen_fd = -1;   /* 监听 fd，-1 = 未初始化 */
static volatile sig_atomic_t   g_running   = 1;    /* 0 = 请求主循环退出 */

/* 发送锁：一条消息必须在一次 send 序列内写完整。多线程回包时若不加锁，
 * 两条消息的字节流会在同一个 fd 上交错，对端拼出半截 JSON（S2 任务 5）。
 * 锁序约定：会话表锁 → 发送锁，任何路径都不得反向获取。 */
static pthread_mutex_t         g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* 投递给线程池的任务：回包目标 fd + 消息副本。
 * 结构体私有于本文件——proto 模块只需要 proto_handle(fd, line) 这个冻结签名。 */
typedef struct {
    int   fd;
    char *line;
} net_task_t;

static void net_task_run(void *arg);

/* 供信号处理函数调用的唯一入口：只写一个 sig_atomic_t，异步信号安全 */
void net_stop(void)
{
    g_running = 0;
}

static void net_accept(void);
static void net_handle_client(int fd);
static void net_on_message(int fd, const char *line, int len);
static void net_shutdown_all(void);

int net_init(int port)
{
    struct sockaddr_in addr;
    int                fd;
    int                on = 1;

    /* 对端已关闭时 send 会触发 SIGPIPE，默认动作是终止进程 → 一律忽略，
     * 由 net_send 的返回值把错误报给调用方 */
    signal(SIGPIPE, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_err("socket() 失败：%s", strerror(errno));
        return -1;
    }

    /* 允许服务器快速重启后立刻复用 9000 端口（TIME_WAIT 残留） */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        log_warn("SO_REUSEADDR 设置失败（不影响运行）：%s", strerror(errno));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 桥接/NAT 两块网卡都可接入 */
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_err("bind() 端口 %d 失败：%s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, SRV_BACKLOG) < 0) {
        log_err("listen() 失败：%s", strerror(errno));
        close(fd);
        return -1;
    }

    g_listen_fd = fd;
    log_info("监听 socket 就绪：0.0.0.0:%d（fd=%d, backlog=%d）",
             port, fd, SRV_BACKLOG);
    return 0;
}

int net_loop(void)
{
    log_info("进入 select 主循环（select 超时 %ds，单条消息上限 %d 字节）",
             SELECT_TIMEOUT_SEC, MAX_LINE_SIZE);

    while (g_running) {
        fd_set         rfds;
        struct timeval tv;
        int            fds[MAX_SESSIONS];
        int            nfds;
        int            maxfd;
        int            n;
        int            i;

        /* ① 每轮重建读集合：select 返回时会就地修改 fd_set */
        FD_ZERO(&rfds);
        FD_SET(g_listen_fd, &rfds);
        maxfd = g_listen_fd;

        nfds = session_get_fds(fds, MAX_SESSIONS);
        for (i = 0; i < nfds; i++) {
            FD_SET(fds[i], &rfds);
            if (fds[i] > maxfd) {
                maxfd = fds[i];
            }
        }

        tv.tv_sec  = SELECT_TIMEOUT_SEC;
        tv.tv_usec = 0;

        n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) {
                continue;                 /* 被信号打断：回顶部重新判断退出标志 */
            }
            log_err("select() 失败：%s", strerror(errno));
            break;
        }
        if (n == 0) {
            /* ③ 超时（每 1s 一次）：巡检会话活跃时间 */
            session_sweep_timeouts(SESSION_TIMEOUT_SEC);
            continue;
        }

        /* ① 新连接 */
        if (FD_ISSET(g_listen_fd, &rfds)) {
            net_accept();
        }

        /* ② 已有连接可读。fds 是 select 之前的快照，每个 fd 只处理一次，
         *    处理中若关闭了某个 fd，不会重复处理它 */
        for (i = 0; i < nfds; i++) {
            if (FD_ISSET(fds[i], &rfds)) {
                net_handle_client(fds[i]);
            }
        }
    }

    net_shutdown_all();
    log_info("select 主循环已退出");
    return 0;
}

static void net_accept(void)
{
    struct sockaddr_in cli;
    struct timeval     snd_tmo;
    socklen_t          cli_len = sizeof(cli);
    char               ip[INET_ADDRSTRLEN];
    int                fd;
    int                on = 1;

    fd = accept(g_listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (fd < 0) {
        /* 被信号打断、或对端在握手阶段就消失：都不算错误 */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == ECONNABORTED) {
            return;
        }
        log_err("accept() 失败：%s", strerror(errno));
        return;
    }

    /* 小包实时交互（回显/告警/心跳）关掉 Nagle，避免响应被延迟合并 */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
        log_warn("TCP_NODELAY 设置失败（不影响运行）：%s", strerror(errno));
    }

    /* 发送超时兜底：net_send 持全局发送锁，若对端一直不读，阻塞的 send()
     * 会把其他连接的应答一起拖住。超时后 send 返回 EAGAIN，按发送失败处理
     * （该连接本就已异常），把"一个坏客户端拖垮全服"降级为"这一个连接失败" */
    snd_tmo.tv_sec  = SEND_TIMEOUT_SEC;
    snd_tmo.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tmo, sizeof(snd_tmo)) < 0) {
        log_warn("SO_SNDTIMEO 设置失败（不影响运行）：%s", strerror(errno));
    }

    if (session_add(fd) < 0) {
        log_warn("会话表已满（上限 %d），拒绝新连接 fd=%d", MAX_SESSIONS, fd);
        close(fd);                        /* close 只允许出现在本文件 */
        return;
    }

    inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
    log_info("新连接 fd=%d 来自 %s:%u（当前会话 %d 个）",
             fd, ip, (unsigned)ntohs(cli.sin_port), session_count());
}

static void net_handle_client(int fd)
{
    char    buf[RECV_BUF_SIZE];
    ssize_t n;
    int     m;

    n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
        session_touch(fd);
        session_feed(fd, buf, (int)n);

        /* 把缓冲里已切出的完整行逐条取出（S1-4 半行留存、S1-5 粘包切分
         * 都由 session 层保证，这里只管消费） */
        for (;;) {
            char line[MAX_LINE_SIZE + 1];

            m = session_next_line(fd, line, (int)sizeof(line));
            if (m < 0) {
                break;                    /* 无完整行（或会话已失效） */
            }
            if (m > 0) {
                net_on_message(fd, line, m);
            }
            /* m == 0：空行，按分帧规则取走但不回显 */
        }
        return;
    }

    if (n == 0) {
        log_info("客户端 fd=%d 主动断开（对端关闭），回收连接", fd);
        net_close(fd);
        return;
    }

    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return;                           /* 信号打断或本轮已无数据：下轮再看 */
    }
    log_warn("recv() 失败 fd=%d：%s，回收连接", fd, strerror(errno));
    net_close(fd);
}

/*
 * 工作线程入口：解析并分发一条消息，随后释放任务自身持有的内存。
 * 任务的"所有权"在这里终结——主线程 malloc 出来的 line 由工作线程 free，
 * 中间不经过任何信号量/引用计数，因为每个任务只有唯一的消费者。
 */
static void net_task_run(void *arg)
{
    net_task_t *t = (net_task_t *)arg;

    if (t == NULL) {
        return;
    }
    proto_handle(t->fd, t->line);
    free(t->line);
    free(t);
}

/*
 * 收到一条完整消息 → 深拷贝后投递线程池（S2 替换掉 S1 的 echo 占位）。
 *
 * 为什么必须深拷贝：line 指向会话槽位内的接收缓冲，本函数返回后主循环立刻
 * 继续 recv，下一批数据会就地覆盖那块内存；而任务可能还排在队列里没被处理
 * （规划 S2 常见坑）。
 */
static void net_on_message(int fd, const char *line, int len)
{
    net_task_t *t;
    size_t      n = (size_t)len;

    if (n > (size_t)MAX_LINE_SIZE) {
        n = MAX_LINE_SIZE;                /* 防御：session 层已保证不会超 */
    }

    t = malloc(sizeof(*t));
    if (t == NULL) {
        log_err("fd=%d 内存不足，丢弃本条 %u 字节消息", fd, (unsigned)n);
        return;
    }
    t->line = malloc(n + 1);
    if (t->line == NULL) {
        log_err("fd=%d 内存不足，丢弃本条 %u 字节消息", fd, (unsigned)n);
        free(t);
        return;
    }

    memcpy(t->line, line, n);
    t->line[n] = '\0';
    t->fd      = fd;

    /* 日志留在主线程一侧：与工作线程的"解析消息"日志对照即可验证 S2-4 */
    log_info("主线程：fd=%d 收到完整消息 %u 字节，投递线程池", fd, (unsigned)n);
    pool_submit(net_task_run, t);
}

int net_send(int fd, const char *buf)
{
    size_t total;
    size_t sent = 0;
    int    rc   = 0;

    if (fd < 0 || buf == NULL) {
        return -1;
    }
    total = strlen(buf);

    /* 持锁期间不允许有其他线程往任何 fd 写：否则同一连接上两条消息的字节
     * 会交错，对端按 '\n' 切出来的就是坏 JSON。代价是这把锁跨 fd 全局，
     * 某个客户端迟迟不收（接收窗口打满）会让阻塞的 send() 拖住其余连接，
     * 由 accept 时设置的 SO_SNDTIMEO 兜底（超时按发送失败返回）。 */
    pthread_mutex_lock(&g_send_lock);

    while (sent < total) {
        ssize_t n = send(fd, buf + sent, total - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_warn("send() 失败 fd=%d：%s", fd, strerror(errno));
            rc = -1;
            break;
        }
        sent += (size_t)n;
    }

    pthread_mutex_unlock(&g_send_lock);
    return (rc < 0) ? -1 : (int)sent;
}

void net_close(int fd)
{
    if (fd < 0) {
        return;
    }
    /* 先清会话再关 fd：若顺序相反，新 accept 可能复用同一 fd 号，
     * 而旧会话槽位还占着它（S3 多线程下尤其要注意） */
    session_remove(fd);
    close(fd);
    log_info("连接已关闭 fd=%d（剩余会话 %d 个）", fd, session_count());
}

/* 退出前收尾：断开全部会话并关闭监听 fd */
static void net_shutdown_all(void)
{
    int fds[MAX_SESSIONS];
    int n;
    int i;

    n = session_get_fds(fds, MAX_SESSIONS);
    for (i = 0; i < n; i++) {
        net_close(fds[i]);
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
