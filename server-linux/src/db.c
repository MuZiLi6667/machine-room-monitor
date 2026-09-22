/*
 * db.c —— MySQL 连接与写入封装（S4：DATA 入库；S5：阈值加载 + 告警入库）
 *
 * 本文件是**唯一**包含 <mysql/mysql.h> 的地方（规划 §2.2 铁律 1），
 * 表结构与字段严格按 `docs/接口规范.md` §4，不增删字段。
 *
 * 连接模型（规划 S4 任务 1）：**每线程一条连接**。
 *   libmysqlclient 的 MYSQL* 不是线程安全的，同一条连接被两个线程并发 query
 *   会读到交错的应答包；最省事的解法是"每个工作线程一条自己的连接"，用
 *   __thread 存着、首次用到时才建（含 mysql_thread_init）。相比"全局一条连接
 *   + 一把大锁"，这里没有数据库锁竞争：4 个线程各写各的，互不等待。
 *
 * 调用时序要求：db_init() 必须在本进程**任何其它线程**之前调用（main.c 里排在
 * pool_init 之前）。工作线程读到的连接参数由 pthread_create 建立 happens-before，
 * 之后参数不再改动，因此无需加锁。
 *
 * 本模块跑在工作线程里 —— 与 proto 一样，只做"锁外"的事：不碰会话表内部结构、
 * 不调用 net_close（关连接只归主线程，调试记录 §十三 要点 1）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql/mysql.h>

#include "config.h"
#include "db.h"
#include "log.h"

/* MySQL 客户端错误码：连接被服务端断开的两类（对应 <mysql/errmsg.h> 的
 * CR_SERVER_GONE_ERROR / CR_SERVER_LOST）。这里直接写字面量，目的是不让
 * db.c 依赖 errmsg.h 的头文件位置——数值是客户端协议里稳定的公开约定。 */
#ifndef CR_SERVER_GONE_ERROR
#define CR_SERVER_GONE_ERROR 2006
#endif
#ifndef CR_SERVER_LOST
#define CR_SERVER_LOST       2013
#endif

#define DB_CONN_TIMEOUT_SEC  5      /* 建连超时：数据库没起时不要吊死工作线程 */
#define DB_SQL_BUF_SIZE      512    /* 单条 INSERT 语句缓冲（转义后仍远小于此） */

/* 连接参数：db_init 时从入参拷进来，之后只读（见文件头说明） */
static char g_host[64]   = DB_HOST;
static char g_user[64];
static char g_pass[64];
static char g_dbname[64];
static int  g_ready      = 0;       /* 0 = 尚未 db_init */

/* 每线程一条连接。__thread 让 4 个工作线程各持一份，互不干扰；
 * 每份只在**本线程**内被读写，故无需任何锁保护。 */
static __thread MYSQL *t_conn = NULL;

/* 阈值缓存（S5）：threshold_cfg 的只读副本，下标 = sensor_type（1~4）。
 * 由 db_load_thresholds() 在**主线程、pool_init 之前**填好，此后不再改动，
 * 工作线程只读——pthread_create 建立的 happens-before 保证它们看到的是
 * 填好的内容，故无需加锁（与连接参数 g_user 等同一套理由）。 */
typedef struct {
    int            valid;              /* 1 = 表里有这一行 */
    db_threshold_t t;
} db_thr_slot_t;

static db_thr_slot_t g_thr[SENSOR_ID_MAX + 1];

static MYSQL *db_conn(void);
static int    db_exec(const char *sql);
static int    db_query(const char *sql, MYSQL_RES **res_out);
static void   db_conn_drop(void);

/* 把入参拷进定长缓冲，超长即判非法（避免静默截断出一个错的用户名/库名） */
static int db_copy_param(char *dst, size_t cap, const char *src, const char *what)
{
    int n;

    if (src == NULL || src[0] == '\0') {
        log_err("数据库%s为空，初始化失败", what);
        return -1;
    }
    n = snprintf(dst, cap, "%s", src);
    if (n < 0 || (size_t)n >= cap) {
        log_err("数据库%s过长（上限 %u 字节），初始化失败", what, (unsigned)cap - 1);
        return -1;
    }
    return 0;
}

