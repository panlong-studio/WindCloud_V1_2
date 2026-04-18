#ifndef _SOCKET_H_
#define _SOCKET_H_

/*
 * init_socket:
 * 功能：
 *   客户端创建 socket，并主动 connect 到服务端。
 *
 * 入参：
 *   fd   - 输出参数，返回创建成功后的 socket fd
 *   ip   - 服务端 IP 字符串
 *   port - 服务端端口字符串
 */
void init_socket(int *fd, char *ip, char *port);

#endif
