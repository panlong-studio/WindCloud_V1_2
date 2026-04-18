#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/*
 * PATH_MAX:
 * 某些平台在头文件中可能没有直接给出 PATH_MAX，
 * 所以这里做一个兜底定义，避免编译期出现“路径长度宏未定义”的问题。
 *
 * 4096 这个值并不是“所有系统都固定如此”，
 * 而是 Linux 上一个常见且足够大的路径长度上限。
 */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * TLV_MAGIC:
 * 协议魔数（magic number）。
 *
 * 它的作用是让接收方在读到一个包头后，先快速确认：
 * “我收到的这一段字节，确实是我们自己定义的协议，而不是乱流、脏数据、旧协议数据。”
 *
 * 0x544C5631 对应 ASCII 可以理解成 "TLV1"。
 */
#define TLV_MAGIC 0x544C5631U

/*
 * TLV_VERSION:
 * 协议版本号。
 *
 * 当前项目虽然只有一个版本，但提前留这个字段非常重要：
 * 将来协议升级时，新老客户端/服务端可以通过 version 判断是否兼容。
 */
#define TLV_VERSION 1U

/*
 * MAX_PACKET_PAYLOAD:
 * 单个 TLV 包允许承载的最大 payload 长度。
 *
 * 注意：
 * 这里限制的是“一包”的大小，不是“整个文件”的大小。
 * 文件传输时，一个大文件会被拆成很多个小包来发送。
 */
#define MAX_PACKET_PAYLOAD (64U * 1024U)

/*
 * MAX_COMMAND_INPUT:
 * 客户端单次输入命令允许的最大长度。
 * 这个宏主要用于限制命令文本，防止无界输入导致缓冲区风险。
 */
#define MAX_COMMAND_INPUT 512U

/*
 * MAX_COMMAND_ARG:
 * 命令参数允许的最大长度。
 * 这里直接沿用 PATH_MAX，表示命令参数一般就是目录名或文件路径。
 */
#define MAX_COMMAND_ARG PATH_MAX

/*
 * MAX_TEXT_PAYLOAD:
 * 文本响应 payload 的最大长度。
 * 例如 pwd/ls/错误消息这类文本，都可以放到这个范围内。
 */
#define MAX_TEXT_PAYLOAD 4096U

/*
 * FILE_BLOCK_SIZE:
 * 文件传输时，每个 FILE_DATA 包里放多少字节。
 *
 * 这里取 4096，和常见页大小一致，便于理解，也便于和 mmap / 文件页缓存联系起来。
 */
#define FILE_BLOCK_SIZE 4096U

/*
 * cmd_type_t:
 * 所有命令和协议数据包类型的枚举。
 *
 * 为什么要用枚举，而不是字符串比较？
 * 1. 效率更高：switch-case 对枚举的分发比反复 strcmp 更直观。
 * 2. 更安全：避免字符串拼写不一致造成的逻辑错误。
 * 3. 更适合协议：网络上传输的“命令类型”更适合用固定整数表示。
 */
typedef enum {
    CMD_INVALID = 0, /* 非法命令或无法识别的命令 */
    CMD_PWD,         /* pwd：查看当前虚拟路径 */
    CMD_CD,          /* cd：切换目录 */
    CMD_LS,          /* ls：列出当前目录内容 */
    CMD_RM,          /* rm/remove：删除文件 */
    CMD_MKDIR,       /* mkdir：创建目录 */
    CMD_PUTS_REQ,    /* puts 请求：客户端准备上传文件 */
    CMD_PUTS_RESP,   /* puts 响应：服务端返回断点偏移 */
    CMD_GETS_REQ,    /* gets 请求：客户端准备下载文件 */
    CMD_GETS_RESP,   /* gets 响应：服务端返回文件元信息 */
    CMD_RESUME_POS,  /* 断点续传位置包 */
    CMD_FILE_DATA,   /* 文件数据包 */
    CMD_FILE_END,    /* 文件结束包 */
    CMD_ACK,         /* 通用成功响应 */
    CMD_ERROR        /* 通用错误响应 */
} cmd_type_t;

/*
 * status_code_t:
 * 协议层状态码。
 *
 * cmd_type 表达“这是什么包”；
 * status 表达“这次操作结果怎么样”。
 *
 * 两者不是一个维度，所以需要分开。
 */
typedef enum {
    STATUS_OK = 0,            /* 成功 */
    STATUS_BAD_REQUEST = 1,   /* 请求格式错误、参数错误 */
    STATUS_NOT_FOUND = 2,     /* 文件或目录不存在 */
    STATUS_IO_ERROR = 3,      /* 本地 IO 错误，例如 open/mkdir/remove 失败 */
    STATUS_PROTOCOL_ERROR = 4 /* 协议流程或包格式错误 */
} status_code_t;

