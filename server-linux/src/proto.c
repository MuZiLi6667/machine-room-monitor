/*
 * proto.c —— cJSON 解析与按 type 分发（S2 实现，S3 填会话业务，S4 接 DATA 入库）
 *
 * 处理流程（接口规范 §3.2 容错约定）：
 *   cJSON_Parse 失败            → ERROR code=1 "malformed json"
 *   type 缺失 / 非字符串 / 未知 → ERROR code=2 "unknown type"
 *   LOGIN                       → 注册 client_id（含踢旧）→ LOGIN_ACK status=0（S3）
 *   HEARTBEAT                   → HEARTBEAT_ACK（S3）
 *   DATA                        → 拆 sensors[] 逐条写 sensor_data（S4）→ 每个值做
 *                                 阈值判定，超标则推 ALARM + 写 alarm_log（S5）；
 *                                 **DATA 本身不回包**（ALARM 是规范 §3.1 定义的
 *                                 服务器主动消息，不是对 DATA 的应答）
 * 所有 ERROR 都只应答、不主动断开（S2-1~S2-3、S3-6）。
 *
 * 登录态拦截（S3）：DATA/HEARTBEAT 若在 LOGIN 之前到达 → ERROR code=3
 *   "not logged in"，同样不断连；连接保持后补发 LOGIN 仍可正常使用。
 *
 * 本文件跑在工作线程里：不碰会话表内部结构，一律走 session_* 与 net_send
 * 这两个线程安全接口；跨模块传递的 client_id 用 session_client_id_copy
 * 拷贝到本地缓冲，避免裸指针出锁失效（调试记录 §十 要点 6）。
 * 写库走 db_*（db 模块每线程一条连接，工作线程直接调即可，无需额外加锁）。
 */

/* strptime() 属于 X/Open（XSI）扩展，glibc 只在 _XOPEN_SOURCE 下声明它——
 * 不定义就只剩隐式声明，返回值（指针）会被当成 int 截断，且 -Wall 报警告。
 * 必须在任何 #include 之前定义（S4 实测）。 */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "cJSON.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "session.h"

#define DATETIME_STR_LEN 19          /* "YYYY-MM-DD HH:MM:SS" 恰好 19 字符 */

/* 错误码：1/2 由接口规范 §3.1 定义；3/4 是规范未定义路径上的自定义码
 * （规划 S3 任务 1、2 允许"自定义码即可"），语义见注释，不改规范字段。 */
#define ERR_CODE_MALFORMED     1   /* malformed json */
#define ERR_CODE_UNKNOWN       2   /* unknown type */
#define ERR_CODE_NOT_LOGGED    3   /* 未 LOGIN 就发 DATA/HEARTBEAT */
#define ERR_CODE_BAD_CLIENT_ID 4   /* LOGIN 的 client_id 缺失或格式非法 */

static void proto_reply(int fd, cJSON *obj);
static void proto_reply_error(int fd, int code, const char *msg);
static void proto_handle_login(int fd, const cJSON *msg);
static void proto_handle_data(int fd, const cJSON *msg);
static void proto_handle_heartbeat(int fd, const cJSON *msg);
static int  proto_datetime_ok(const cJSON *ts, char *out, size_t cap);

/* 统一回包：组 JSON 文本 + 补分帧符 '\n' + net_send（规划 S2 任务 4）。
 * 分帧符必须和 JSON 文本在**同一次** net_send 内发出：拆成两次调用会被其他
 * 工作线程的回包插在中间，对端就收到半截 JSON。 */
static void proto_reply(int fd, cJSON *obj)
{
    char   out[MAX_LINE_SIZE + 2];
    char  *text;
    size_t len;

    if (obj == NULL) {
        return;
    }

    text = cJSON_PrintUnformatted(obj);
    if (text == NULL) {
        log_err("fd=%d 组包失败（内存不足），本条应答未发送", fd);
        return;
    }

    len = strlen(text);
    if (len > (size_t)MAX_LINE_SIZE) {
        log_err("fd=%d 应答 %u 字节超过单条消息上限 %d，已丢弃",
                fd, (unsigned)len, MAX_LINE_SIZE);
        cJSON_free(text);
        return;
    }

    memcpy(out, text, len);
    out[len]     = MSG_DELIMITER;
    out[len + 1] = '\0';
    cJSON_free(text);

    if (net_send(fd, out) < 0) {
        log_warn("fd=%d 应答发送失败，连接可能已断开", fd);
    }
}

