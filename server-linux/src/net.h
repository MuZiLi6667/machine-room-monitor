/*
 * net.h —— select 网络层对外契约（规划 §2.3，S1 实现）
 *
 * 职责：监听端口、select 主循环、连接收发、fd 的创建与回收。
 *
 * 铁律（规划 §2.2）：只有本模块可以调用 socket()/accept()/recv()/send()/close()；
 * 其他模块要回消息一律走 net_send(fd, buf)。
 *
 * 线程模型：主线程只负责 accept/recv/按 '\n' 取行，取到的每条消息深拷贝后
 * 投递线程池（S2 起），业务解析在工作线程内完成；net_send 内部持发送锁，
 * 因此**任何线程**都可以安全地用它回包。
 */
#ifndef NET_H
#define NET_H

/* 创建监听 socket 并 bind/listen。成功返回 0；失败返回 -1（已打印错误日志）。 */
int net_init(int port);

/* select 主循环：阻塞运行，直到 net_stop() 被调用或发生致命错误。
 * 返回前会关闭监听 fd 与全部会话连接。返回 0 表示正常退出。 */
int net_loop(void);

/* 单连接发送：把 buf 原样写完（不自动补 '\n'，分帧符由调用方按接口规范 §3 补）。
 * 返回实际写出的字节数；失败（连接已被对端关闭等）返回 -1。 */
int net_send(int fd, const char *buf);

/* 关闭连接并从会话表移除（幂等：重复调用或无效 fd 安全忽略）。 */
void net_close(int fd);

/* 请求主循环退出。仅供 main.c 的信号处理函数调用：
 * 内部只置一个 volatile sig_atomic_t 标志，满足异步信号安全要求。 */
void net_stop(void);

#endif /* NET_H */
