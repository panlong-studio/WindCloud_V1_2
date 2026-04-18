#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * fill_request:
 * 功能：
 *   把命令类型和参数安全地写入 command_request_t。
 *
 * 为什么单独抽一个函数？
 *   因为 parse_command_request() / build_command_request() 最终都要构造同一种结果，
 *   抽出来可以避免重复清零、重复拷贝。
 *
 * 返回值：
 *   0  成功
 *  -1  req 为空
 */
static int fill_request(cmd_type_t type, const char *arg, command_request_t *req) {
    if (req == NULL) {
        return -1;
    }

    /*
     * 先把整个结构体清零，
     * 这样即使调用者没有手动初始化，结构体里也不会残留脏数据。
     */
    memset(req, 0, sizeof(*req));

    /* 保存命令类型。 */
    req->type = type;

    /*
     * 参数允许为空。
     * 例如 pwd / ls 这种命令，本来就不需要 arg。
     */
    if (arg != NULL) {
        /*
         * 用 snprintf 而不是 strcpy，
         * 是为了保证最多只写满目标缓冲区大小，避免越界。
         */
        snprintf(req->arg, sizeof(req->arg), "%s", arg);
    }
    return 0;
}

/*
 * parse_command_type:
 * 把字符串命令映射成枚举命令。
 *
 * 这里使用一串 if 判断，
 * 虽然不是最“高级”的写法，但对初学者来说足够直观：
 * 看到什么字符串，就映射成什么枚举。
 */
cmd_type_t parse_command_type(const char *cmd) {
    /*
     * 空指针或者空字符串都直接视为非法命令。
     */
    if (cmd == NULL || *cmd == '\0') {
        return CMD_INVALID;
    }

    if (strcmp(cmd, "pwd") == 0) {
        return CMD_PWD;
    }
    if (strcmp(cmd, "cd") == 0) {
        return CMD_CD;
    }
    if (strcmp(cmd, "ls") == 0) {
        return CMD_LS;
    }
    /*
     * remove 和 rm 都映射到同一个命令类型，
     * 这样协议层只需要处理一种删除逻辑。
     */
    if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "remove") == 0) {
        return CMD_RM;
    }
    if (strcmp(cmd, "mkdir") == 0) {
        return CMD_MKDIR;
    }
    if (strcmp(cmd, "puts") == 0) {
        return CMD_PUTS_REQ;
    }
    if (strcmp(cmd, "gets") == 0) {
        return CMD_GETS_REQ;
    }

    /* 走到这里说明命令不在已支持列表中。 */
    return CMD_INVALID;
}

/*
 * command_type_to_string:
 * 主要给日志打印使用，把枚举值变成人类可读文本。
 */
const char *command_type_to_string(cmd_type_t type) {
    switch (type) {
    case CMD_PWD:
        return "pwd";
    case CMD_CD:
        return "cd";
    case CMD_LS:
        return "ls";
    case CMD_RM:
        return "rm";
    case CMD_MKDIR:
        return "mkdir";
    case CMD_PUTS_REQ:
        return "puts";
    case CMD_GETS_REQ:
        return "gets";
    case CMD_PUTS_RESP:
        return "puts_resp";
    case CMD_GETS_RESP:
        return "gets_resp";
    case CMD_RESUME_POS:
        return "resume_pos";
    case CMD_FILE_DATA:
        return "file_data";
    case CMD_FILE_END:
        return "file_end";
    case CMD_ACK:
        return "ack";
    case CMD_ERROR:
        return "error";
    case CMD_INVALID:
    default:
        return "invalid";
    }
}

/*
 * parse_command_request:
 * 把一整行命令文本拆成：
 *   1. 命令类型
 *   2. 参数字符串
 *
 * 例如：
 *   "mkdir demo"
 * 会拆成：
 *   cmd = "mkdir"
 *   arg = "demo"
 */
