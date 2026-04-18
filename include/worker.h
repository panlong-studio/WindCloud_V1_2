#ifndef _WORKER_H_
#define _WORKER_H_

/*
 * thread_func:
 * 功能：
 *   线程池工作线程的线程函数。
 *
 * 入参：
 *   arg - 一般是 thread_pool_t *，用来访问任务队列、锁、条件变量等共享资源
 *
 * 返回值：
 *   线程退出时返回 NULL。
 */
void *thread_func(void *arg);

#endif
