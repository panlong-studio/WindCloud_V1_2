#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "epoll.h"
#include "error_check.h"
#include "log.h"
#include "server_socket.h"
#include "thread_pool.h"

/*
 * pipe_fd:
 * self-pipe 技术中使用的一对管道 fd。
 *
 * 含义：
 *   pipe_fd[0] -> 读端
 *   pipe_fd[1] -> 写端
 *
 * 为什么要用 pipe？
 * 因为在信号处理函数里不适合做复杂逻辑，
 * 但可以安全地 write 一个字节到管道中，
 * 再让 epoll 监听这个管道，从而把“信号事件”转成“普通 fd 可读事件”。
 */
static int pipe_fd[2];

/*
 * handle_sigint:
 * SIGINT（通常来自 Ctrl+C）的信号处理函数。
 *
 * 注意：
 *   这里故意只做一件极简单的事：往管道写 1 个字节。
 *   这是因为信号处理函数里可安全调用的函数非常有限。
 */
static void handle_sigint(int signo) {
    (void)signo;
    write(pipe_fd[1], "1", 1);
}

/*
 * load_value_or_default:
 * 从配置文件读取值，失败时使用默认值。
 */
static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};

    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    snprintf(value, value_sz, "%s", default_value);
}

int main(void) {
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};
    thread_pool_t pool;
    int listen_fd;
    int epfd;
    struct sigaction sa;

    /* 先读取服务端配置。 */
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("server_log", log_file, sizeof(log_file), "../log/server.log");

    /*
     * 先初始化日志，再做后续动作。
     * 这样后面如果 socket/bind/listen 失败，也能留下日志。
     */
    if (init_log(log_level, log_file) != 0) {
        fprintf(stderr, "init_log failed, fallback to stdout\n");
    }

    /*
     * 忽略 SIGPIPE。
     * 否则当服务端向一个已经断开的客户端 socket 写数据时，
     * 进程可能被 SIGPIPE 默认行为直接杀死。
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * 创建管道，用于退出通知。
     */
    if (pipe(pipe_fd) != 0) {
        perror("pipe");
        return 1;
    }

    /*
     * 安装 SIGINT 处理函数。
     */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    /* 初始化监听 socket。 */
    init_socket(&listen_fd, ip, port);

    /* 初始化线程池。 */
    init_thread_pool(&pool, 5);

    /*
     * 创建 epoll 实例。
     * epoll_create1(0) 返回一个 epoll fd，
     * 后续所有监听的 fd 都会挂到它上面。
     */
    epfd = epoll_create1(0);
    ERROR_CHECK(epfd, -1, "epoll_create1");

    /*
     * 监听两个重要 fd：
     *   1. listen_fd  -> 新客户端连接
     *   2. pipe_fd[0] -> 退出通知
     */
    add_epoll_fd(epfd, listen_fd);
    add_epoll_fd(epfd, pipe_fd[0]);

    LOG_INFO("server started ip=%s port=%s", ip, port);

    while (1) {
        struct epoll_event events[16];

        /*
         * epoll_wait 会阻塞等待事件到来。
         * 返回值 nready 表示本次有多少个 fd 就绪。
         */
        int nready = epoll_wait(epfd, events, 16, -1);

        if (nready == -1) {
            /*
             * epoll_wait 可能被信号中断。
             * 如果是 EINTR，可以继续等待，不算真正错误。
             */
            if (errno == EINTR) {
                continue;
            }
            ERROR_CHECK(nready, -1, "epoll_wait");
        }

        for (int idx = 0; idx < nready; ++idx) {
            int fd = events[idx].data.fd;

            /*
             * 如果是管道读端可读，说明收到退出通知。
             */
            if (fd == pipe_fd[0]) {
                char buf[16];

                /*
                 * 把管道中的通知字节读掉，
                 * 防止 epoll 持续看到这个 fd 一直可读。
                 */
                read(pipe_fd[0], buf, sizeof(buf));
                LOG_INFO("server received shutdown signal");

                /*
                 * 关闭监听 socket，防止再 accept 新连接。
                 */
                close(listen_fd);

                /*
                 * 设置线程池退出标记，并唤醒所有空闲线程。
                 */
                pthread_mutex_lock(&pool.lock);
                pool.exitFlag = 1;
                pthread_cond_broadcast(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                /*
                 * 再把活动连接和队列中的待处理连接都收掉，
                 * 这样忙线程和未取任务都能及时退出。
                 */
                shutdown_active_fds(&pool);
                drain_pending_fds(&pool);

                /* 等所有工作线程真正结束。 */
                for (int i = 0; i < pool.num; ++i) {
                    pthread_join(pool.thread_id_arr[i], NULL);
                }

                /* 退出前释放资源。 */
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                close(epfd);
                free(pool.active_fds);
                free(pool.thread_id_arr);
                close_log();
                return 0;
            }

            /*
             * 如果是监听 socket 可读，说明有新客户端连接到来。
             */
            if (fd == listen_fd) {
                int conn_fd = accept(listen_fd, NULL, NULL);

                if (conn_fd == -1) {
                    LOG_WARN("accept failed errno=%d", errno);
                    continue;
                }

                LOG_INFO("accepted client fd=%d", conn_fd);

                /*
                 * 把新连接放入线程池任务队列，
                 * 再唤醒一个工作线程去处理。
                 */
                pthread_mutex_lock(&pool.lock);
                enQueue(&pool.queue, conn_fd);
                pthread_cond_signal(&pool.cond);
                pthread_mutex_unlock(&pool.lock);
            }
        }
    }
}
