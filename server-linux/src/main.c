/*
 * main.c —— 程序入口（S3 版本：会话协议 LOGIN/HEARTBEAT/超时）
 *
 * 本文件是"组装者"：唯一知道所有模块的文件（规划 §2.2）。
 * 启动顺序：日志 → 线程池 → 网络监听 → select 主循环（net_loop）。
 * 线程池必须先于 net_init：主循环一旦开始 accept，消息随时可能到达并投递。
 * S6 再补 getopt 命令行参数（-p/-t/-w）。
 */
#include <signal.h>
#include <stddef.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "pool.h"

/* 退出标志由 net 模块持有，信号处理函数只负责通知它。
 * 只做一次赋值，满足异步信号安全要求（不能在这里打日志/加锁）。 */
static void on_signal(int signo)
{
    (void)signo;
    net_stop();
}

int main(void)
{
    struct sigaction sa;

    /* 1. 初始化日志 */
    log_init(LOG_TAG);

    /* 2. 启动信息 */
    log_info("==================================================");
    log_info("机房环境多指标远程监测与分析系统 —— 服务器端");
    log_info("版本 S3：会话协议（LOGIN / HEARTBEAT / 90s 超时断开 / 踢旧连接）");
    log_info("监听端口   : %d", SRV_PORT);
    log_info("工作线程   : %d 个（任务队列 %d）", POOL_THREADS, POOL_QUEUE_SIZE);
    log_info("心跳/超时  : %ds / %ds", HEARTBEAT_INTERVAL_SEC, SESSION_TIMEOUT_SEC);
    log_info("分析周期/窗口: %ds / %ds", ANALYSIS_PERIOD_SEC, ANALYSIS_WINDOW_SEC);
    log_info("数据库     : %s@%s/%s", DB_USER, DB_HOST, DB_NAME);
    log_info("==================================================");

    /* 3. 注册退出信号。不设 SA_RESTART：让阻塞中的 select 返回 EINTR，
     *    主循环据此回到顶部检查退出标志（见 net.c） */
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 4. 线程池：必须先于网络就绪，保证第一条消息到达时已有消费者 */
    pool_init(POOL_THREADS);

    /* 5. 网络初始化（创建监听 socket） */
    if (net_init(SRV_PORT) < 0) {
        log_err("网络初始化失败，服务器退出");
        return 1;
    }

    log_info("服务器已就绪，按 Ctrl-C 退出");

    /* 6. select 主循环：阻塞运行，直到收到退出信号 */
    net_loop();

    /* 7. 收尾 */
    log_info("收到退出信号，服务器正常退出");
    return 0;
}
