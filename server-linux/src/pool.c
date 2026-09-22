/*
 * pool.c —— 固定线程数 + 有界任务队列的线程池（S2 实现）
 *
 * 队列用环形数组实现：head 指向待取任务，tail 指向下一个空位，count 为当前
 * 任务数。生产者（主线程）在 count == 容量 时睡在 not_full 上，消费者（工作
 * 线程）在 count == 0 时睡在 not_empty 上。
 *
 * 关键约定：task.fn(task.arg) 在**解锁之后**执行。若持锁执行，任务体里的任何
 * 阻塞调用（net_send、后续的 MySQL 查询）都会卡住整池；而且任务体若再调
 * pool_submit 会自死锁。
 */
#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "pool.h"

typedef struct {
    void (*fn)(void *);
    void  *arg;
} pool_task_t;

static pool_task_t     g_queue[POOL_QUEUE_SIZE];
static int             g_head  = 0;                 /* 取任务位置 */
static int             g_tail  = 0;                 /* 放任务位置 */
static int             g_count = 0;                 /* 当前任务数 */
static pthread_t       g_threads[POOL_THREADS];
static int             g_nthreads = 0;              /* 实际创建成功的线程数 */
static int             g_inited   = 0;

static pthread_mutex_t g_lock      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_not_empty = PTHREAD_COND_INITIALIZER;  /* 队列非空：唤醒消费者 */
static pthread_cond_t  g_not_full  = PTHREAD_COND_INITIALIZER;  /* 队列非满：唤醒生产者 */

/* 工作线程主函数：取任务 → 解锁执行 → 回到取任务 */
static void *pool_worker(void *arg)
{
    (void)arg;

    for (;;) {
        pool_task_t task;

        pthread_mutex_lock(&g_lock);
        /* 用 while 而非 if：被唤醒后要重新判断条件（可能被其他线程抢先取走） */
        while (g_count == 0) {
            pthread_cond_wait(&g_not_empty, &g_lock);
        }

        task    = g_queue[g_head];
        g_head  = (g_head + 1) % POOL_QUEUE_SIZE;
        g_count--;

        pthread_cond_signal(&g_not_full);   /* 腾出一个空位，通知生产者 */
        pthread_mutex_unlock(&g_lock);

        task.fn(task.arg);                  /* 锁外执行，见文件头说明 */
    }
    return NULL;                            /* 不会走到：工作线程随进程退出 */
}

void pool_init(int nthreads)
{
    int i;

    if (nthreads <= 0) {
        nthreads = 1;
    }
    if (nthreads > POOL_THREADS) {
        log_warn("请求的线程数 %d 超过上限 %d，按上限创建", nthreads, POOL_THREADS);
        nthreads = POOL_THREADS;
    }
    if (g_inited) {
        log_warn("线程池已初始化过，忽略本次 pool_init(%d)", nthreads);
        return;
    }

    for (i = 0; i < nthreads; i++) {
        int rc = pthread_create(&g_threads[i], NULL, pool_worker, NULL);

        if (rc != 0) {
            log_err("工作线程 %d 创建失败：%s", i, strerror(rc));
            break;
        }
    }
    g_nthreads = i;

    /* 先置 inited 再打日志：调用方（main）在本函数返回后才会投递任务，
     * 生产者在同一条线程上，故不存在竞态 */
    g_inited = 1;
    log_info("线程池已就绪：%d 个工作线程，任务队列容量 %d",
             g_nthreads, POOL_QUEUE_SIZE);
}

void pool_submit(void (*fn)(void *), void *arg)
{
    if (fn == NULL) {
        log_err("pool_submit 收到空任务函数，已忽略");
        return;
    }
    if (!g_inited) {
        log_err("线程池尚未初始化，本次任务已丢弃（arg 由调用方自行处理）");
        return;
    }

    pthread_mutex_lock(&g_lock);
    while (g_count == POOL_QUEUE_SIZE) {
        pthread_cond_wait(&g_not_full, &g_lock);   /* 队列满：阻塞直到有空位 */
    }

    g_queue[g_tail].fn  = fn;
    g_queue[g_tail].arg = arg;
    g_tail  = (g_tail + 1) % POOL_QUEUE_SIZE;
    g_count++;

    pthread_cond_signal(&g_not_empty);             /* 通知一个消费者取任务 */
    pthread_mutex_unlock(&g_lock);
}
