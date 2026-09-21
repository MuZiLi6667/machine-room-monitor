/*
 * log.c —— 带时间戳与级别的 printf 封装（S0 实现）
 *
 * 线程安全：所有输出走同一把锁 + 一次性 vfprintf，避免多工作线程下日志串行错乱。
 * 时间戳取本地时间，格式与接口规范 §3.2 的 ts 字段一致：%Y-%m-%d %H:%M:%S
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "log.h"
#include "config.h"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_tag[32] = LOG_TAG;

void log_init(const char *tag)
{
    if (tag == NULL) {
        return;
    }
    /* 只拷贝安全长度，保证不溢出 */
    strncpy(g_log_tag, tag, sizeof(g_log_tag) - 1);
    g_log_tag[sizeof(g_log_tag) - 1] = '\0';
}

/* 统一输出：拼前缀后一次性写出 */
static void log_write(const char *level, const char *fmt, va_list ap)
{
    char      ts[32];
    time_t    now = time(NULL);
    struct tm tm_buf;

    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    pthread_mutex_lock(&g_log_lock);
    fprintf(stdout, "[%s] [%-5s] [%s] [tid:%lu] ",
            ts, level, g_log_tag, (unsigned long)pthread_self());
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);              /* 重定向到文件时也能实时看到 */
    pthread_mutex_unlock(&g_log_lock);
}

void log_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("WARN", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}
