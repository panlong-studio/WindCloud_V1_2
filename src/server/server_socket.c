#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "error_check.h"
#include "log.h"
#include "server_socket.h"

void init_socket(int *fd, char *ip, char *port) {
    /*
     * 创建 TCP 监听 socket。
     */
    *fd = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(*fd, -1, "socket");
    LOG_INFO("server socket created fd=%d", *fd);

    /*
     * SO_REUSEADDR:
     * 允许端口快速复用。
     *
     * 典型场景：
     *   服务器刚退出又立刻重启时，
     *   端口可能还处在 TIME_WAIT 相关状态，
     *   不开这个选项可能会报 “Address already in use”。
     */
    int opt = 1;
    setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port));
    addr.sin_addr.s_addr = inet_addr(ip);

    /* 把 socket 和指定 IP/端口绑定起来。 */
    int ret = bind(*fd, (struct sockaddr *)&addr, sizeof(addr));
    ERROR_CHECK(ret, -1, "bind");
    LOG_INFO("server bind ip=%s port=%s", ip, port);

    /*
     * listen 让这个 socket 进入监听状态。
     * backlog=10 表示内核层允许的未处理连接排队数量上限近似值。
     */
    ret = listen(*fd, 10);
    ERROR_CHECK(ret, -1, "listen");
    LOG_INFO("server listen fd=%d", *fd);
}