int parse_command_request(const char *input, command_request_t *req) {
    char cmd[64] = {0};
    char arg[MAX_COMMAND_ARG] = {0};
    cmd_type_t type;

    if (input == NULL || req == NULL) {
        return -1;
    }

    /*
     * sscanf 格式串解释：
     *   %63s       -> 最多读 63 个非空白字符到 cmd
     *   %4095[^\n] -> 读取直到换行符为止的内容到 arg
     *
     * 为什么要写宽度？
     *   因为 %s 如果不写宽度，超长输入可能把缓冲区撑爆。
     */
    if (sscanf(input, "%63s %4095[^\n]", cmd, arg) < 1) {
        return -1;
    }

    /* 先把字符串命令变成枚举。 */
    type = parse_command_type(cmd);
    if (type == CMD_INVALID) {
        return -1;
    }

    /* 再统一填充到请求结构体中。 */
    return fill_request(type, arg, req);
}

/*
 * 当前 build_command_request 只是 parse_command_request 的一层轻包装。
 * 这样以后如果“构造协议请求”和“解析命令文本”要分离，不需要大改调用点。
 */
int build_command_request(const char *input, command_request_t *req) {
    return parse_command_request(input, req);
}

/*
 * host_to_net_u64 / net_to_host_u64:
 *
 * 为什么不能直接拿 htonl 去转 64 位？
 *   因为 htonl 只处理 32 位整数。
 *
 * 这里的做法是：
 *   1. 把 64 位数拆成高 32 位和低 32 位
 *   2. 分别做 htonl/ntohl
 *   3. 再拼回去
 */
uint64_t host_to_net_u64(uint64_t value) {
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)(value & 0xffffffffU));

    /*
     * 这里要注意拼接顺序：
     * 转换后的 low 被放到了高位，
     * 转换后的 high 被放到了低位，
     * 这样最终得到的 64 位值才符合网络字节序布局。
     */
    return ((uint64_t)low << 32) | high;
}

uint64_t net_to_host_u64(uint64_t value) {
    uint32_t high = ntohl((uint32_t)(value >> 32));
    uint32_t low = ntohl((uint32_t)(value & 0xffffffffU));

    return ((uint64_t)low << 32) | high;
}

/*
 * send_n:
 * 这是网络编程里非常经典的“发满”函数。
 *
 * 为什么需要它？
 *   因为 write/send 一次不保证把所有字节都写出去。
 *
 * 例如：
 *   你要发 10000 字节，
 *   内核本次可能只接受了 4096 字节，
 *   剩下的必须继续补发。
 */
