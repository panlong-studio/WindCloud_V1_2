# 第二阶段 TLV 重构设计

## 目标

在不超前实现第三期到第五期功能的前提下，修复当前第一期/第二期核心功能中的协议错位、粘包/半包、断点续传失效、退出卡死、日志不可用等问题。

本次设计明确：

- 保留 `epoll + 线程池` 整体架构。
- 不实现数据库、JWT、真正多用户。
- 不实现伪多用户隔离。
- 引入 `session_t` 仅作为连接上下文，为第三期预留接口。

## 核心方案

采用统一 TLV 协议重构客户端和服务端通信：

- 所有命令请求、命令响应、文件元信息、断点偏移、文件数据块都带统一协议头。
- 命令分发从 `if/else if(strcmp())` 改为 `enum + switch-case`。
- 所有收发统一走 `send_n()/recv_n()`，禁止裸用单次 `send()/recv()` 假设一次完成。
- 文件传输不再把 `off_t` 和文件流直接裸发到 socket，而是用明确的包类型拆分。

## 协议设计

### 协议头

定义固定长度头部 `tlv_header_t`，字段至少包括：

- `magic`
- `version`
- `cmd_type`
- `status`
- `data_len`

使用固定宽度整数并进行网络字节序转换，避免直接传裸 `int`、裸 `off_t` 带来的位宽和端序问题。

### 命令类型

定义枚举 `cmd_type_t`，覆盖：

- `CMD_PWD`
- `CMD_CD`
- `CMD_LS`
- `CMD_RM`
- `CMD_MKDIR`
- `CMD_PUTS_REQ`
- `CMD_PUTS_RESP`
- `CMD_GETS_REQ`
- `CMD_GETS_RESP`
- `CMD_RESUME_POS`
- `CMD_FILE_DATA`
- `CMD_FILE_END`
- `CMD_ACK`
- `CMD_ERROR`

### 报文流程

普通命令：

- 客户端发命令包
- 服务端回 ACK 或文本响应包

上传：

- 客户端发送 `PUTS_REQ`
- 服务端返回 `PUTS_RESP`，携带服务器本地已有大小
- 客户端从断点位置开始发送多个 `FILE_DATA`
- 客户端发送 `FILE_END`
- 服务端返回最终 `ACK`

下载：

- 客户端发送 `GETS_REQ`
- 服务端返回 `GETS_RESP`，携带服务器文件总大小
- 客户端发送 `RESUME_POS`
- 服务端发送多个 `FILE_DATA`
- 服务端发送 `FILE_END`

## 服务端结构

保留现有模块分工，但调整职责：

- `server.c`
  - 初始化日志
  - 初始化 socket、线程池、epoll
  - 负责退出事件与监听连接
- `worker.c`
  - 从队列中取出 `peer_fd`
  - 初始化 `session_t`
  - 调用 `handle_request(session_t *)`
- `handle.c`
  - 收包/解包
  - 字符串命令转枚举
  - `switch-case` 路由
  - 具体业务处理

## `session_t` 设计

定义连接上下文：

- `int peer_fd`
- `char current_path[PATH_MAX]`
- `int user_id`
- `int should_close`

其中：

- `current_path` 用于维护当前虚拟路径
- `user_id` 第二阶段仅占位，默认 `-1`
- `should_close` 用于退出或协议错误时收口连接

## 文件传输设计

上传接收端继续允许使用 `mmap`，但必须保证：

- 先 `ftruncate()` 扩展到目标大小
- 再 `mmap()`
- 中断时按真实接收大小二次 `ftruncate()`

下载发送端优先继续使用 `sendfile()`，但只在包边界明确后发送：

- 文件大小和断点偏移通过 TLV 控制包同步
- 真正文件数据拆成文件块包或在受控分支中发送
- 对端关闭时正确处理 `EPIPE`/`SIGPIPE`

## 日志设计

修复并接入现有 `log.c`：

- 在 `client.c`、`server.c` 启动阶段调用 `init_log()`
- `log.c` 默认兜底到 `stdout`
- 时间转换改为线程安全版本
- 在关键模块补日志：
  - 配置加载
  - socket 建连/监听
  - epoll 事件
  - 线程池取任务/退出
  - 命令解析与路由
  - 文件上传下载阶段

## 测试设计

引入轻量测试，不搭大型框架：

- 协议函数测试
  - 命令字符串映射
  - TLV 编解码
- I/O 测试
  - `socketpair()` 上验证 `send_n()/recv_n()`
- 集成烟测
  - 构建客户端和服务端
  - 执行 `pwd/ls/mkdir/cd/puts/gets/rm`
  - 校验日志与文件结果

## 非目标

本次不做以下内容：

- 数据库表设计与接入
- 登录注册与鉴权
- 用户隔离目录森林
- 多点下载
- 更高级线程池调度策略
