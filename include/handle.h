#ifndef _HANDLE_H_
#define _HANDLE_H_

#include "protocol.h"

/*
 * handle_request:
 * 功能：
 *   服务端处理单个客户端连接的核心入口函数。
 *
 * 它会在一个循环中不断：
 *   1. 从 socket 读取 TLV 包
 *   2. 识别包类型
 *   3. 路由到具体命令处理函数
 *   4. 直到客户端断开或 session->should_close 变为 1
 *
 * 入参：
 *   session - 当前客户端连接对应的会话上下文
 *
 * 返回值：
 *   0  表示正常结束处理循环
 *  -1 表示入参非法等严重错误
 */
int handle_request(session_t *session);

#endif /* _HANDLE_H_ */
