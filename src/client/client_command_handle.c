#include "client_command_handle.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "protocol.h"

/*
 * write_n:
 * 功能：
 *   保证把一整段数据完整写入本地文件 fd。
 *
 * 为什么客户端下载文件时也要自己封装 write_n？
 *   因为 write() 写文件时，同样可能不是一次把所有数据都写完。
 *   虽然本地普通文件短写不像 socket 那么常见，但写“完整性”这件事不能靠运气。
 */
static int write_n(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total = 0U;

    while (total < len) {
        ssize_t written = write(fd, ptr + total, len - total);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (written == 0) {
            return -1;
        }

        total += (size_t)written;
    }

    return 0;
}

/*
 * safe_basename:
 * 功能：
 *   从路径中取出“最后一级文件名”。
 *
 * 例如：
 *   tests/tmp/a.txt -> a.txt
 *
 * 为什么不直接把用户输入路径原样发给服务端？
 *   因为当前第一二期没有做完整的客户端路径到服务端路径映射，
 *   puts 上传时，只需要告诉服务端目标文件名即可。
 */
static const char *safe_basename(const char *path, char *buf, size_t buf_sz) {
    char tmp[PATH_MAX] = {0};
    char *base;

    /*
     * basename 可能会修改传入字符串，
     * 所以这里先复制到临时缓冲区，再把 basename 作用在 tmp 上。
     */
    snprintf(tmp, sizeof(tmp), "%s", path);
    base = basename(tmp);
    snprintf(buf, buf_sz, "%s", base);
    return buf;
}

/*
 * print_text_from_packet:
 * 功能：
 *   把服务端 ACK / ERROR 中携带的文本直接打印出来。
 */
static void print_text_from_packet(const packet_t *packet) {
    if (packet->payload != NULL && packet->header.data_len > 0U) {
        printf("%s\n", (const char *)packet->payload);
    }
}

/*
 * recv_text_response:
 * 功能：
 *   接收一个“文本型响应包”。
 *
 * 当前普通命令通常以：
 *   CMD_ACK 或 CMD_ERROR
 * 的形式返回。
 */
static int recv_text_response(int sock_fd) {
    packet_t packet;

    memset(&packet, 0, sizeof(packet));
    if (recv_packet(sock_fd, &packet) != 0) {
        fprintf(stderr, "接收服务端响应失败\n");
        return -1;
    }

    /*
     * 对普通命令来说，如果收到的不是 ACK 或 ERROR，
     * 就说明协议流程乱了。
     */
    if ((cmd_type_t)packet.header.cmd_type != CMD_ACK &&
        (cmd_type_t)packet.header.cmd_type != CMD_ERROR) {
        free_packet(&packet);
        fprintf(stderr, "服务端返回了意外响应类型\n");
        return -1;
    }

    print_text_from_packet(&packet);
    free_packet(&packet);
    return 0;
}

/*
 * handle_simple_command:
 * 功能：
 *   处理 pwd / cd / ls / mkdir / rm 这类“没有文件流”的普通命令。
 *
 * 核心流程：
 *   1. 把命令参数作为 payload 发出去
 *   2. 等服务端回 ACK / ERROR
 */
static int handle_simple_command(int sock_fd, const command_request_t *req) {
    const void *payload = NULL;
    uint32_t payload_len = 0U;

    /*
     * 有些命令需要参数，例如 cd demo；
     * 有些命令没有参数，例如 pwd / ls。
     */
    if (req->arg[0] != '\0') {
        payload = req->arg;

        /*
         * +1 是为了把字符串结尾的 '\0' 一起发过去。
         * 这样服务端可以把 payload 直接当 C 字符串使用。
         */
        payload_len = (uint32_t)strlen(req->arg) + 1U;
    }

    if (send_packet(sock_fd, req->type, STATUS_OK, payload, payload_len) != 0) {
        fprintf(stderr, "发送命令失败\n");
        return -1;
    }

    return recv_text_response(sock_fd);
}