/* 组装并回一条 ERROR。字段与顺序严格按接口规范 §3.1：
 * {"type":"ERROR","code":1,"msg":"malformed json"} */
static void proto_reply_error(int fd, int code, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();

    if (obj == NULL) {
        log_err("fd=%d 内存不足，ERROR 应答未能组装", fd);
        return;
    }
    cJSON_AddStringToObject(obj, "type", "ERROR");
    cJSON_AddNumberToObject(obj, "code", (double)code);
    cJSON_AddStringToObject(obj, "msg", msg);

    proto_reply(fd, obj);
    cJSON_Delete(obj);
}

/* ---- 已知 type 的处理分支 ---- */

/*
 * LOGIN（S3）：注册 client_id → 回 {"type":"LOGIN_ACK","status":0}
 * client_id 缺失/非字符串/格式非法一律回 ERROR 且不注册（规划 S3 任务 1）。
 * 同 client_id 的旧连接由 session_login 打上待踢标记，主线程下一轮巡检回收，
 * 本连接照常应答 status=0（接口规范 §3.1：踢旧连接、应答新连接）。
 */
static void proto_handle_login(int fd, const cJSON *msg)
{
    const cJSON *cid = cJSON_GetObjectItemCaseSensitive(msg, "client_id");
    cJSON       *ack;

    if (!cJSON_IsString(cid) || cid->valuestring == NULL) {
        log_warn("fd=%d LOGIN 缺少 client_id 字段，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_BAD_CLIENT_ID);
        proto_reply_error(fd, ERR_CODE_BAD_CLIENT_ID, "invalid client_id");
        return;
    }
    if (session_login(fd, cid->valuestring) < 0) {
        proto_reply_error(fd, ERR_CODE_BAD_CLIENT_ID, "invalid client_id");
        return;                        /* session_login 内已打印具体原因 */
    }
    session_touch(fd);

    ack = cJSON_CreateObject();
    if (ack == NULL) {
        log_err("fd=%d 内存不足，LOGIN_ACK 未能组装", fd);
        return;
    }
    cJSON_AddStringToObject(ack, "type", "LOGIN_ACK");
    cJSON_AddNumberToObject(ack, "status", 0);
    proto_reply(fd, ack);
    cJSON_Delete(ack);
}

/*
 * ts 合法性判定（S4-3 / S4-4）：接口规范 §3.2 规定 ts 为 "%Y-%m-%d %H:%M:%S"
 * 且**可缺省**——缺省或非法时服务器用自身当前时间入库（SQL 的 NOW()）。
 * 这里做两道校验：
 *   ① strptime 按规范格式串逐字段解析，并要求整串被消费完：
 *      "not-a-time"、"2026-09-28 15:00:01xyz" 都在这一步被拒；
 *   ② mktime 归一化后再 strftime 回同一格式串，要求与输入逐字节相同：
 *      专拦"格式对但日期不存在"的输入（如 2 月 31 日会被归一化成 3 月 3 日）。
 *      不做这道检查的话，这种 ts 会原样拼进 SQL，在 MySQL 严格模式下报错、
 *      白丢一行数据。
 * 合法 → 拷进 out（供入库使用）并返回 1；缺省/非法 → 返回 0（调用方落 NOW()）。
 */
static int proto_datetime_ok(const cJSON *ts, char *out, size_t cap)
{
    struct tm   tmv;
    char        norm[DATETIME_STR_LEN + 1];
    const char *rest;
    time_t      t;

    if (!cJSON_IsString(ts) || ts->valuestring == NULL) {
        return 0;
    }

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_isdst = -1;                 /* 夏令时未知，交给 mktime 判断 */
    rest = strptime(ts->valuestring, "%Y-%m-%d %H:%M:%S", &tmv);
    if (rest == NULL || *rest != '\0') {
        return 0;
    }

    t = mktime(&tmv);                  /* 就地归一化 tmv */
    if (t == (time_t)-1) {
        return 0;
    }
    if (strftime(norm, sizeof(norm), "%Y-%m-%d %H:%M:%S", &tmv) == 0 ||
        strcmp(norm, ts->valuestring) != 0) {
        return 0;
    }

    snprintf(out, cap, "%s", ts->valuestring);
    return 1;
}

/*
 * DATA（S4 入库 + S5 告警）：登录态拦截 → 拆 sensors[] 逐条写 sensor_data →
 * 每个值过阈值判定（超标推 ALARM）→ 记日志。
 *
 * **本分支不产生任何"应答"**：接口规范 §3.1 没有为 DATA 定义应答，客户端
 * 也不等应答（调试记录 §十三 要点 5）——入库成功与否都不该回一条 DATA_ACK
 * 之类的东西。S5 起连接上可能因本条 DATA 收到 ALARM，但那是规范定义的
 * **服务器主动消息**（任一 DATA 值超阈值时推送），与"应答 DATA"是两回事。
 *
 * 容错口径（接口规范 §3.2：id 限 1~4、v 收 int 或 float）：
 *   单条 sensor 的 id/v 缺失、非数值、id 非整数或超出 1~4 → 跳过该条并记 WARN，
 *   其余条目照常入库；一条 DATA 里的坏数据不影响同批的好数据。
 *   整批 sensors 不是数组 → 整条丢弃（记 WARN）。
 */
static void proto_handle_data(int fd, const cJSON *msg)
{
    const cJSON *sensors;
    const cJSON *ts;
    const cJSON *it;
    char         cid[CLIENT_ID_MAX_LEN + 1];
    char         tsbuf[DATETIME_STR_LEN + 1];
    const char  *ts_use  = NULL;       /* NULL → 入库时用 SQL 的 NOW() */
    const char  *ts_show;
    char         vals[256];
    size_t       used = 0;
    int          total = 0;
    int          ins   = 0;
    int          skip  = 0;

    if (!session_is_logged(fd)) {
        log_warn("fd=%d 未登录就发 DATA，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_NOT_LOGGED);
        proto_reply_error(fd, ERR_CODE_NOT_LOGGED, "not logged in");
        return;
    }
    session_touch(fd);                 /* 合法消息刷新活跃时间（规划 S3 任务 4） */

    /* 入库要用的 client_id 必须是锁内拷出来的本地副本：session_client_id()
     * 返回的是会话槽内部指针，出锁即可能因连接被踢/回收而失效
     * （调试记录 §十 要点 6、§十三 要点 4）。 */
    if (session_client_id_copy(fd, cid, (int)sizeof(cid)) < 0) {
        cid[0] = '\0';                 /* 理论上不会发生（上面刚判过登录态） */
    }

    ts = cJSON_GetObjectItemCaseSensitive(msg, "ts");
    if (proto_datetime_ok(ts, tsbuf, sizeof(tsbuf))) {
        ts_use  = tsbuf;
        ts_show = tsbuf;
    } else {
        ts_show = (ts != NULL) ? "(非法，改用服务器 NOW())"
                               : "(缺省，用服务器 NOW())";
    }

    vals[0] = '\0';
    sensors = cJSON_GetObjectItemCaseSensitive(msg, "sensors");
    if (!cJSON_IsArray(sensors)) {
        log_warn("fd=%d client_id=%s DATA 的 sensors 不是数组，本条整条丢弃（未入库）",
                 fd, cid);
        return;
    }

    cJSON_ArrayForEach(it, sensors) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *v  = cJSON_GetObjectItemCaseSensitive(it, "v");
        int          sid;
        int          w;

        total++;

        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(v)) {
            skip++;
            log_warn("fd=%d client_id=%s DATA 跳过一条：id/v 缺失或非数值", fd, cid);
            continue;
        }
        sid = (int)id->valuedouble;
        if ((double)sid != id->valuedouble ||
            sid < SENSOR_ID_MIN || sid > SENSOR_ID_MAX) {
            skip++;
            log_warn("fd=%d client_id=%s DATA 跳过一条：id=%g 不是 %d~%d 的整数",
                     fd, cid, id->valuedouble, SENSOR_ID_MIN, SENSOR_ID_MAX);
            continue;
        }

        /* 一个传感器值一行；v 是 int 还是 float 都走同一条路（cJSON 一律按
         * double 取出），由 DECIMAL(10,2) 落库 */
        if (db_insert_sensor(cid, sid, v->valuedouble, ts_use) == 0) {
            ins++;
        } else {
            skip++;                    /* db 内已记 ERROR 日志 */
        }

        /* S5：同一条 DATA 先入库再告警（规划 S5 任务 3，在入库循环里顺带调用）。
         * 无论入库成败都做判定——ALARM 是给客户端的实时通知，不该被
         * sensor_data 的写入失败连累；alarm 内部写 alarm_log 失败会自己记日志。
         * 传入的是上面拷好的 client_id 本地副本（出锁即失效的裸指针不能给它）。 */
        alarm_check(fd, cid, sid, v->valuedouble);

        /* 汇总成 [1=25.6 2=55 3=312 4=356]，与 S3-2 的日志格式一致，
         * 便于和 SELECT 出来的行逐值核对（S4-2） */
        if (used < sizeof(vals) - 1) {
            w = snprintf(vals + used, sizeof(vals) - used, "%s%d=%g",
                         (used > 0) ? " " : "", sid, v->valuedouble);
            if (w > 0) {
                used += (size_t)w;
            }
        }
    }

    log_info("fd=%d client_id=%s DATA：ts=%s 入库 %d/%d 行（跳过 %d）[%s]",
             fd, cid, ts_show, ins, total, skip, vals);
}

