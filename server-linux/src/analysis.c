/*
 * analysis.c —— 分析线程：周期统计 + ANALYSIS 推送（S6 实现）
 *
 * 一条线程，循环三件事（规划 S6 任务 1）：
 *   ① 等一个周期（默认 60s，getopt -t 可改）；
 *   ② db_run_analysis()：把最近一个窗口（默认 600s，getopt -w 可改）的数据
 *      按 client_id × sensor_type 聚合，逐组写 analysis_result，统计结果同时
 *      留在 db 模块的内存缓存里；
 *   ③ 对**当前在线**的每个已登录客户端，把属于它自己的那些组拼成一条
 *      ANALYSIS 推送出去（接口规范 §3.1）。窗口内没有它的数据就不推空包；
 *      不在线的 client 只入库、不推送（规划 S6 任务 3）。
 *
 * 为什么统计要用 db 模块的缓存、而不是在这里查库：铁律 1——只有 db.c 能碰
 * mysql.h。db 模块内部把最近一次统计结果留了一份只读副本，本文件用
 * db_analysis_count()/db_analysis_row() 读出来（与 S5 的 db_threshold_get
 * 同一套路），没有改任何 §2.3 冻结的签名。
 *
 * "禁止把 A 的统计发给 B"的两道闸：
 *   ① 组包时只挑 client_id 与本连接**逐字符相等**的组；
 *   ② 推送前用 session_client_id_copy() 复核该 fd 现在还属于这个 client_id
 *      （快照到推送之间，fd 可能已被主线程回收并被新连接复用）。
 *
 * 线程与内存：分析线程用**自己那条** MySQL 连接（db 模块 __thread 惰性建连，
 * 首次调 db_* 时自动建立），绝不复用主线程的连接对象。本文件不碰会话表内部
 * 结构、不关闭任何连接（close 只归主线程，调试记录 §十三 要点 1）。
 * 锁序（S1 定案）：会话表锁 → 发送锁；本文件的路径都是"先取快照（会话表锁，
 * 已出锁）→ 再 net_send（发送锁）"，不会反向。
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "analysis.h"
#include "cJSON.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "net.h"
#include "session.h"

#define ANALYSIS_TS_LEN  20      /* "YYYY-MM-DD HH:MM:SS" + '\0' */

/* 退出请求：由主线程（main 的收尾流程）经 analysis_stop() 置位，分析线程在
 * 等待周期时检查。
 *
 * 这里用互斥锁 + 条件变量，**不用 volatile 标志**：volatile 只保证"不被优化
 * 掉"，不构成线程间同步，helgrind 会如实报数据竞争（S6 首轮实测 2 errors，
 * 堆栈就指向这个标志）。条件变量顺带让"收到退出请求"立刻生效，不必等分段
 * sleep 的粒度。 */
static pthread_mutex_t g_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_stop_cond = PTHREAD_COND_INITIALIZER;
static int             g_stop      = 0;      /* 受 g_stop_lock 保护 */

/*
 * 等一个统计周期。返回非 0 = 收到退出请求（analysis_stop 叫醒）。
 * pthread_cond_timedwait 既能到点返回，又能被立即叫醒——"定时等待 + 可中断"
 * 正是这个语义要的东西。
 */
static int analysis_wait(int seconds)
{
    struct timespec deadline;
    int             stop;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&g_stop_lock);
    while (!g_stop) {
        if (pthread_cond_timedwait(&g_stop_cond, &g_stop_lock, &deadline) == ETIMEDOUT) {
            break;              /* 睡满一个周期 */
        }
    }
    stop = g_stop;
    pthread_mutex_unlock(&g_stop_lock);
    return stop;
}

/*
 * 统计值按 analysis_result 的列精度（DECIMAL(10,2)）规整后再推送。
 * 两个理由：① 推送值与库里存的值完全一致——联调时"客户端显示的数字"与
 * "SQL 查出来的数字"不会对不上；② 客户端不必显示一长串浮点尾数
 * （不规整时 std 会打成 3.8810187113933439）。舍入方向与 MySQL 的 DECIMAL
 * 一致（远离零），因此两边的小数第二位逐位相同。
 */