/*
 * session_t:
 * 服务端“单个客户端连接”的上下文。
 *
 * 为什么要引入 session_t，而不是一直传裸 fd？
 * 因为 fd 只能代表“这是哪个 socket”，却无法代表：
 * - 当前用户所在路径
 * - 当前连接是否应该关闭
 * - 将来第三期接数据库后，这是谁的会话
 *
 * 所以把和连接相关的状态打包进一个结构体，是更合理的扩展方式。
 */
typedef struct {
    int peer_fd;                  /* 当前客户端连接对应的 socket fd */
    char current_path[PATH_MAX];  /* 当前连接所处的“虚拟路径”，例如 / 或 /demo */
    int user_id;                  /* 预留给第三期数据库用户体系，当前阶段默认 -1 */
    int should_close;             /* 是否应该关闭连接，1 表示退出请求处理循环 */
} session_t;

/*
 * tlv_header_t:
 * TLV 协议头。
 *
 * 这里的 T/L/V 可以理解为：
 * - Type   -> cmd_type
 * - Length -> data_len
 * - Value  -> payload
 *
 * 另外再加上 magic / version / status，用于协议校验和状态表达。
 *
 * 所有字段都用 uint32_t，是为了保证字段宽度固定。
 * 这在网络协议里非常重要，因为 int 在不同平台上的位宽可能不同。
 */
typedef struct {
    uint32_t magic;    /* 魔数：确认这是我们协议的包 */
    uint32_t version;  /* 版本号：方便未来协议升级 */
    uint32_t cmd_type; /* 命令类型：例如 CMD_PWD / CMD_FILE_DATA */
    uint32_t status;   /* 状态码：例如 STATUS_OK / STATUS_IO_ERROR */
    uint32_t data_len; /* payload 的字节数 */
} tlv_header_t;

/*
 * packet_t:
 * “已经解包后的包对象”。
 *
 * header 里放元数据；
 * payload 指向动态分配出来的数据区。
 *
 * 注意：
 * recv_packet() 会为 payload 申请内存，
 * 所以用完以后必须调用 free_packet() 释放。
 */
typedef struct {
    tlv_header_t header; /* 已经转换到主机字节序的头部 */
    void *payload;       /* 动态申请的负载内存；如果 data_len==0，则可能为 NULL */
} packet_t;

/*
 * command_request_t:
 * 客户端把一行命令文本解析后的结果。
 *
 * 例如：
 *   "mkdir demo"
 * 会被解析成：
 *   type = CMD_MKDIR
 *   arg  = "demo"
 */
typedef struct {
    cmd_type_t type;             /* 解析出的命令类型 */
    char arg[MAX_COMMAND_ARG];   /* 命令参数，例如目录名或文件名 */
} command_request_t;

/*
 * path_payload_t:
 * 一个简单路径负载结构。
 *
 * 当前版本中没有大规模单独使用它，
 * 但保留这个结构对初学者理解“路径也是一种协议 payload”很有帮助。
 */
typedef struct {
    char path[PATH_MAX]; /* 路径字符串 */
} path_payload_t;

/*
 * text_payload_t:
 * 一个简单文本负载结构。
 * 常用于 ACK / ERROR 中承载普通文本消息。
 */
typedef struct {
    char text[MAX_TEXT_PAYLOAD]; /* 文本内容 */
} text_payload_t;

/*
 * file_info_payload_t:
 * 文件元信息包。
 *
 * 用于 puts/gets 之类需要先同步文件名和文件大小的场景。
 */
typedef struct {
    char file_name[PATH_MAX]; /* 文件名（当前阶段直接按文件名识别） */
    uint64_t file_size;       /* 文件总大小；注意发网络前要做字节序转换 */
} file_info_payload_t;

/*
 * resume_payload_t:
 * 断点续传偏移量包。
 *
 * 上传时：
 *   服务端告诉客户端“我已经有多少了”
 *
 * 下载时：
 *   客户端告诉服务端“我本地已经下了多少了”
 */
typedef struct {
    uint64_t offset; /* 断点续传偏移量 */
} resume_payload_t;

/*
 * file_chunk_payload_t:
 * 文件块结构体。
 *
 * 当前实现里真正的 FILE_DATA 发送没有直接构造这个结构体整体发出去，
 * 但这个定义有教学意义：
 * 它展示了“文件块 = 长度 + 数据区”的基本思路。
 */
