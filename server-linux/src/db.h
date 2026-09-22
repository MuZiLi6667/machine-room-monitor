/*
 * db.h —— 数据库对外契约（规划 §2.3，S4 实现连接与写入）
 *
 * 铁律 1（规划 §2.2）：只有 db.c 可以 #include <mysql/mysql.h>；
 * 其他模块要数据一律调本文件的函数，不得直接碰 MySQL。
 *
 * 连接模型（规划 S4 任务 1）：**每个线程一条 MySQL 连接**，用 __thread 保存、
 * 首次使用时惰性建立。libmysqlclient 的连接不能跨线程并发使用，每线程独立
 * 连接既绕开这条限制，也免去一把全局数据库锁——所以本模块全部函数都是
 * 线程安全的，任意工作线程可直接调用。
 *
 * 表结构与字段以 `docs/接口规范.md` §4 为唯一依据，禁止擅改。
 */
#ifndef DB_H
#define DB_H

#include <stddef.h>         /* size_t：db_analysis_period 的缓冲长度参数 */

#define DB_LEVEL_LEN 9      /* threshold_cfg.level VARCHAR(8) + 结尾 '\0' */

/* 一个传感器的阈值配置（threshold_cfg 一行的只读副本）。
 * has_min / has_max 为 0 表示该侧在表里是 NULL → **不检查该侧**
 * （烟雾只有上限，min 就是 NULL）。level 取 LOW/MID/HIGH。 */
typedef struct {
    double min_value;
    double max_value;
    int    has_min;
    int    has_max;
    char   level[DB_LEVEL_LEN];
} db_threshold_t;

/* 保存连接参数并做一次连通性自检（主线程在 pool_init 之前调用）。
 * 成功返回 0；参数非法或数据库连不上返回 -1（调用方应视为致命错误）。
 * 这里自检的是主线程这条连接，工作线程的连接在各自首次写库时惰性建立。 */
int db_init(const char *user, const char *pass, const char *dbname);

/* 往 sensor_data 写一行（每个传感器值一行）。
 *   client_id   ：1~31 字符，函数内部用 mysql_real_escape_string 转义（防注入）
 *   sensor_type ：1~4（温度/湿度/烟雾/光照），越界直接返回 -1
 *   value       ：传感器读数，按 DECIMAL(10,2) 存
 *   datetime    ："YYYY-MM-DD HH:MM:SS"；**传 NULL 表示用服务器当前时间（SQL 的 NOW()）**
 * 成功返回 0；失败返回 -1（只记日志、不中断调用方——一条数据写不进去
 * 不应该影响这个连接上后续的 DATA）。 */
int db_insert_sensor(const char *client_id, int sensor_type,
                     double value, const char *datetime);

/* 关闭**调用线程**的连接并释放库资源（主线程在退出前调用一次）。
 * 常驻工作线程的连接不在此列，随进程退出由内核回收（线程池没有 shutdown
 * 机制，见调试记录 §九 的说明）。 */
void db_shutdown(void);

/* 关闭**本线程**的 MySQL 连接（S8 新增，修调试记录问题 40）。
 *
 * 专供"会真正退出的线程"在自己退出前调用：连接存在线程局部 __thread 里，
 * 线程一退出 TLS 就销毁，主线程再也拿不到那个指针——连接若不先关掉，就成了
 * 没人指向的内存（memcheck 报 definitely lost）。分析线程被 pthread_join 真
 * 回收，属于这一类。
 *
 * **常驻工作线程不要调**（它们没有退出路径，连接随进程退出回收）；
 * 调用后若本线程还要写库，db_* 会自动重建连接，无需额外处理。 */
void db_close_this_thread(void);

/* 写一行告警（alarm_log 表，S5 实现）。
 *   client_id   ：内部转义防注入
 *   sensor_type ：1~4，越界返回 -1
 *   value       ：本次读数；threshold：被越过的那个边界
 *   level       ：LOW/MID/HIGH（来自 threshold_cfg）
 * alarm_time 由 SQL 的 NOW() 填——本函数签名（规划 §2.3 冻结）没有时间参数。
 * 成功返回 0，失败返回 -1（只记日志，不影响调用方继续处理）。 */
int db_insert_alarm(const char *client_id, int sensor_type,
                    double value, double threshold, const char *level);

/* 启动时把 threshold_cfg 读进内存数组（主线程在 pool_init 之前调用一次）。
 * 返回读到的行数（可能为 0 = 表空 → 告警不会触发）；查询失败返回 -1。
 * 加载后这份缓存只读，任意线程可经 db_threshold_get() 读取。 */
int db_load_thresholds(void);

/* 取某传感器的阈值（读 db_load_thresholds 建立的缓存）。
 * 成功返回 0 并把配置写进 *out；**无该传感器配置返回 -1**——调用方据此
 * 判"这型传感器不告警"（光照在 threshold_cfg 里不插行）。S5 起由 alarm.c 使用。 */
int db_threshold_get(int sensor_type, db_threshold_t *out);

/* ---- S6：周期统计 ---- */

/* 一条分析结果 = analysis_result 一行的只读副本（字段与接口规范 §4 同名）。
 * std_value 对"窗口内只有一个样本"或"所有样本相同"的组为 0（不是 NULL）；
 * over_count 是窗口内该组超过阈值的数据条数（与 ALARM 同一判定口径）。 */
typedef struct {
    char   client_id[CLIENT_ID_MAX_LEN + 1];
    int    sensor_type;
    double avg_value;
    double max_value;
    double min_value;
    double std_value;
    int    over_count;
} db_analysis_row_t;

/* 设置统计窗口秒数（main 解析 getopt -w 后调用；不调用则用
 * config.h 的 ANALYSIS_WINDOW_SEC）。参数 <1 时回退到默认值。
 * 必须在分析线程启动前调用（靠 pthread_create 建立 happens-before）。
 * 返回实际生效的窗口秒数。 */
int db_set_analysis_window(int sec);

/* 聚合"最近 window 秒"的 sensor_data → 逐组写 analysis_result，返回**写入
 * 的行数**（0 = 窗口内无数据，此时不写库）。由分析线程每周期调用一次。
 *
 * 查询走 db_query()（带结果集的 SELECT + 断线重连一次）；窗口两端点取自
 * 本函数执行时的服务器时间，并原样作为 period_start/period_end 与 SQL 的
 * WHERE 字面量——推送里的窗口与实际统计的窗口因此逐字节一致。
 * over_count 的判定阈值来自 db_threshold_get() 的内存缓存（与 alarm.c 同源同
 * 口径：严格不等号、NULL 侧不检查、无配置不判），不另查一次库。
 *
 * 统计结果同时留在本模块内部缓存里，供 analysis.c 组包推送（见下面两个
 * getter）——分析线程自己写、自己读，故无需加锁。 */
int  db_run_analysis(void);

/* 取最近一次 db_run_analysis 的窗口（ANALYSIS 的 period_start / period_end）。 */
void db_analysis_period(char *start_out, size_t scap, char *end_out, size_t ecap);

/* 最近一次 db_run_analysis 统计出的组数 / 第 idx 组（越界返回 NULL）。
 * 返回的是 db.c 内部缓存的指针，**只允许分析线程访问**（写者与读者都是它，
 * 下一次 db_run_analysis 会原地覆盖）。 */
int  db_analysis_count(void);
const db_analysis_row_t *db_analysis_row(int idx);

#endif /* DB_H */