int db_init(const char *user, const char *pass, const char *dbname)
{
    if (db_copy_param(g_user,   sizeof(g_user),   user,   "用户名")   < 0 ||
        db_copy_param(g_pass,   sizeof(g_pass),   pass,   "密码")     < 0 ||
        db_copy_param(g_dbname, sizeof(g_dbname), dbname, "库名")     < 0) {
        return -1;
    }

    /* mysql_library_init 不是线程安全的，必须在创建任何线程之前调用一次；
     * 之后各线程再各自 mysql_thread_init（见 db_conn）。 */
    if (mysql_library_init(0, NULL, NULL) != 0) {
        log_err("mysql_library_init() 失败，数据库模块不可用");
        return -1;
    }
    if (!mysql_thread_safe()) {
        /* 编译出来的 libmysqlclient 未开线程安全：每线程一条连接也救不了 */
        log_err("libmysqlclient 非线程安全版本，拒绝启动");
        mysql_library_end();
        return -1;
    }

    g_ready = 1;

    /* 自检一条连接：把"库不存在/密码错/MySQL 没起"这类错误挡在服务启动阶段，
     * 而不是等到第一条 DATA 到达才在日志里刷错误。主线程这条连接同时留给
     * 后续（S6 的分析线程若跑在主线程）使用。 */
    if (db_conn() == NULL) {
        log_err("数据库初始化失败：%s@%s/%s 连不上，服务器退出", user, g_host, dbname);
        g_ready = 0;
        mysql_library_end();
        return -1;
    }

    log_info("数据库已就绪：%s@%s/%s（每线程一条连接，字符集 utf8mb4）",
             user, g_host, dbname);
    return 0;
}

/*
 * 取本线程的连接，没有就建一条（惰性建连）。
 * 返回 NULL 表示本线程当前无法写库——调用方按"这一条数据写失败"处理，
 * 不影响网络侧继续收发（数据库短暂不可用不该让服务器停止工作）。
 */
