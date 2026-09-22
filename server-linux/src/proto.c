/*
 * proto.c —— cJSON 解析与按 type 分发（S2 实现）
 *
 * 处理流程（接口规范 §3.2 容错约定）：
 *   cJSON_Parse 失败            → ERROR code=1 "malformed json"
 *   type 缺失 / 非字符串 / 未知 → ERROR code=2 "unknown type"
 *   LOGIN / DATA / HEARTBEAT    → 已知类型，S2 只记日志占位（S3/S4 填业务）
 * 前两类都只回 ERROR 并保持连接，不主动断开（S2-1~S2-3）。
 *
 * 本文件跑在工作线程里：不碰会话表内部结构，回包统一走线程安全的 net_send。
 */
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "proto.h"

/* 错误码（接口规范 §3.1 ERROR 行：1=malformed json，2=unknown type） */
#define ERR_CODE_MALFORMED 1
#define ERR_CODE_UNKNOWN   2

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

/* ---- 已知 type 的处理分支 ----
 * S2 只证明"解析成功且能按 type 路由"，业务实现留给后续步骤：
 *   LOGIN     → S3（session_login + 回 LOGIN_ACK）
 *   DATA      → S4（拆包入库）
 *   HEARTBEAT → S3（回 HEARTBEAT_ACK）
 * 占位阶段刻意不回任何应答，避免提前假装成已完成，也不改动接口规范。 */
static void proto_handle_login(int fd, const cJSON *msg)
{
    (void)msg;
    log_info("fd=%d 收到 LOGIN（S2 占位：解析与路由成功，业务实现见 S3）", fd);
}

static void proto_handle_data(int fd, const cJSON *msg)
{
    (void)msg;
    log_info("fd=%d 收到 DATA（S2 占位：解析与路由成功，业务实现见 S4）", fd);
}

static void proto_handle_heartbeat(int fd, const cJSON *msg)
{
    (void)msg;
    log_info("fd=%d 收到 HEARTBEAT（S2 占位：解析与路由成功，业务实现见 S3）", fd);
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