/*
 * handle_puts:
 * 功能：
 *   客户端上传文件。
 *
 * 协议流程：
 *   1. 本地 open + fstat
 *   2. 发 PUTS_REQ(file_name, file_size)
 *   3. 收 PUTS_RESP(resume_offset)
 *   4. lseek 到断点
 *   5. 循环发送 FILE_DATA
 *   6. 发 FILE_END
 *   7. 收最终 ACK / ERROR
 */
static int handle_puts(int sock_fd, const command_request_t *req) {
    int file_fd = -1;
    struct stat st;
    packet_t packet;
    uint64_t file_size;
    uint64_t resume_offset = 0U;
    char base_name[PATH_MAX] = {0};
    file_info_payload_t info;

    /* 先打开本地文件。 */
    file_fd = open(req->arg, O_RDONLY);
    if (file_fd == -1) {
        perror("打开文件失败");
        return -1;
    }

    /*
     * fstat 能通过 fd 直接获取文件信息。
     * 这里主要需要它的 st_size，也就是文件总大小。
     */
    if (fstat(file_fd, &st) != 0) {
        perror("读取文件信息失败");
        close(file_fd);
        return -1;
    }

    file_size = (uint64_t)st.st_size;

    /*
     * 组装上传请求里的文件元信息。
     * 这里把本地路径转成纯文件名，避免把客户端本地目录结构暴露给服务端。
     */
    memset(&info, 0, sizeof(info));
    safe_basename(req->arg, base_name, sizeof(base_name));
    snprintf(info.file_name, sizeof(info.file_name), "%s", base_name);

    /*
     * 文件大小是 64 位整数，发网络前必须转换为网络字节序。
     */
    info.file_size = host_to_net_u64(file_size);

    if (send_packet(sock_fd, CMD_PUTS_REQ, STATUS_OK, &info, (uint32_t)sizeof(info)) != 0) {
        close(file_fd);
        fprintf(stderr, "发送上传请求失败\n");
        return -1;
    }

    /*
     * 服务端会回一个 PUTS_RESP，告诉客户端：
     * “我已经有多少字节了，你从这个偏移继续发。”
     */
    memset(&packet, 0, sizeof(packet));
    if (recv_packet(sock_fd, &packet) != 0) {
        close(file_fd);
        fprintf(stderr, "接收服务端断点失败\n");
        return -1;
    }

    /* 如果服务端直接回了错误包，就停止上传。 */
    if ((cmd_type_t)packet.header.cmd_type == CMD_ERROR) {
        print_text_from_packet(&packet);
        free_packet(&packet);
        close(file_fd);
        return -1;
    }

    /*
     * 这里额外校验：
     * 只有包类型和 payload 大小都符合预期，才说明协议没有乱。
     */
    if ((cmd_type_t)packet.header.cmd_type != CMD_PUTS_RESP ||
        packet.header.data_len != sizeof(resume_payload_t)) {
        free_packet(&packet);
        close(file_fd);
        fprintf(stderr, "服务端上传响应格式错误\n");
        return -1;
    }

    /* 从 payload 中取出断点偏移，并转回主机字节序。 */
    resume_offset = net_to_host_u64(((resume_payload_t *)packet.payload)->offset);
    free_packet(&packet);

    /*
     * 如果服务端返回的断点比本地文件还大，说明状态不可信。
     * 当前策略是退回从头开始上传。
     */
    if (resume_offset > file_size) {
        resume_offset = 0U;
    }

    /*
     * lseek 把读写位置移动到断点处。
     * 这样接下来 read() 出来的就不再是文件开头，而是“剩余还没传的部分”。
     */
    if (lseek(file_fd, (off_t)resume_offset, SEEK_SET) == (off_t)-1) {
        close(file_fd);
        perror("定位上传断点失败");
        return -1;
    }

    /*
     * 按固定块大小循环读取本地文件，再发成 FILE_DATA 包。
     */
    while (resume_offset < file_size) {
        unsigned char buf[FILE_BLOCK_SIZE];
        ssize_t nread = read(file_fd, buf, sizeof(buf));

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(file_fd);
            perror("读取本地文件失败");
            return -1;
        }

        /* nread == 0 表示已经到达文件末尾。 */
        if (nread == 0) {
            break;
        }

        if (send_packet(sock_fd, CMD_FILE_DATA, STATUS_OK, buf, (uint32_t)nread) != 0) {
            close(file_fd);
            fprintf(stderr, "发送文件块失败\n");
            return -1;
        }

        /* 每成功发送一块，就把当前断点往后推进。 */
        resume_offset += (uint64_t)nread;
    }

    close(file_fd);

    /*
     * FILE_END 的作用不是“这个包里还有数据”，
     * 而是明确告诉服务端：
     * “文件流到这里结束了。”
     *
     * 没有这个结束标记的话，服务端就不知道后续还会不会有更多 FILE_DATA。
     */
    if (send_packet(sock_fd, CMD_FILE_END, STATUS_OK, NULL, 0U) != 0) {
        fprintf(stderr, "发送文件结束标记失败\n");
        return -1;
    }

    /* 等待服务端确认上传结果。 */
    if (recv_text_response(sock_fd) != 0) {
        return -1;
    }

    printf("上传完成: %s\n", base_name);
    LOG_INFO("puts success file=%s", base_name);
    return 0;
}

