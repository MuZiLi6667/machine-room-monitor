/*
 * main.c —— 程序入口（S0 版本：只打印启动日志并可正常退出）
 *
 * 本文件是"组装者"：唯一知道所有模块的文件（规划 §2.2）。
 * S0 阶段仅验证工程骨架可编译、可运行、可被 Ctrl-C 正常终止；
 * S1 起此处将替换为 net_init() + net_loop()，S6 再补 getopt 命令行参数。
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

/* 退出标志：信号处理函数里只能写 sig_atomic_t */
static volatile sig_atomic_t g_running = 1;

static void on_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

int main(void)
{
    sigset_t mask;
    sigset_t oldmask;
    struct sigaction sa;

    /* 1. 初始化日志 */
    log_init(LOG_TAG);

    /* 2. 打印启动日志（S0 的全部业务） */
    log_info("==================================================");
    log_info("机房环境多指标远程监测与分析系统 —— 服务器端");
    log_info("版本 S0：工程骨架，业务逻辑自 S1 起逐步接入");
    log_info("监听端口   : %d", SRV_PORT);
    log_info("心跳/超时  : %ds / %ds", HEARTBEAT_INTERVAL_SEC, SESSION_TIMEOUT_SEC);
    log_info("分析周期/窗口: %ds / %ds", ANALYSIS_PERIOD_SEC, ANALYSIS_WINDOW_SEC);
    log_info("数据库     : %s@%s/%s", DB_USER, DB_HOST, DB_NAME);
    log_info("==================================================");

    /* 3. 注册退出信号：Ctrl-C(SIGINT) / kill(SIGTERM) 均走正常退出流程 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    /* 先屏蔽，避免"判断标志"与"sigsuspend"之间丢信号 */
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    sa.sa_handler = on_signal;
    sa.sa_mask    = mask;
    sa.sa_flags   = 0;                 /* 不设 SA_RESTART，保证 sigsuspend 被唤醒 */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    log_info("服务器已就绪，按 Ctrl-C 退出");

    /* 4. 主循环：S0 阶段空转等待退出信号；S1 起此处为 net_loop() */
    while (g_running) {
        sigsuspend(&oldmask);          /* 原子地解除屏蔽并等待信号 */
    }

    /* 5. 收尾 */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    log_info("收到退出信号，服务器正常退出");
    return 0;
}
