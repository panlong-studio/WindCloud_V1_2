#include <sys/epoll.h>

#include "epoll.h"
#include "error_check.h"

/*
 * add_epoll_fd:
 * 把 fd 加入 epoll 监听集合。
 *
 * 这里当前只关心 EPOLLIN，
 * 即“这个 fd 变得可读了”。
 *
 * 对 listen_fd 来说，可读意味着有新连接可 accept；
 * 对 pipe_fd[0] 来说，可读意味着收到了退出通知。
 */
void add_epoll_fd(int epfd, int fd) {
    struct epoll_event evt;
    evt.data.fd = fd;
    evt.events = EPOLLIN;

    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
    ERROR_CHECK(ret, -1, "epoll_ctl add");
}

/*
 * del_epoll_fd:
 * 把 fd 从 epoll 集合中移除。
 */
void del_epoll_fd(int epfd, int fd) {
    struct epoll_event evt;
    evt.data.fd = fd;
    evt.events = EPOLLIN;

    int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &evt);
    ERROR_CHECK(ret, -1, "epoll_ctl del");
}