static MYSQL *db_conn(void)
{
    unsigned int tmo = DB_CONN_TIMEOUT_SEC;

    if (t_conn != NULL) {
        return t_conn;
    }
    if (!g_ready) {
        log_err("数据库尚未初始化（db_init 未成功），本条数据未写入");
        return NULL;
    }

    /* 每个线程首次接触 MySQL 前必须初始化线程局部环境，否则 mysql_init /
     * 查询里会碰未初始化的线程数据（规划 S4 任务 1）。 */
    if (mysql_thread_init() != 0) {
        log_err("mysql_thread_init() 失败，本线程无法建库连接");
        return NULL;
    }

    t_conn = mysql_init(NULL);
    if (t_conn == NULL) {
        log_err("mysql_init() 失败（内存不足），本线程无法建库连接");
        return NULL;
    }

    /* 字符集与库一致（接口规范 §4：utf8mb4）；建连超时避免数据库没起时
     * 工作线程长时间吊死（队列有背压，会一路顶到主线程）。 */
    mysql_options(t_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    mysql_options(t_conn, MYSQL_OPT_CONNECT_TIMEOUT, &tmo);

    if (mysql_real_connect(t_conn, g_host, g_user, g_pass, g_dbname,
                           0, NULL, 0) == NULL) {
        log_err("连接 MySQL 失败：%s（%s@%s/%s）",
                mysql_error(t_conn), g_user, g_host, g_dbname);
        mysql_close(t_conn);
        t_conn = NULL;
        return NULL;
    }

    log_info("本线程已建立 MySQL 连接（每线程一条，%s@%s/%s）",
             g_user, g_host, g_dbname);
    return t_conn;
}

/* 丢弃本线程的连接（下次 db_conn 会重建） */
static void db_conn_drop(void)
{
    if (t_conn != NULL) {
        mysql_close(t_conn);
        t_conn = NULL;
    }
}

/*
 * 执行一条无结果集的 SQL。
 * 连接被服务端断开（wait_timeout 到期、MySQL 重启过）时重连一次再试：
 * 否则服务器会从那一秒起每条数据都写不进去，直到人工重启——这是 S7 压测
 * 用 10 分钟以上长跑时最容易撞上的失效。
 */
static int db_exec(const char *sql)
{
    MYSQL *c;

    c = db_conn();
    if (c == NULL) {
        return -1;
    }

    if (mysql_query(c, sql) != 0) {
        unsigned int err = mysql_errno(c);

        if (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST) {
            log_warn("MySQL 连接已断开（%u: %s），本线程重连后重试一次",
                     err, mysql_error(c));
            db_conn_drop();
            c = db_conn();
            if (c == NULL || mysql_query(c, sql) != 0) {
                log_err("重连后仍写入失败：%s",
                        (c != NULL) ? mysql_error(c) : "连接建立失败");
                return -1;
            }
            return 0;
        }

        log_err("SQL 执行失败（%u: %s）：%.200s", err, mysql_error(c), sql);
        return -1;
    }

    return 0;
}

/*
 * 执行一条**带结果集**的 SELECT，结果集经 *res_out 交给调用方
 * （调用方负责 mysql_free_result）。与 db_exec 同样的"断线重连一次"兜底。
 * 成功返回 0；失败返回 -1 且 *res_out = NULL。S5 的阈值加载走这里，
 * S6 的聚合查询同样可复用它，不必再各写一遍 mysql_query。
 */
static int db_query(const char *sql, MYSQL_RES **res_out)
{
    MYSQL *c;

    if (res_out == NULL) {
        return -1;
    }
    *res_out = NULL;

    c = db_conn();
    if (c == NULL) {
        return -1;
    }

    if (mysql_query(c, sql) != 0) {
        unsigned int err = mysql_errno(c);

        if (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST) {
            log_warn("MySQL 连接已断开（%u: %s），本线程重连后重试一次",
                     err, mysql_error(c));
            db_conn_drop();
            c = db_conn();
            if (c == NULL || mysql_query(c, sql) != 0) {
                log_err("重连后仍查询失败：%s",
                        (c != NULL) ? mysql_error(c) : "连接建立失败");
                return -1;
            }
        } else {
            log_err("SQL 查询失败（%u: %s）：%.200s", err, mysql_error(c), sql);
            return -1;
        }
    }

    *res_out = mysql_store_result(c);
    if (*res_out == NULL) {
        /* 对 SELECT 而言只有出错时才会是 NULL（结果集为空时 store_result
         * 返回的是非 NULL 的空结果集） */
        log_err("取结果集失败（%s）：%.200s", mysql_error(c), sql);
        return -1;
    }
    return 0;
}

/*
 * 写一行 sensor_data。
 * datetime == NULL → 用服务器的 NOW()，这正是"ts 缺省或非法"时的落点
 * （判定在 proto.c，见 proto_datetime_ok）。
 * 两个字符串字段都先过 mysql_real_escape_string，杜绝拼接注入。
 */
int db_insert_sensor(const char *client_id, int sensor_type,
                     double value, const char *datetime)
{
    MYSQL *c;
    char   esc_id[CLIENT_ID_MAX_LEN * 2 + 1];   /* 最坏情况每个字节都被转义 */
    char   esc_ts[64];
    char   sql[DB_SQL_BUF_SIZE];
    int    n;

    if (client_id == NULL) {
        log_err("client_id 为空，本条数据未写入");
        return -1;
    }
    if (sensor_type < SENSOR_ID_MIN || sensor_type > SENSOR_ID_MAX) {
        log_err("sensor_type=%d 超出 %d~%d，本条数据未写入",
                sensor_type, SENSOR_ID_MIN, SENSOR_ID_MAX);
        return -1;
    }

    /* 转义也必须用本线程自己的连接（字符集随连接走） */
    c = db_conn();
    if (c == NULL) {
        return -1;
    }

    mysql_real_escape_string(c, esc_id, client_id, (unsigned long)strlen(client_id));
    if (datetime != NULL) {
        mysql_real_escape_string(c, esc_ts, datetime, (unsigned long)strlen(datetime));
    }

    if (datetime == NULL) {
        n = snprintf(sql, sizeof(sql),
                     "INSERT INTO sensor_data"
                     "(client_id, sensor_type, sensor_value, collect_time) "
                     "VALUES ('%s', %d, %.2f, NOW())",
                     esc_id, sensor_type, value);
    } else {
        n = snprintf(sql, sizeof(sql),
                     "INSERT INTO sensor_data"
                     "(client_id, sensor_type, sensor_value, collect_time) "
                     "VALUES ('%s', %d, %.2f, '%s')",
                     esc_id, sensor_type, value, esc_ts);
    }
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        log_err("SQL 语句超长（>%d 字节），本条数据未写入", DB_SQL_BUF_SIZE - 1);
        return -1;
    }

    return db_exec(sql);
}

