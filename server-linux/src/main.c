/*
 * main.c —— 程序入口（S6 版本：DATA 入库 + 阈值告警 + 周期统计/ANALYSIS 推送）
 *
 * 本文件是"组装者"：唯一知道所有模块的文件（规划 §2.2）。
 * 启动顺序：参数解析 → 日志 → 数据库 → 阈值加载 → 线程池 → 网络监听 →
 *           分析线程 → select 主循环。
 *
 * 为什么数据库要排在线程池之前：db_init 里的 mysql_library_init() 不是线程
 * 安全的，必须在**创建任何线程之前**调用；且工作线程一旦开始跑任务就可能写库，
 * 那时连接参数必须已经就位（见 db.h 的连接模型说明）。数据库连不上直接退出——
 * 收得到数据却存不下来，属于配置错误，早点暴露比跑起来才发现好。
 * 阈值加载（S5）同理排在 pool_init 之前：第一条 DATA 就可能要判阈值；
 * 但它失败只让告警不可用，不拦服务器启动。
 *
 * 分析线程（S6）反过来要排在网络就绪**之后**：它自己等一个周期才开工，且推送
 * 目标此时才可能存在。线程用自己那条 MySQL 连接（db 模块 __thread 惰性建连），
 * 不复用主线程的连接；退出前先请它收工再 db_shutdown（见 analysis.h）。
 */
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "analysis.h"
#include "config.h"
#include "db.h"
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

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法：%s [-p 端口] [-t 分析周期秒] [-w 统计窗口秒]\n"
            "  -p  监听端口      默认 %d\n"
            "  -t  分析周期(秒)  默认 %d\n"
            "  -w  统计窗口(秒)  默认 %d\n",
            (prog != NULL) ? prog : "server",
            SRV_PORT, ANALYSIS_PERIOD_SEC, ANALYSIS_WINDOW_SEC);
}

/* 解析一个命令行整数：要求整串是十进制数字且落在 [lo, hi] 内。
 * 不用 atoi —— 它把 "abc" 静默当成 0，配置写错会以"莫名其妙的行为"暴露。 */
static int parse_int_arg(const char *s, int lo, int hi, int *out)
{
    char *end = NULL;
    long  v;

    if (s == NULL || s[0] == '\0') {
        return -1;
    }
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    pthread_t        ana_tid;
    analysis_cfg_t   ana_cfg;
    int              port    = SRV_PORT;
    int              period  = ANALYSIS_PERIOD_SEC;
    int              window  = ANALYSIS_WINDOW_SEC;
    int              ana_window;
    int              ana_started = 0;
    int              opt;

    /* 0. 命令行参数（规划 S6 任务 4 的可选加分项 C4.4）：
     *    -p 监听端口 / -t 分析周期 / -w 统计窗口，默认 9000 / 60 / 600。
     *    参数在 log_init 之前解析，好让启动横幅打印的是**实际生效**的配置。 */
    while ((opt = getopt(argc, argv, "p:t:w:")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_int_arg(optarg, 1, 65535, &port) < 0) {
                fprintf(stderr, "参数错误：-p 需要 1~65535 的端口号（收到 \"%s\"）\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case 't':
            if (parse_int_arg(optarg, 1, 86400, &period) < 0) {
                fprintf(stderr, "参数错误：-t 需要 1~86400 的秒数（收到 \"%s\"）\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case 'w':
            if (parse_int_arg(optarg, 1, 604800, &window) < 0) {
                fprintf(stderr, "参数错误：-w 需要 1~604800 的秒数（收到 \"%s\"）\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* 1. 初始化日志 */
    log_init(LOG_TAG);

    /* 2. 启动信息 */
    log_info("==================================================");
    log_info("机房环境多指标远程监测与分析系统 —— 服务器端");
    log_info("版本 S6：周期统计 + ANALYSIS 推送（analysis_result 入库）");
    log_info("监听端口   : %d", port);
    log_info("工作线程   : %d 个（任务队列 %d）", POOL_THREADS, POOL_QUEUE_SIZE);
    log_info("心跳/超时  : %ds / %ds", HEARTBEAT_INTERVAL_SEC, SESSION_TIMEOUT_SEC);
    log_info("分析周期/窗口: %ds / %ds", period, window);
    log_info("数据库     : %s@%s/%s", DB_USER, DB_HOST, DB_NAME);
    log_info("==================================================");

    /* 3. 注册退出信号。不设 SA_RESTART：让阻塞中的 select 返回 EINTR，
     *    主循环据此回到顶部检查退出标志（见 net.c） */
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 4. 数据库：必须第一件事（mysql_library_init 不能在线程起来之后再调） */
    if (db_init(DB_USER, DB_PASS, DB_NAME) < 0) {
        log_err("数据库初始化失败，服务器退出");
        return 1;
    }

    /* 5. 统计窗口（getopt -w）交给 db 模块：db_run_analysis 用它算窗口两端点。
     *    取它的返回值填进线程参数，保证"日志里显示的窗口"就是"生效的窗口"。 */
    ana_window = db_set_analysis_window(window);
    if (ana_window < period) {
        log_warn("统计窗口 %ds 小于分析周期 %ds：相邻两期的窗口不重叠，"
                 "两期之间的数据不会被统计（测试可用，验收请用默认 60/600）",
                 ana_window, period);
    }

    /* 6. 阈值：把 threshold_cfg 读进内存（告警判定 + over_count 的数据源）。
     *    必须排在 pool_init 之前——工作线程收到的第一条 DATA 就可能触发告警，
     *    那时缓存必须已经填好（pthread_create 建立 happens-before）。
     *    表为空或读取失败只影响告警与 over_count：服务器照常收数据、入库。 */
    if (db_load_thresholds() <= 0) {
        log_err("告警阈值未加载，S5 的告警功能与 S6 的 over_count 不可用"
                "（数据入库不受影响）");
    }

    /* 7. 线程池：必须先于网络就绪，保证第一条消息到达时已有消费者 */
    pool_init(POOL_THREADS);

    /* 8. 网络初始化（创建监听 socket） */
    if (net_init(port) < 0) {
        log_err("网络初始化失败，服务器退出");
        db_shutdown();
        return 1;
    }

    /* 9. 分析线程（S6）：每 period 秒统计一次并推送在线客户端。
     *    创建失败不拦服务器启动——收数据/告警/入库都照常，只是没有周期统计。 */
    ana_cfg.period_sec = period;
    ana_cfg.window_sec = ana_window;
    if (pthread_create(&ana_tid, NULL, analysis_thread, &ana_cfg) != 0) {
        log_err("分析线程创建失败：周期统计与 ANALYSIS 推送不可用"
                "（其余功能不受影响）");
    } else {
        ana_started = 1;
    }

    log_info("服务器已就绪，按 Ctrl-C 退出");

    /* 10. select 主循环：阻塞运行，直到收到退出信号 */
    net_loop();

    /* 11. 收尾：先请分析线程收工并等它退出，再关数据库——它持有自己的 MySQL
     *     连接，若在 mysql_library_end() 之后还在查询，行为未定义（analysis.h）。
     *     工作线程没有 join 机制，随进程退出（调试记录 §九 的同一取舍）。 */
    if (ana_started) {
        analysis_stop();
        pthread_join(ana_tid, NULL);
    }
    db_shutdown();

    log_info("收到退出信号，服务器正常退出");
    return 0;
}
