/*
 * main.c —— 程序入口（S4 版本：DATA 入库）
 *
 * 本文件是"组装者"：唯一知道所有模块的文件（规划 §2.2）。
 * 启动顺序：日志 → 数据库 → 线程池 → 网络监听 → select 主循环（net_loop）。
 *
 * 为什么数据库要排在线程池之前：db_init 里的 mysql_library_init() 不是线程
 * 安全的，必须在**创建任何线程之前**调用；且工作线程一旦开始跑任务就可能写库，
 * 那时连接参数必须已经就位（见 db.h 的连接模型说明）。数据库连不上直接退出——
 * 收得到数据却存不下来，属于配置错误，早点暴露比跑起来才发现好。
 *
 * S6 再补 getopt 命令行参数（-p/-t/-w）。
 */
#include <signal.h>
#include <stddef.h>

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

int main(void)
{
    struct sigaction sa;

    /* 1. 初始化日志 */
    log_init(LOG_TAG);

    /* 2. 启动信息 */
    log_info("==================================================");
    log_info("机房环境多指标远程监测与分析系统 —— 服务器端");
    log_info("版本 S4：DATA 拆包入库（每工作线程一条 MySQL 连接）");
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

    /* 4. 数据库：必须第一件事（mysql_library_init 不能在线程起来之后再调） */
    if (db_init(DB_USER, DB_PASS, DB_NAME) < 0) {
        log_err("数据库初始化失败，服务器退出");
        return 1;
    }

    /* 5. 线程池：必须先于网络就绪，保证第一条消息到达时已有消费者 */
    pool_init(POOL_THREADS);

    /* 6. 网络初始化（创建监听 socket） */
    if (net_init(SRV_PORT) < 0) {
        log_err("网络初始化失败，服务器退出");
        db_shutdown();
        return 1;
    }

    log_info("服务器已就绪，按 Ctrl-C 退出");

    /* 7. select 主循环：阻塞运行，直到收到退出信号 */
    net_loop();

    /* 8. 收尾：关闭主线程的数据库连接并释放库资源（工作线程的连接随进程退出） */
    db_shutdown();

    log_info("收到退出信号，服务器正常退出");
    return 0;
}