int send_n(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total = 0;

    /*
     * 只要还没发满，就继续循环。
     */
    while (total < len) {
        /*
         * 从 ptr + total 开始发，
         * 表示“前面 total 个字节已经发出去了，现在从剩余部分继续”。
         */
        ssize_t sent = write(fd, ptr + total, len - total);

        if (sent < 0) {
            /*
             * EINTR 表示这次系统调用被信号中断了。
             * 这种情况不是致命错误，重试即可。
             */
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        /*
         * 对 socket 来说，write 返回 0 一般说明连接状态已经不正常，
         * 再继续发也没有意义。
         */
        if (sent == 0) {
            return -1;
        }

        /* 把已经成功发送的字节数累计起来。 */
        total += (size_t)sent;
    }

    return 0;
}

/*
 * recv_n:
 * 这是和 send_n 对应的“收满”函数。
 *
 * 它的目标是：一定要把期望长度 len 读满。
 *
 * 为什么不能直接 recv 一次？
 *   因为 TCP 是字节流。
 *   对端即使一次 send 了完整数据，
 *   本端也可能分多次 recv 才拿全。
 */
int recv_n(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t recved = recv(fd, ptr + total, len - total, 0);

        if (recved < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        /*
         * recv 返回 0 是一个必须记住的知识点：
         *   它不表示“本次没收到，等会再读”；
         *   它表示“对端已经关闭连接了”。
         */
        if (recved == 0) {
            return -1;
        }

        total += (size_t)recved;
    }

    return 0;
}

/*
 * send_packet:
 * 把一个完整 TLV 包发送到 socket。
 *
 * 发送顺序非常明确：
 *   1. 先发送头部
 *   2. 再发送 payload
 */
int send_packet(int fd, cmd_type_t type, status_code_t status, const void *payload, uint32_t payload_len) {
    tlv_header_t header = {
        /*
         * 这里所有固定宽度整数都要转换成网络字节序。
         * 原因是网络协议必须约定统一字节序，否则不同机器之间可能解释不同。
         */
        .magic = htonl(TLV_MAGIC),
        .version = htonl(TLV_VERSION),
        .cmd_type = htonl((uint32_t)type),
        .status = htonl((uint32_t)status),
        .data_len = htonl(payload_len)
    };

    /*
     * 防御性检查：
     * payload 过大时直接拒绝发送，
     * 否则对端可能会因为分配超大内存而出问题。
     */
    if (payload_len > MAX_PACKET_PAYLOAD) {
        return -1;
    }

    /* 先发 20 字节协议头。 */
    if (send_n(fd, &header, sizeof(header)) != 0) {
        return -1;
    }

    /*
     * 只有 payload_len > 0 且 payload 非空时，才真正发送负载。
     * 像 FILE_END / 某些 ACK 这种包，完全可以只有头部没有负载。
     */
    if (payload_len > 0 && payload != NULL) {
        if (send_n(fd, payload, payload_len) != 0) {
            return -1;
        }
    }

    return 0;
}

/*
 * recv_packet:
 * 从 socket 中读出一个完整 TLV 包。
 *
 * 拆包顺序：
 *   1. 先读固定长度头部
 *   2. 把头部字段从网络字节序转回主机字节序
 *   3. 校验 magic / version / data_len
 *   4. 如果有 payload，再动态申请内存并读满
 */
int recv_packet(int fd, packet_t *packet) {
    tlv_header_t wire_header;
    uint32_t payload_len;

    if (packet == NULL) {
        return -1;
    }

    /*
     * 先把输出对象清零，
     * 防止调用者误用旧数据。
     */
    memset(packet, 0, sizeof(*packet));

    /* 先收 20 字节协议头。 */
    if (recv_n(fd, &wire_header, sizeof(wire_header)) != 0) {
        return -1;
    }

    /*
     * 收到的是“网络字节序的头”，
     * 所以这里必须逐字段转回主机字节序。
     */
    packet->header.magic = ntohl(wire_header.magic);
    packet->header.version = ntohl(wire_header.version);
    packet->header.cmd_type = ntohl(wire_header.cmd_type);
    packet->header.status = ntohl(wire_header.status);
    packet->header.data_len = ntohl(wire_header.data_len);

    /*
     * 校验魔数和版本号。
     * 如果这一步不过，说明收到的包不是我们想要的协议格式。
     */
    if (packet->header.magic != TLV_MAGIC || packet->header.version != TLV_VERSION) {
        return -1;
    }

    payload_len = packet->header.data_len;

    /*
     * 限制 payload 长度，防止恶意包伪造出一个超大长度，
     * 诱导本地分配大量内存。
     */
    if (payload_len > MAX_PACKET_PAYLOAD) {
        return -1;
    }

    /*
     * 如果没有负载，说明这个包只有头部。
     * 例如某些 ACK / FILE_END 就可能这样。
     */
    if (payload_len == 0) {
        return 0;
    }

    /*
     * 为 payload 单独分配内存。
     * 为什么不用栈数组？
     *   因为 payload 长度是运行时才知道的，不适合固定写死在栈上。
     */
    packet->payload = calloc(1, payload_len);
    if (packet->payload == NULL) {
        return -1;
    }

    /* 把 payload 读满。 */
    if (recv_n(fd, packet->payload, payload_len) != 0) {
        free_packet(packet);
        return -1;
    }

    return 0;
}

/*
 * free_packet:
 * 释放 packet 中动态分配的 payload，并把头部也清零。
 *
 * 清零的意义是：
 *   避免调用者误以为 packet 仍然保存着一个有效包。
 */
void free_packet(packet_t *packet) {
    if (packet == NULL) {
        return;
    }

    free(packet->payload);
    packet->payload = NULL;
    memset(&packet->header, 0, sizeof(packet->header));
}
