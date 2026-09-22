/*
 * analysis.h —— 分析线程对外契约（规划 §2.3 冻结签名，S6 实现）
 *
 * 职责：每 period_sec 秒跑一次统计（db_run_analysis）→ 把结果组包成 ANALYSIS
 * 推送给**当前在线**的客户端；不在线的 client 只入库、不推送。
 *
 * 与其它模块的关系：本模块不碰 MySQL（铁律 1，统计都经 db.h），不碰套接字
 * （铁律 2，推送走 net_send），不碰会话表内部结构（铁律 3，用 session_* 函数）。
 */
#ifndef ANALYSIS_H
#define ANALYSIS_H

/* analysis_thread 的参数。§2.3 冻结的签名是 `void *analysis_thread(void *arg)`，
 * 配置经这个结构体传入（由 main 持有，pthread_create 建立 happens-before，
 * 分析线程读到的一定是初始化好的值）。 */
typedef struct {
    int period_sec;      /* 统计周期秒数（getopt -t，默认 config.h 的 ANALYSIS_PERIOD_SEC） */
    int window_sec;      /* 统计窗口秒数（getopt -w）。只用于日志显示——真正生效的
                          * 窗口由 db_set_analysis_window() 交给 db 模块，填这里时
                          * 请用它的返回值，保证两处是同一个数。 */
} analysis_cfg_t;

/* 分析线程主函数：循环 { 等一个周期 → 统计 → 逐 client 推送 }。
 * 在 main 中用 pthread_create 启动；线程只在收到 analysis_stop() 请求后才返回，
 * 否则一直随进程运行。arg 传 analysis_cfg_t*（传 NULL 则用默认周期）。 */
void *analysis_thread(void *arg);

/* 请求分析线程收工（main 在 net_loop 返回后调用）。
 * 属新增（§2.3 未列，与 db_shutdown 同类）——分析线程持有自己的 MySQL 连接，
 * 若它在 db_shutdown() 调 mysql_library_end() 之后还在查询，行为未定义。
 * 内部用条件变量叫醒线程（不是标志轮询），调用后请 pthread_join() 等它退出：
 * 正常情况下马上返回，最多等它跑完手上那一次统计查询。 */
void analysis_stop(void);

#endif /* ANALYSIS_H */
