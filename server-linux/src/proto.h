/*
 * proto.h —— 协议解析与分发对外契约（规划 §2.3，S2 实现）
 *
 * 本模块是唯一接触 cJSON 的地方：把一行 JSON 解析成树，再按 type 分发。
 * 回包一律走 net_send（本模块不直接调用 send()，规划 §2.2 铁律 2）。
 *
 * 错误约定（接口规范 §3.1 ERROR 行 / §3.2）：
 *   解析失败或 type 未知都只回 ERROR，**不断开连接**。
 */
#ifndef PROTO_H
#define PROTO_H

/* 处理一条完整消息（不含结尾的 '\n'，已由 session 层切好）。
 * 在工作线程内被调用（见 net.c 的任务投递），实现必须线程安全。
 * json_line 是调用方持有的一条消息副本，本函数不接管其生命周期。 */
void proto_handle(int fd, const char *json_line);

#endif /* PROTO_H */
