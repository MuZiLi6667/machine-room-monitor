/*
 * proto.c —— cJSON 解析与按 type 分发（S2 实现，S3 填会话业务）
 *
 * 处理流程（接口规范 §3.2 容错约定）：
 *   cJSON_Parse 失败            → ERROR code=1 "malformed json"
 *   type 缺失 / 非字符串 / 未知 → ERROR code=2 "unknown type"
 *   LOGIN                       → 注册 client_id（含踢旧）→ LOGIN_ACK status=0（S3）
 *   HEARTBEAT                   → HEARTBEAT_ACK（S3）
 *   DATA                        → S3 只记录；S4 起入库并触发告警
 * 所有 ERROR 都只应答、不主动断开（S2-1~S2-3、S3-6）。
 *
 * 登录态拦截（S3）：DATA/HEARTBEAT 若在 LOGIN 之前到达 → ERROR code=3
 *   "not logged in"，同样不断连；连接保持后补发 LOGIN 仍可正常使用。
 *
 * 本文件跑在工作线程里：不碰会话表内部结构，一律走 session_* 与 net_send
 * 这两个线程安全接口；跨模块传递的 client_id 用 session_client_id_copy
 * 拷贝到本地缓冲，避免裸指针出锁失效（调试记录 §十 要点 6）。
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "session.h"

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
 * DATA（S3）：登录态拦截 + 记录；**本分支任何情况下都不回包**
 * （接口规范 §3.1 未给 DATA 定义应答，S3 也不提前实现 S4 的入库）。
 */
static void proto_handle_data(int fd, const cJSON *msg)
{
    const cJSON *sensors;
    const cJSON *ts;
    const cJSON *it;
    char         cid[CLIENT_ID_MAX_LEN + 1];
    char         vals[256];
    size_t       used = 0;
    int          n    = 0;

    if (!session_is_logged(fd)) {
        log_warn("fd=%d 未登录就发 DATA，回 ERROR code=%d（连接保持）",
                 fd, ERR_CODE_NOT_LOGGED);
        proto_reply_error(fd, ERR_CODE_NOT_LOGGED, "not logged in");
        return;
    }
    session_touch(fd);                 /* 合法消息刷新活跃时间（规划 S3 任务 4） */

    if (session_client_id_copy(fd, cid, (int)sizeof(cid)) < 0) {
        cid[0] = '\0';                 /* 理论上不会发生（上面刚判过登录态） */
    }
    ts = cJSON_GetObjectItemCaseSensitive(msg, "ts");

    /* 按 [1=25.6 2=55 3=312 4=356] 的形式汇总，便于 S3-2 肉眼核对 4 个值。
     * 字段缺失/类型不对只跳过该条，不作协议错误处理（容错规则见 S4）。 */
    vals[0] = '\0';
    sensors = cJSON_GetObjectItemCaseSensitive(msg, "sensors");
    if (cJSON_IsArray(sensors)) {
        cJSON_ArrayForEach(it, sensors) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *v  = cJSON_GetObjectItemCaseSensitive(it, "v");
            int          w;

            n++;
            if (!cJSON_IsNumber(id) || !cJSON_IsNumber(v) ||
                used >= sizeof(vals) - 1) {
                continue;
            }
            w = snprintf(vals + used, sizeof(vals) - used, "%s%d=%g",
                         (used > 0) ? " " : "",
                         (int)id->valuedouble, v->valuedouble);
            if (w > 0) {
                used += (size_t)w;
            }
        }
    }

    log_info("fd=%d client_id=%s DATA：ts=%.31s sensors=%d [%s]（S3 仅记录，S4 起入库）",
             fd, cid,
             (cJSON_IsString(ts) && ts->valuestring != NULL) ? ts->valuestring : "(缺省)",
             n, vals);
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