static double analysis_round2(double v)
{
    double scaled = v * 100.0;

    return (scaled >= 0.0) ? (double)(long long)(scaled + 0.5) / 100.0
                           : (double)(long long)(scaled - 0.5) / 100.0;
}

/*
 * 给**一个**客户端组 ANALYSIS 并推送：sensors 数组只装它自己的组。
 * 与 alarm.c 的 alarm_send 同构（组 JSON → 补分帧符 → 一次 net_send），
 * 原因相同：proto_reply 是 proto.c 内部的 static，跨模块复用等于改契约。
 * 返回 0 = 已推送；-1 = 该 client 本窗口无数据（不推空包）或发送失败。
 */
static int analysis_send(int fd, const char *client_id,
                         const char *period_start, const char *period_end)
{
    cJSON *obj;
    cJSON *arr;
    char   out[MAX_LINE_SIZE + 2];
    char  *text;
    size_t len;
    int    i;
    int    cnt  = 0;
    int    nrow = db_analysis_count();

    obj = cJSON_CreateObject();
    if (obj == NULL) {
        log_err("fd=%d 内存不足，ANALYSIS 未能组装", fd);
        return -1;
    }

    /* 字段与顺序严格按接口规范 §3.1 的 ANALYSIS 行，不得增删改：
     * type / period_start / period_end / sensors[{id,avg,max,min,std,over_count}] */
    cJSON_AddStringToObject(obj, "type", "ANALYSIS");
    cJSON_AddStringToObject(obj, "period_start", period_start);
    cJSON_AddStringToObject(obj, "period_end", period_end);

    arr = cJSON_AddArrayToObject(obj, "sensors");
    if (arr == NULL) {
        log_err("fd=%d 内存不足，ANALYSIS 未能组装", fd);
        cJSON_Delete(obj);
        return -1;
    }

    for (i = 0; i < nrow; i++) {
        const db_analysis_row_t *r = db_analysis_row(i);
        cJSON                   *item;

        /* 第一道闸：只挑 client_id 与本连接完全相等的组，
         * 别的客户端的统计绝不进这个包 */
        if (r == NULL || strcmp(r->client_id, client_id) != 0) {
            continue;
        }

        item = cJSON_CreateObject();
        if (item == NULL) {
            log_err("fd=%d 内存不足，ANALYSIS 未能组装", fd);
            cJSON_Delete(obj);
            return -1;
        }
        cJSON_AddNumberToObject(item, "id", (double)r->sensor_type);
        cJSON_AddNumberToObject(item, "avg", analysis_round2(r->avg_value));
        cJSON_AddNumberToObject(item, "max", analysis_round2(r->max_value));
        cJSON_AddNumberToObject(item, "min", analysis_round2(r->min_value));
        cJSON_AddNumberToObject(item, "std", analysis_round2(r->std_value));
        cJSON_AddNumberToObject(item, "over_count", (double)r->over_count);
        cJSON_AddItemToArray(arr, item);
        cnt++;
    }

    if (cnt == 0) {
        /* 窗口内该 client 一条数据都没有（例如刚登录）：不推空 sensors 数组
         * （规划 S6-4 的二选一口径，运行说明里注明） */
        cJSON_Delete(obj);
        return -1;
    }

    text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        log_err("fd=%d 内存不足，ANALYSIS 未能序列化", fd);
        return -1;
    }

    len = strlen(text);
    if (len > (size_t)MAX_LINE_SIZE) {
        log_err("fd=%d ANALYSIS %u 字节超过单条消息上限 %d，已丢弃",
                fd, (unsigned)len, MAX_LINE_SIZE);
        cJSON_free(text);
        return -1;
    }

    /* JSON 文本与 '\n' 必须在**同一次** net_send 内发出，拆开会别的线程的
     * 回包插在中间，对端收到半截 JSON（S2 任务 4、调试记录 §十） */
    memcpy(out, text, len);
    out[len]     = MSG_DELIMITER;
    out[len + 1] = '\0';
    cJSON_free(text);

    if (net_send(fd, out) < 0) {
        log_warn("fd=%d client_id=%s ANALYSIS 推送失败，连接可能已断开", fd, client_id);
        return -1;
    }

    log_info("fd=%d client_id=%s 已推送 ANALYSIS（%d 个传感器组，%u 字节）",
             fd, client_id, cnt, (unsigned)len);
    return 0;
}

