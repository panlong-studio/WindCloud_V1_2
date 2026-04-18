#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "error_check.h"
#include "log.h"
#include "thread_pool.h"
#include "worker.h"

void init_thread_pool(thread_pool_t *pool, int num) {
    /*
     * exitFlag 初始为 0，表示线程池处于工作状态。
     */
    pool->exitFlag = 0;
    pool->num = num;

    /*
     * 初始化互斥锁和条件变量。
     * lock 保护共享数据；
     * cond 用来在“队列为空”时让工作线程睡眠等待。
     */
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    /* 先把任务队列整体清零。 */
    memset(&pool->queue, 0, sizeof(queue_t));

    /*
     * 为线程 id 数组和活动 fd 数组分配空间。
     * active_fds[i] 记录第 i 个工作线程当前正在处理哪个客户端。
     */
    pool->thread_id_arr = (pthread_t *)calloc((size_t)num, sizeof(pthread_t));
    pool->active_fds = (int *)malloc((size_t)num * sizeof(int));

    if (pool->thread_id_arr == NULL) {
        LOG_ERROR("calloc thread_id_arr failed");
        exit(1);
    }
    if (pool->active_fds == NULL) {
        LOG_ERROR("malloc active_fds failed");
        exit(1);
    }

    /*
     * -1 表示“这个槽位当前没有活动客户端”。
     */
    for (int idx = 0; idx < num; ++idx) {
        pool->active_fds[idx] = -1;
    }

    /*
     * 创建 num 个工作线程。
     */
    for (int idx = 0; idx < num; ++idx) {
        int ret = pthread_create(&pool->thread_id_arr[idx], NULL, thread_func, (void *)pool);
        THREAD_ERROR_CHECK(ret, "pthread_create");
    }
}

/*
 * unregister_active_fd:
 * 一个工作线程处理完某个客户端后，把它从 active_fds 中删掉。
 */
void unregister_active_fd(thread_pool_t *pool, int fd) {
    pthread_mutex_lock(&pool->lock);

    for (int idx = 0; idx < pool->num; ++idx) {
        if (pool->active_fds[idx] == fd) {
            pool->active_fds[idx] = -1;
            break;
        }
    }

    pthread_mutex_unlock(&pool->lock);
}

/*
 * shutdown_active_fds:
 * 服务端退出时，主动 shutdown 所有正在处理中的客户端连接。
 *
 * 为什么这一步这么重要？
 * 因为工作线程很可能正阻塞在 recv_packet() 里，
 * 不主动 shutdown 的话，它可能要一直等对端发数据，导致服务端卡住退不掉。
 */
void shutdown_active_fds(thread_pool_t *pool) {
    pthread_mutex_lock(&pool->lock);

    for (int idx = 0; idx < pool->num; ++idx) {
        if (pool->active_fds[idx] != -1) {
            LOG_INFO("shutdown active client fd=%d", pool->active_fds[idx]);
            shutdown(pool->active_fds[idx], SHUT_RDWR);
        }
    }

    pthread_mutex_unlock(&pool->lock);
}

/*
 * drain_pending_fds:
 * 把任务队列里还没被工作线程取走的连接全部清掉。
 */
void drain_pending_fds(thread_pool_t *pool) {
    pthread_mutex_lock(&pool->lock);

    while (pool->queue.size > 0) {
        int fd = deQueue(&pool->queue);
        if (fd != -1) {
            LOG_INFO("close queued client fd=%d during shutdown", fd);
            close(fd);
        }
    }

    pthread_mutex_unlock(&pool->lock);
}
