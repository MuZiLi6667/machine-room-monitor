/*
 * alarm.c —— 阈值判定与 ALARM 推送（S5 实现）
 *
 * 一条 DATA 里的每个传感器值，入库后都要过一遍这里（proto.c 的 DATA 分支调用）：
 *   1. 取阈值：db_threshold_get() 读 db 模块启动时建立的 threshold_cfg 内存缓存。
 *      **没有该传感器的配置行就直接返回、不告警**——光照（sensor=4）在接口
 *      规范 §4 的初始数据里就是不插行的，天然落到这条路径上。
 *   2. 越界判定：超上限（value > max）或低于下限（value < min）。表里 min/max
 *      都允许为 NULL，NULL 的那一侧不检查（烟雾只有上限）。
 *   3. 命中 → 组 ALARM（字段与顺序严格按接口规范 §3.1：
 *      type/level/sensor/value/threshold/msg/ts）→ net_send 推送 → 写 alarm_log。
 *
 * threshold 字段口径（规划 S5 常见坑）：给**被越过的那个边界**——超上限给 max、
 *   低于下限给 min，不是另一个边界，也不是区间中点。
 *
 * ts 用**服务器当前时间**（规划 S5 任务 2）：这条告警是服务器收到数据时判出来的，
 *   直接 strftime 生成；不走 proto.c 里那个给客户端输入用的 proto_datetime_ok
 *   （那个函数的两道校验是防非法输入的，服务器自己生成的时间没必要绕道，
 *   见调试记录 §十六 要点 5）。
 *
 * 不去重（规划 S5 任务 5，文档明确要求保持简单）：每条超标的 DATA 都推送 +
 *   入库一条。连续三条超标就得到三条告警，不做"同一告警只在状态翻转时报一次"
 *   的抑制。
 *
 * 线程模型：本文件的代码全部跑在工作线程里。阈值缓存启动后只读（db.c 在
 *   pool_init 之前完成加载，pthread_create 已建立 happens-before）；net_send 与
 *   db_* 都是线程安全的；不碰会话表内部结构、不关闭任何连接
 *   （close 只归主线程，调试记录 §十三 要点 1）。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "cJSON.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "net.h"

#define ALARM_MSG_LEN   64            /* 告警文案缓冲（"温度超上限"最长 6 汉字） */
#define ALARM_TS_LEN    20            /* "YYYY-MM-DD HH:MM:SS" + '\0' */

/* 传感器名（接口规范 §1 的编号对照），只用于拼告警文案 */
static const char *alarm_sensor_name(int sensor_type)
{
    switch (sensor_type) {
    case 1:  return "温度";
    case 2:  return "湿度";
    case 3:  return "烟雾";
    case 4:  return "光照";
    default: return "传感器";
    }
}

/* 服务器当前时间，"%Y-%m-%d %H:%M:%S"（与接口规范 §3.2 的 ts 格式一致） */
static void alarm_now(char *out, size_t cap)
{
    time_t    now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    if (strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv) == 0) {
        out[0] = '\0';                /* 缓冲太小（不会发生），退化成空串 */
    }
}

/*
 * 组 ALARM 并推送。
 * 与 proto.c 的 proto_reply 同构（组 JSON → 补分帧符 → 一次 net_send），
 * 但 proto_reply 是 proto.c 内部的 static，跨模块复用要把它提升为公开接口
 * （等于改动 proto 契约）；这里保留十行同构代码换取模块边界清晰。
 * JSON 文本与 '\n' 必须在**同一次** net_send 内发出，拆开会别的线程的回包
 * 插在中间，对端收到半截 JSON（S2 任务 4、调试记录 §十）。
 */
static void alarm_send(int fd, const char *level, int sensor_type, double value,
                       double threshold, const char *msg, const char *ts)
{
    cJSON *obj;
    char   out[MAX_LINE_SIZE + 2];
    char  *text;
    size_t len;

    obj = cJSON_CreateObject();
    if (obj == NULL) {
        log_err("fd=%d 内存不足，ALARM 未能组装", fd);
        return;
    }

    /* 字段与顺序严格按接口规范 §3.1 的 ALARM 行，不得增删改 */
    cJSON_AddStringToObject(obj, "type", "ALARM");
    cJSON_AddStringToObject(obj, "level", level);
    cJSON_AddNumberToObject(obj, "sensor", (double)sensor_type);
    cJSON_AddNumberToObject(obj, "value", value);
    cJSON_AddNumberToObject(obj, "threshold", threshold);
    cJSON_AddStringToObject(obj, "msg", msg);
    cJSON_AddStringToObject(obj, "ts", ts);

    text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        log_err("fd=%d 内存不足，ALARM 未能序列化", fd);
        return;
    }

    len = strlen(text);
    if (len > (size_t)MAX_LINE_SIZE) {
        log_err("fd=%d ALARM %u 字节超过单条消息上限 %d，已丢弃",
                fd, (unsigned)len, MAX_LINE_SIZE);
        cJSON_free(text);
        return;
    }

    memcpy(out, text, len);
    out[len]     = MSG_DELIMITER;
    out[len + 1] = '\0';
    cJSON_free(text);

    if (net_send(fd, out) < 0) {
        /* 推送失败不代表告警丢了：下面照样写 alarm_log，
         * 客户端侧只是少收到这一次实时通知（连接已断，重连后能查到记录） */
        log_warn("fd=%d ALARM 推送失败，连接可能已断开（告警仍写入 alarm_log）", fd);
    }
}

void alarm_check(int fd, const char *client_id, int sensor_type, double value)
{
    db_threshold_t th;
    const char    *name;
    char           msg[ALARM_MSG_LEN];
    char           ts[ALARM_TS_LEN];
    double         threshold;
    int            over_high;

    if (client_id == NULL) {
        return;                        /* 调用方已保证非空，这里只作防御 */
    }

    /* 无配置 → 不告警（光照走的就是这条路：threshold_cfg 里没有 sensor_type=4） */
    if (db_threshold_get(sensor_type, &th) < 0) {
        return;
    }

    if (th.has_max && value > th.max_value) {
        over_high = 1;
        threshold = th.max_value;      /* ALARM 的 threshold = 被越过的那个边界 */
    } else if (th.has_min && value < th.min_value) {
        over_high = 0;
        threshold = th.min_value;
    } else {
        return;                        /* 在阈值内，不告警（S5-5） */
    }

    name = alarm_sensor_name(sensor_type);
    snprintf(msg, sizeof(msg), "%s%s", name, over_high ? "超上限" : "低于下限");
    alarm_now(ts, sizeof(ts));

    log_info("fd=%d client_id=%s 告警：sensor=%d(%s) value=%.2f %c threshold=%.2f "
             "level=%s msg=\"%s\" ts=%s",
             fd, client_id, sensor_type, name, value,
             over_high ? '>' : '<', threshold, th.level, msg, ts);

    /* 先推送（实时性）再入库（留痕），顺序按规划 S5 任务 2 */
    alarm_send(fd, th.level, sensor_type, value, threshold, msg, ts);

    if (db_insert_alarm(client_id, sensor_type, value, threshold, th.level) < 0) {
        log_err("client_id=%s 告警未能写入 alarm_log（sensor=%d level=%s）",
                client_id, sensor_type, th.level);
    }
}
