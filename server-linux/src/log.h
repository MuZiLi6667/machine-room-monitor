/*
 * log.h —— 日志模块对外契约（规划 §2.3）
 *
 * 输出格式：[时间戳] [级别] [标签] [tid:线程号] 正文
 * 带线程号是为了在 S2 之后区分"主线程只收发 / 工作线程处理业务"（验收 S2-4）。
 * 本模块内部持有打印锁，保证多线程下每行日志不被交错截断。
 */
#ifndef LOG_H
#define LOG_H

/* 设置日志标签（如 "server"），程序启动时调用一次；不调用则用默认值 */
void log_init(const char *tag);

void log_info(const char *fmt, ...);   /* 普通信息 */
void log_warn(const char *fmt, ...);   /* 警告 */
void log_err (const char *fmt, ...);   /* 错误 */

#endif /* LOG_H */
