#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "handle.h"
#include "log.h"
#include "queue.h"
#include "thread_pool.h"
#include "worker.h"

void *thread_func(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->lock);

        /*
         * 如果队列为空，并且线程池还没要求退出，
         * 当前线程就在条件变量上睡眠。
         *
         * 为什么这里要用 while 而不是 if？
         * 因为条件变量可能存在“伪唤醒”，
         * 线程被唤醒后仍然应该重新检查条件是否满足。
         */
        while (pool->queue.size == 0 && !pool->exitFlag) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        /*
         * 一旦 exitFlag 被设置，线程就结束主循环。
         * 注意这里必须先解锁再 break。
         */
        if (pool->exitFlag) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /*
         * 从队列中拿出一个客户端连接 fd。
         */
        int client_fd = deQueue(&pool->queue);

        /*
         * 把这个 fd 登记到 active_fds 中。
         * 这样主线程如果要退出，就知道有哪些线程还在忙。
         */
        for (int idx = 0; idx < pool->num; ++idx) {
            if (pool->active_fds[idx] == -1) {
                pool->active_fds[idx] = client_fd;
                break;
            }
        }

        pthread_mutex_unlock(&pool->lock);

        /*
         * 为这个连接初始化会话上下文。
         * 当前阶段 user_id 只是预留字段，所以设为 -1。
         */
        session_t session;
        memset(&session, 0, sizeof(session));
        session.peer_fd = client_fd;
        session.user_id = -1;
        snprintf(session.current_path, sizeof(session.current_path), "/");

        LOG_INFO("worker thread=%lu handling client fd=%d", (unsigned long)pthread_self(), client_fd);

        /* 真正的业务处理都在 handle_request 里。 */
        handle_request(&session);

        /* 处理完成后，把 fd 从活动列表移除。 */
        unregister_active_fd(pool, client_fd);

        /* 再关闭真实 socket。 */
        close(client_fd);
        LOG_INFO("worker thread=%lu finished client fd=%d", (unsigned long)pthread_self(), client_fd);
    }

    return NULL;
}
