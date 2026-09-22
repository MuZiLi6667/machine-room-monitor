/*
 * alarm.h —— 阈值判定与告警推送对外契约（S5 实现，签名按规划 §2.3 冻结）
 *
 * 调用者（proto.c 的 DATA 分支）在**每个传感器值入库之后**调用 alarm_check：
 * 该值越过 threshold_cfg 配置的范围时，本模块负责
 *   ① 组 ALARM 消息按接口规范 §3.1 推送给该连接；
 *   ② 往 alarm_log 写一行。
 * 没有该传感器的阈值配置（光照就是这种情况）或值在范围内 → 什么也不做。
 *
 * 阈值数据不在本模块：由 db 模块在启动时从 threshold_cfg 读进内存
 * （db_load_thresholds），此后只读（只有 db.c 能碰 MySQL，规划 §2.2 铁律 1）。
 * 接口规范 §3.1/§4 的字段零改动。
 */
#ifndef ALARM_H
#define ALARM_H

/* 阈值检查：超上限/低于下限 → 推送 ALARM + 写 alarm_log；否则直接返回。
 *   fd          ：消息来源连接（ALARM 推回该连接）
 *   client_id   ：**调用方已拷贝好的本地副本**（session_client_id_copy 的产物；
 *                 不要传 session_client_id() 的裸指针——出锁即可能失效）
 *   sensor_type ：1~4（温度/湿度/烟雾/光照）
 *   value       ：该传感器的本次读数
 * 本函数在工作线程内调用，内部只用线程安全的 net_send / db_* / log_*，
 * 不碰会话表内部结构、不关闭任何连接（close 只归主线程）。 */
void alarm_check(int fd, const char *client_id, int sensor_type, double value);

#endif /* ALARM_H */