/*
 * HEARTBEAT（S3）：回 {"type":"HEARTBEAT_ACK"}；未登录同样拦截。
 * 心跳只证明链路存活，不承载业务字段（接口规范 §3.1）。
 */
static void proto_handle_heartbeat(int fd, const cJSON *msg)
{
    cJSON *ack;

    (void)msg;

    if (!session_is_logged(fd)) {
        log_warn("fd=%d 未登录就发 HEARTBEAT，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_NOT_LOGGED);
        proto_reply_error(fd, ERR_CODE_NOT_LOGGED, "not logged in");
        return;
    }
    session_touch(fd);

    ack = cJSON_CreateObject();
    if (ack == NULL) {
        log_err("fd=%d 内存不足，HEARTBEAT_ACK 未能组装", fd);
        return;
    }
    cJSON_AddStringToObject(ack, "type", "HEARTBEAT_ACK");
    proto_reply(fd, ack);
    cJSON_Delete(ack);
}

void proto_handle(int fd, const char *json_line)
{
    cJSON       *root;
    const cJSON *type;

    if (json_line == NULL) {
        return;
    }

    /* 标识工作线程：与 net.c 的"主线程"日志对比线程号即可验证 S2-4 */
    log_info("fd=%d [工作线程] 解析消息：%.200s", fd, json_line);

    root = cJSON_Parse(json_line);
    if (root == NULL) {
        log_warn("fd=%d JSON 解析失败，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_MALFORMED);
        proto_reply_error(fd, ERR_CODE_MALFORMED, "malformed json");
        return;
    }

    /* type 缺失或不是字符串，与"未知 type"同码处理（含 S2-3 的 {"foo":1}） */
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        log_warn("fd=%d 缺少 type 字段，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_UNKNOWN);
        proto_reply_error(fd, ERR_CODE_UNKNOWN, "unknown type");
    } else if (strcmp(type->valuestring, "LOGIN") == 0) {
        proto_handle_login(fd, root);
    } else if (strcmp(type->valuestring, "DATA") == 0) {
        proto_handle_data(fd, root);
    } else if (strcmp(type->valuestring, "HEARTBEAT") == 0) {
        proto_handle_heartbeat(fd, root);
    } else {
        log_warn("fd=%d 未知 type=\"%s\"，回 ERROR code=%d（连接保持）",
                 fd, type->valuestring, ERR_CODE_UNKNOWN);
        proto_reply_error(fd, ERR_CODE_UNKNOWN, "unknown type");
    }

    cJSON_Delete(root);     /* 每条消息都要释放解析树，否则逐条泄漏 */
}