/*
 * handle_gets:
 * 功能：
 *   客户端下载文件，并支持断点续传。
 *
 * 协议流程：
 *   1. 发 GETS_REQ(file_name)
 *   2. 收 GETS_RESP(file_size)
 *   3. 检查本地已有大小
 *   4. 发 RESUME_POS(local_size)
 *   5. 循环收 FILE_DATA
 *   6. 收到 FILE_END 表示完成
 */
static int handle_gets(int sock_fd, const command_request_t *req) {
    int file_fd = -1;
    packet_t packet;
    struct stat st;
    uint64_t server_size = 0U;
    uint64_t local_size = 0U;
    resume_payload_t resume_payload;

    /* 先把想下载的文件名发给服务端。 */
    if (send_packet(sock_fd, CMD_GETS_REQ, STATUS_OK, req->arg, (uint32_t)strlen(req->arg) + 1U) != 0) {
        fprintf(stderr, "发送下载请求失败\n");
        return -1;
    }

    /*
     * 服务端会先回一个 GETS_RESP，
     * 告诉客户端目标文件总大小是多少。
     */
    memset(&packet, 0, sizeof(packet));
    if (recv_packet(sock_fd, &packet) != 0) {
        fprintf(stderr, "接收服务端文件信息失败\n");
        return -1;
    }

    if ((cmd_type_t)packet.header.cmd_type == CMD_ERROR) {
        print_text_from_packet(&packet);
        free_packet(&packet);
        return -1;
    }

    if ((cmd_type_t)packet.header.cmd_type != CMD_GETS_RESP ||
        packet.header.data_len != sizeof(file_info_payload_t)) {
        free_packet(&packet);
        fprintf(stderr, "服务端下载响应格式错误\n");
        return -1;
    }

    server_size = net_to_host_u64(((file_info_payload_t *)packet.payload)->file_size);
    free_packet(&packet);

    /*
     * 以读写方式打开本地目标文件。
     * O_CREAT 表示如果文件不存在就创建。
     */
    file_fd = open(req->arg, O_RDWR | O_CREAT, 0666);
    if (file_fd == -1) {
        perror("创建下载文件失败");
        return -1;
    }

    /*
     * 如果本地已经存在这个文件，就先读取它当前大小，
     * 把这个大小作为下载断点。
     */
    if (fstat(file_fd, &st) == 0 && st.st_size > 0) {
        local_size = (uint64_t)st.st_size;
    }

    /*
     * 如果本地文件反而比服务端文件还大，说明本地内容已经不可信。
     * 当前做法是清空本地文件，重新从头下载。
     */
    if (local_size > server_size) {
        if (ftruncate(file_fd, 0) != 0) {
            close(file_fd);
            perror("重置本地文件失败");
            return -1;
        }
        local_size = 0U;
    }

    /*
     * 把本地文件写指针移动到断点位置。
     * 这样后续写入数据时，会从“已下载内容之后”继续写。
     */
    if (lseek(file_fd, (off_t)local_size, SEEK_SET) == (off_t)-1) {
        close(file_fd);
        perror("定位下载断点失败");
        return -1;
    }

    /*
     * 把客户端已有大小打包发送给服务端。
     * 这一步就是下载断点续传的核心同步动作。
     */
    memset(&resume_payload, 0, sizeof(resume_payload));
    resume_payload.offset = host_to_net_u64(local_size);
    if (send_packet(sock_fd, CMD_RESUME_POS, STATUS_OK, &resume_payload, (uint32_t)sizeof(resume_payload)) != 0) {
        close(file_fd);
        fprintf(stderr, "发送断点位置失败\n");
        return -1;
    }

    /*
     * 持续接收服务端后续发来的文件块。
     * 直到收到 FILE_END。
     */
    while (1) {
        memset(&packet, 0, sizeof(packet));
        if (recv_packet(sock_fd, &packet) != 0) {
            close(file_fd);
            fprintf(stderr, "接收下载数据失败\n");
            return -1;
        }

        /* FILE_END 表示服务端已经把剩余数据发完。 */
        if ((cmd_type_t)packet.header.cmd_type == CMD_FILE_END) {
            free_packet(&packet);
            break;
        }

        if ((cmd_type_t)packet.header.cmd_type == CMD_ERROR) {
            print_text_from_packet(&packet);
            free_packet(&packet);
            close(file_fd);
            return -1;
        }

        /*
         * 如果这里收到的不是 FILE_DATA，
         * 就说明客户端和服务端对协议流程的理解已经不同步了。
         */
        if ((cmd_type_t)packet.header.cmd_type != CMD_FILE_DATA) {
            free_packet(&packet);
            close(file_fd);
            fprintf(stderr, "收到未知文件数据包\n");
            return -1;
        }

        /*
         * 把这一块真实写入本地文件。
         * 不能偷懒只写一次 write，因为理论上也可能发生短写。
         */
        if (write_n(file_fd, packet.payload, packet.header.data_len) != 0) {
            free_packet(&packet);
            close(file_fd);
            perror("写入本地文件失败");
            return -1;
        }

        local_size += packet.header.data_len;
        free_packet(&packet);
    }

    close(file_fd);
    printf("下载完成: %s (%llu 字节)\n", req->arg, (unsigned long long)server_size);
    LOG_INFO("gets success file=%s size=%llu", req->arg, (unsigned long long)server_size);
    return 0;
}