typedef struct {
    uint32_t data_len;                 /* 当前块里有效数据的字节数 */
    unsigned char data[FILE_BLOCK_SIZE]; /* 实际数据字节 */
} file_chunk_payload_t;

/*
 * parse_command_type:
 * 功能：
 *   把字符串命令映射成枚举命令类型。
 *
 * 入参：
 *   cmd - 命令字符串，例如 "pwd"、"puts"
 *
 * 返回值：
 *   成功返回对应的 cmd_type_t；
 *   失败返回 CMD_INVALID。
 */
cmd_type_t parse_command_type(const char *cmd);

/*
 * command_type_to_string:
 * 功能：
 *   把枚举命令类型转回字符串，便于打印日志。
 *
 * 入参：
 *   type - 命令类型枚举值
 *
 * 返回值：
 *   返回一个只读字符串常量。
 */
const char *command_type_to_string(cmd_type_t type);

/*
 * parse_command_request:
 * 功能：
 *   把客户端输入的一整行命令文本解析成 command_request_t。
 *
 * 入参：
 *   input - 例如 "mkdir demo"
 *   req   - 输出参数，保存解析结果
 *
 * 返回值：
 *   0  表示成功；
 *  -1 表示失败。
 */
int parse_command_request(const char *input, command_request_t *req);

/*
 * build_command_request:
 * 功能：
 *   当前阶段它和 parse_command_request 做的是同一件事，
 *   但单独保留这个接口，有利于未来把“解析输入”和“构造协议请求”分层。
 *
 * 入参/返回值与 parse_command_request 相同。
 */
int build_command_request(const char *input, command_request_t *req);

/*
 * host_to_net_u64 / net_to_host_u64:
 * 功能：
 *   把 64 位整数在主机字节序与网络字节序之间转换。
 *
 * 为什么需要自己写？
 *   标准库常见的是 htonl/ntohl 这种 32 位函数；
 *   对 64 位字段，课程型项目里通常自己拆成两个 32 位去转换。
 */
uint64_t host_to_net_u64(uint64_t value);
uint64_t net_to_host_u64(uint64_t value);

/*
 * send_n:
 * 功能：
 *   保证把 len 个字节全部写到 fd。
 *
 * 为什么不能直接调用一次 write/send 就结束？
 *   因为一次系统调用可能只写出部分数据，特别是在网络场景里更常见。
 *
 * 入参：
 *   fd  - 目标文件描述符，通常是 socket
 *   buf - 要发送的数据起始地址
 *   len - 要发送的总字节数
 *
 * 返回值：
 *   0  表示全部发送完成；
 *  -1 表示发送失败或对端关闭。
 */
int send_n(int fd, const void *buf, size_t len);

/*
 * recv_n:
 * 功能：
 *   保证从 fd 中读取 len 个字节。
 *
 * 为什么需要它？
 *   因为 TCP 是字节流，单次 recv 可能只能收到一部分。
 *
 * 入参：
 *   fd  - 数据来源 fd，通常是 socket
 *   buf - 接收缓冲区
 *   len - 期望读取的字节数
 *
 * 返回值：
 *   0  表示刚好读满 len；
 *  -1 表示连接关闭或发生错误。
 */
int recv_n(int fd, void *buf, size_t len);

/*
 * send_packet:
 * 功能：
 *   发送一个完整的 TLV 包。
 *
 * 内部会做两件事：
 *   1. 先发送协议头
 *   2. 再发送 payload
 *
 * 入参：
 *   fd          - 目标 socket
 *   type        - 包类型
 *   status      - 状态码
 *   payload     - 负载起始地址，可以为 NULL
 *   payload_len - 负载长度
 *
 * 返回值：
 *   0  表示发送成功；
 *  -1 表示失败。
 */
int send_packet(int fd, cmd_type_t type, status_code_t status, const void *payload, uint32_t payload_len);

/*
 * recv_packet:
 * 功能：
 *   从 socket 中完整接收一个 TLV 包，并解包到 packet 对象中。
 *
 * 入参：
 *   fd     - 来源 socket
 *   packet - 输出参数，保存解包后的结果
 *
 * 返回值：
 *   0  表示成功；
 *  -1 表示失败。
 *
 * 注意：
 *   成功后如果 payload 非空，需要调用 free_packet() 释放。
 */
int recv_packet(int fd, packet_t *packet);

/*
 * free_packet:
 * 功能：
 *   释放 recv_packet() 为 payload 分配的内存，并清空头部。
 *
 * 入参：
 *   packet - 需要清理的包对象
 *
 * 返回值：
 *   无。
 */
void free_packet(packet_t *packet);

#endif