/*
 * ---- S5：告警入库与阈值加载 ----
 */

/*
 * 写一行 alarm_log（接口规范 §4）。
 * alarm_time 用 SQL 的 NOW()：本函数签名（规划 §2.3 冻结）没有时间参数，
 * 这与 ALARM 推送里 alarm.c 用 strftime 生成的 ts 同为服务器时钟，两者
 * 相差 <1s（下一次告警查询时不必纠结这 1 秒）。
 */
int db_insert_alarm(const char *client_id, int sensor_type,
                    double value, double threshold, const char *level)
{
    MYSQL *c;
    char   esc_id[CLIENT_ID_MAX_LEN * 2 + 1];
    char   esc_lv[DB_LEVEL_LEN * 2];
    char   sql[DB_SQL_BUF_SIZE];
    int    n;

    if (client_id == NULL || level == NULL) {
        log_err("client_id/level 为空，本条告警未写入");
        return -1;
    }
    if (sensor_type < SENSOR_ID_MIN || sensor_type > SENSOR_ID_MAX) {
        log_err("sensor_type=%d 超出 %d~%d，本条告警未写入",
                sensor_type, SENSOR_ID_MIN, SENSOR_ID_MAX);
        return -1;
    }

    c = db_conn();
    if (c == NULL) {
        return -1;
    }

    mysql_real_escape_string(c, esc_id, client_id, (unsigned long)strlen(client_id));
    mysql_real_escape_string(c, esc_lv, level, (unsigned long)strlen(level));

    /* value/level 是 MySQL 的非保留关键字，加反引号消除歧义（字段名不变） */
    n = snprintf(sql, sizeof(sql),
                 "INSERT INTO alarm_log"
                 "(client_id, sensor_type, `value`, threshold, `level`, alarm_time) "
                 "VALUES ('%s', %d, %.2f, %.2f, '%s', NOW())",
                 esc_id, sensor_type, value, threshold, esc_lv);
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        log_err("SQL 语句超长（>%d 字节），本条告警未写入", DB_SQL_BUF_SIZE - 1);
        return -1;
    }

    return db_exec(sql);
}

/*
 * 启动时把 threshold_cfg 读进内存（规划 S5 任务 1）。
 * 调用时机：主线程、db_init 之后、pool_init 之前——工作线程第一条 DATA 就可能
 * 触发告警，缓存必须已就绪；且加载用的是主线程这条连接。
 *
 * 表里 min_value / max_value 可以是 NULL：DECIMAL 从 MySQL 取出来是**字符串**，
 * NULL 则取到 C 的 NULL 指针，据此置 has_min/has_max = 0（该侧不检查）。
 * 光照不插行 → 没有 sensor_type=4 的槽位 → alarm 那边查不到配置就不告警。
 */