/*
 * process_command:
 * 功能：
 *   客户端命令总入口。
 *
 * 它先做命令文本解析，再根据命令类型进入不同处理分支。
 */
int process_command(int sock_fd, const char *input) {
    command_request_t req;

    memset(&req, 0, sizeof(req));

    /*
     * build_command_request 会把原始字符串解析成：
     *   req.type
     *   req.arg
     */
    if (build_command_request(input, &req) != 0) {
        fprintf(stderr, "无效命令\n");
        LOG_WARN("invalid command input=%s", input);
        return -1;
    }

    /*
     * 普通命令直接走 handle_simple_command；
     * 文件传输命令单独走上传/下载逻辑。
     */
    switch (req.type) {
    case CMD_PWD:
    case CMD_CD:
    case CMD_LS:
    case CMD_RM:
    case CMD_MKDIR:
        return handle_simple_command(sock_fd, &req);

    case CMD_PUTS_REQ:
        /*
         * puts 必须带文件名参数。
         * 否则服务端根本不知道要接收什么文件。
         */
        if (req.arg[0] == '\0') {
            fprintf(stderr, "用法: puts <文件名>\n");
            return -1;
        }
        return handle_puts(sock_fd, &req);

    case CMD_GETS_REQ:
        /*
         * gets 也必须带文件名参数。
         */
        if (req.arg[0] == '\0') {
            fprintf(stderr, "用法: gets <文件名>\n");
            return -1;
        }
        return handle_gets(sock_fd, &req);

    default:
        fprintf(stderr, "暂不支持该命令\n");
        return -1;
    }
}
