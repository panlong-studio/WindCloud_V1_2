#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_

#include <pthread.h>
#include "queue.h"

/*
 * thread_pool_t:
 * 一个非常朴素的线程池结构。
 *
 * 成员说明：
 *   num
 *     线程池中的工作线程数量。
 *
 *   thread_id_arr
 *     保存每个工作线程的 pthread_t。
 *     后续主线程退出时要用它们做 pthread_join。
 *
 *   active_fds
 *     当前“正在被某个工作线程处理”的客户端 fd 列表。
 *     它的作用非常关键：
 *     当主线程收到 SIGINT 想退出时，必须知道哪些线程正阻塞在 recv 上，
 *     才能主动 shutdown 这些 fd，让线程及时醒来退出。
 *
 *   queue
 *     等待处理的客户端连接队列。
 *
 *   lock
 *     保护队列、active_fds、exitFlag 等共享资源的互斥锁。
 *
 *   cond
 *     当队列为空时，工作线程阻塞在这个条件变量上等待新任务。
 *
 *   exitFlag
 *     线程池退出标记。
 *     置为 1 后，空闲线程会被唤醒并结束循环。
 */
typedef struct thread_pool {
    int num;
    pthread_t *thread_id_arr;
    int *active_fds;
    queue_t queue;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int exitFlag;
} thread_pool_t;

/*
 * init_thread_pool:
 * 功能：
 *   初始化线程池，并创建 num 个工作线程。
 *
 * 入参：
 *   pool - 要初始化的线程池对象
 *   num  - 线程数量
 *
 * 返回值：
 *   无。
 *   当前实现如果初始化失败，会直接记录日志并退出进程。
 */
void init_thread_pool(thread_pool_t *pool, int num);

/*
 * unregister_active_fd:
 * 功能：
 *   当某个工作线程处理完客户端连接后，
 *   把该 fd 从 active_fds 中移除。
 *
 * 入参：
 *   pool - 线程池
 *   fd   - 需要移除的活动连接 fd
 */
void unregister_active_fd(thread_pool_t *pool, int fd);

/*
 * shutdown_active_fds:
 * 功能：
 *   服务端退出时，对所有正在被工作线程处理的连接执行 shutdown。
 *
 * 为什么不是 close？
 *   因为 shutdown 更适合“通知对端和本地：连接要结束了”，
 *   它可以唤醒阻塞中的 recv/send，而真正的 close 仍由工作线程在收尾阶段执行。
 */
void shutdown_active_fds(thread_pool_t *pool);

/*
 * drain_pending_fds:
 * 功能：
 *   清空任务队列里还没来得及被工作线程取走的客户端连接。
 *
 * 为什么需要它？
 *   因为服务端退出时，队列中可能还有“待处理 fd”，
 *   如果不关闭它们，就会造成 fd 泄漏。
 */
void drain_pending_fds(thread_pool_t *pool);

#endif
