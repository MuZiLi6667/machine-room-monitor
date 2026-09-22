/*
 * db.c —— MySQL 连接与写入封装（S4：DATA 入库）
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

static MYSQL *db_conn(void);
static int    db_exec(const char *sql);
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

void db_shutdown(void)
{
    db_conn_drop();                 /* 主线程这条 */
    if (g_ready) {
        mysql_library_end();        /* 与 db_init 的 mysql_library_init 配对 */
        g_ready = 0;
    }
    log_info("数据库连接已关闭（mysql_library_end 已调用）");
}
