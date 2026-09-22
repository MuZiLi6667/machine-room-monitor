/*
 * net.c —— select 网络层（S1 实现）
 *
 * 结构：net_init 建监听 socket → net_loop 里 select 循环，四件事：
 *   ① 监听 fd 可读 → accept + session_add 登记；
 *   ② 客户端 fd 可读 → recv + session_feed 入缓冲，再逐条取完整行交给
 *      net_on_message（S1 占位为 echo，S2 替换成投递线程池）；
 *   ③ select 超时（每 1s）→ session_sweep_timeouts 巡检空闲会话；
 *   ④ recv 返回 0 / 出错 → net_close 回收连接。
 *
 * 易错点：select 会就地修改 fd_set，所以每轮必须重新 FD_ZERO/FD_SET。
 * 信号：accept/recv/select 都可能被 Ctrl-C 打断返回 EINTR，按"回到循环
 *       顶部重新判断退出标志"处理，不当作错误。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "session.h"

static int                     g_listen_fd = -1;   /* 监听 fd，-1 = 未初始化 */
static volatile sig_atomic_t   g_running   = 1;    /* 0 = 请求主循环退出 */

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
 * S1 占位业务：收到一条完整消息就原样回显。
 * S2 起本函数改为"深拷贝消息 → pool_submit 投递线程池"，其余流程不动。
 */
static void net_on_message(int fd, const char *line, int len)
{
    char   out[MAX_LINE_SIZE + 2];
    size_t n = (size_t)len;

    if (n > (size_t)MAX_LINE_SIZE) {
        n = MAX_LINE_SIZE;                /* 防御：session 层已保证不会超 */
    }

    log_info("fd=%d 收到完整消息 %u 字节：%.120s", fd, (unsigned)n, line);

    memcpy(out, line, n);
    out[n]     = MSG_DELIMITER;           /* 回发时补回分帧符 */
    out[n + 1] = '\0';

    if (net_send(fd, out) < 0) {
        log_warn("fd=%d 回显失败，连接可能已断开", fd);
    }
}

int net_send(int fd, const char *buf)
{
    size_t total;
    size_t sent = 0;

    if (fd < 0 || buf == NULL) {
        return -1;
    }
    total = strlen(buf);

    while (sent < total) {
        ssize_t n = send(fd, buf + sent, total - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_warn("send() 失败 fd=%d：%s", fd, strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
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
