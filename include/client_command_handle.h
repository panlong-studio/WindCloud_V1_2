#ifndef CLIENT_COMMAND_HANDLE_H
#define CLIENT_COMMAND_HANDLE_H

/*
 * process_command:
 * 功能：
 *   客户端处理一行用户输入命令的总入口。
 *
 * 它会：
 *   1. 解析命令文本
 *   2. 构造对应 TLV 请求
 *   3. 根据命令类型决定走普通命令流程、上传流程还是下载流程
 *
 * 入参：
 *   sock_fd - 已连接到服务端的 socket fd
 *   input   - 用户输入的一整行命令
 *
 * 返回值：
 *   0  表示处理成功
 *  -1 表示处理失败
 */
int process_command(int sock_fd, const char *input);

#endif