int db_load_thresholds(void)
{
    MYSQL_RES *res = NULL;
    MYSQL_ROW  row;
    int        n = 0;

    memset(g_thr, 0, sizeof(g_thr));

    if (db_query("SELECT sensor_type, min_value, max_value, `level` "
                 "FROM threshold_cfg", &res) < 0) {
        log_err("无法读取 threshold_cfg，本次不启用告警（其余功能不受影响）");
        return -1;
    }

    while ((row = mysql_fetch_row(res)) != NULL) {
        db_thr_slot_t *slot;
        char           minbuf[32];
        char           maxbuf[32];
        int            st;

        /* 数值列也按字符串取回，用 atoi/strtod 转换（DECIMAL 没有二进制形式） */
        st = (row[0] != NULL) ? atoi(row[0]) : 0;
        if (st < SENSOR_ID_MIN || st > SENSOR_ID_MAX) {
            log_warn("threshold_cfg 的 sensor_type=%s 不在 %d~%d，忽略该行",
                     (row[0] != NULL) ? row[0] : "(NULL)", SENSOR_ID_MIN, SENSOR_ID_MAX);
            continue;
        }

        slot = &g_thr[st];
        if (row[1] != NULL) {
            slot->t.has_min   = 1;
            slot->t.min_value = strtod(row[1], NULL);
        }
        if (row[2] != NULL) {
            slot->t.has_max   = 1;
            slot->t.max_value = strtod(row[2], NULL);
        }

        if (row[3] != NULL && row[3][0] != '\0') {
            snprintf(slot->t.level, sizeof(slot->t.level), "%s", row[3]);
        } else {
            /* level 缺失时按最严重级别兜底：宁可误报，不可漏报 */
            snprintf(slot->t.level, sizeof(slot->t.level), "%s", "HIGH");
            log_warn("threshold_cfg 的 sensor_type=%d 未配 level，按 HIGH 兜底", st);
        }

        slot->valid = 1;
        n++;

        if (slot->t.has_min) {
            snprintf(minbuf, sizeof(minbuf), "%.2f", slot->t.min_value);
        } else {
            snprintf(minbuf, sizeof(minbuf), "不检查");
        }
        if (slot->t.has_max) {
            snprintf(maxbuf, sizeof(maxbuf), "%.2f", slot->t.max_value);
        } else {
            snprintf(maxbuf, sizeof(maxbuf), "不检查");
        }
        log_info("阈值已加载：sensor=%d 下限=%s 上限=%s level=%s",
                 st, minbuf, maxbuf, slot->t.level);
    }

    mysql_free_result(res);

    if (n == 0) {
        log_err("threshold_cfg 表为空，任何数据都不会触发告警"
                "（请执行 sql/init.sql 写入初始阈值）");
        return 0;
    }

    log_info("阈值加载完成：%d 个传感器有配置（未配置的如光照不告警）", n);
    return n;
}

/*
 * 取某传感器的阈值配置。读的是上面那份只读缓存，多线程安全。
 * 返回 -1 表示"没有这个传感器的配置"——这是**正常路径**（光照），不是错误，
 * 故不打日志（每条光照数据都打一条日志会刷屏）。
 */
int db_threshold_get(int sensor_type, db_threshold_t *out)
{
    if (out == NULL ||
        sensor_type < SENSOR_ID_MIN || sensor_type > SENSOR_ID_MAX) {
        return -1;
    }
    if (!g_thr[sensor_type].valid) {
        return -1;
    }

    *out = g_thr[sensor_type].t;
    return 0;
}

void db_shutdown(void)
{
    db_conn_drop();                 /* 主线程这条 */
    if (g_ready) {
        mysql_library_end();        /* 与 db_init 的 mysql_library_init 配对 */
        g_ready = 0;
    }
    log_info("数据库连接已关闭（mysql_library_end 已调用）");
}
