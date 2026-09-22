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

/* ---- 以下三个属于 S5/S6：S4 只冻结 §2.3 的签名、不填实现 ---- */
/* 写一行告警（alarm_log 表），S5 实现 */
int db_insert_alarm(const char *client_id, int sensor_type,
                    double value, double threshold, const char *level);
/* 启动时把 threshold_cfg 读进内存，S5 实现 */
int db_load_thresholds(void);
/* 聚合近 10 分钟 → 写 analysis_result，返回生成行数；S6 实现 */
int db_run_analysis(void);

#endif /* DB_H */