/* 一个统计周期：跑统计 → 给每个在线 client 推它自己那份 */
static void analysis_once(void)
{
    session_snapshot_t snaps[MAX_SESSIONS];
    char start[ANALYSIS_TS_LEN];
    char end[ANALYSIS_TS_LEN];
    int  written;
    int  nrow;
    int  nsnap;
    int  pushed  = 0;
    int  skipped = 0;
    int  i;

    written = db_run_analysis();     /* 写入 analysis_result 的行数 */
    nrow    = db_analysis_count();   /* 本周期统计出的组数（内存副本） */

    if (nrow <= 0) {
        return;                      /* 窗口内无数据：不写库也不推送（S6-4，
                                      * db_run_analysis 已记录日志） */
    }

    db_analysis_period(start, sizeof(start), end, sizeof(end));

    nsnap = session_snapshot_logged(snaps, MAX_SESSIONS);
    if (nsnap == 0) {
        log_info("分析：窗口 %s ~ %s 共 %d 组、入库 %d 行；当前无在线客户端，不推送",
                 start, end, nrow, written);
        return;
    }

    for (i = 0; i < nsnap; i++) {
        char now_id[CLIENT_ID_MAX_LEN + 1];

        /* 第二道闸：复核这个 fd 现在是否仍属于快照里的 client_id。
         * 快照到此刻之间，该 fd 可能已被主线程回收、号又被新连接复用。 */
        if (session_client_id_copy(snaps[i].fd, now_id, sizeof(now_id)) < 0) {
            skipped++;               /* 连接已断或已登出 */
            continue;
        }
        if (strcmp(now_id, snaps[i].client_id) != 0) {
            log_warn("分析：fd=%d 的 client_id 已由 %s 变为 %s，跳过本次推送",
                     snaps[i].fd, snaps[i].client_id, now_id);
            skipped++;
            continue;
        }

        if (analysis_send(snaps[i].fd, snaps[i].client_id, start, end) == 0) {
            pushed++;
        } else {
            skipped++;               /* 该 client 本窗口无数据，或发送失败 */
        }
    }

    log_info("分析：窗口 %s ~ %s 共 %d 组、入库 %d 行；在线 %d 个客户端，推送 %d 个（跳过 %d）",
             start, end, nrow, written, nsnap, pushed, skipped);
}

void *analysis_thread(void *arg)
{
    const analysis_cfg_t *cfg    = (const analysis_cfg_t *)arg;
    int                   period = (cfg != NULL && cfg->period_sec > 0)
                                   ? cfg->period_sec : ANALYSIS_PERIOD_SEC;
    int                   window = (cfg != NULL && cfg->window_sec > 0)
                                   ? cfg->window_sec : ANALYSIS_WINDOW_SEC;

    log_info("分析线程已启动：每 %ds 统计一次，窗口 %ds，仅推送给在线客户端",
             period, window);

    /* 先等一个周期再统计（规划 S6 任务 1 的循环语义），此后周期性地跑到
     * analysis_stop() 叫醒为止。 */
    while (!analysis_wait(period)) {
        analysis_once();
    }

    log_info("分析线程已收到退出请求，退出");
    return NULL;
}

void analysis_stop(void)
{
    pthread_mutex_lock(&g_stop_lock);
    g_stop = 1;
    pthread_cond_signal(&g_stop_cond);      /* 叫醒正在等周期的分析线程 */
    pthread_mutex_unlock(&g_stop_lock);
    /* 调用方随后应 pthread_join() 等它真正退出（见 analysis.h） */
}
