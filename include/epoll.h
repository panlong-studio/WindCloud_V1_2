#ifndef _EPOLL_H_
#define _EPOLL_H_

/*
 * add_epoll_fd:
 * 功能：
 *   把 fd 加入 epoll 监听集合。
 *
 * 入参：
 *   epfd - epoll 实例 fd
 *   fd   - 需要被监听的目标 fd
 */
void add_epoll_fd(int epfd, int fd);

/*
 * del_epoll_fd:
 * 功能：
 *   从 epoll 监听集合中移除 fd。
 */
void del_epoll_fd(int epfd, int fd);

#endif
