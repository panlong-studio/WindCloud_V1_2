#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client_socket.h"
#include "error_check.h"
#include "log.h"

void init_socket(int *listen_fd, char *ip, char *port) {
    /*
     * socket(AF_INET, SOCK_STREAM, 0)
     * 含义：
     *   AF_INET     -> IPv4
     *   SOCK_STREAM -> 面向连接的字节流 socket，也就是 TCP
     *   0           -> 让系统根据前两个参数自动选择默认协议
     */
    *listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(*listen_fd, -1, "socket");
    LOG_INFO("client socket created fd=%d", *listen_fd);

    /*
     * sockaddr_in 是 IPv4 专用地址结构体。
     * 使用前先清零，是个很好的习惯，可以避免未初始化字段带来的问题。
     */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;

    /*
     * inet_addr:
     *   把点分十进制字符串 IP（例如 "127.0.0.1"）
     *   转换成网络字节序的 IPv4 地址。
     */
    addr.sin_addr.s_addr = inet_addr(ip);

    /*
     * htons:
     *   host to network short
     *   把主机字节序的 16 位端口号转换成网络字节序。
     *
     * 端口在网络结构里是 16 位，所以用 htons，而不是 htonl。
     */
    addr.sin_port = htons(atoi(port));

    /*
     * connect:
     *   对客户端来说，这一步会发起 TCP 三次握手。
     *   成功后，这个 socket 就变成了“已连接 socket”。
     */
    int ret = connect(*listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    ERROR_CHECK(ret, -1, "connect");
    LOG_INFO("client connect ip=%s port=%s", ip, port);
}
