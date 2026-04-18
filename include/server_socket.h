#ifndef SERVER_SOCKET_H_
#define SERVER_SOCKET_H_

/*
 * init_socket:
 * 功能：
 *   服务端创建监听 socket，完成 bind 和 listen。
 *
 * 入参：
 *   fd   - 输出参数，返回监听 socket fd
 *   ip   - 监听的 IP 地址
 *   port - 监听的端口号
 */
void init_socket(int *fd, char *ip, char *port);

#endif
