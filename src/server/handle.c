#include "handle.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

/*
 * SERVER_BASE_DIR:
 * 服务端真实磁盘根目录。
 *
 * 当前第一二期仍然是“单物理根目录”方案，
 * 所有客户端看到的虚拟路径，最终都映射到 ./upload 下面。
 */
#define SERVER_BASE_DIR "../tests"

/*
 * send_text_reply:
 * 功能：
 *   给客户端发送一个“文本响应包”。
 *
 * 为什么要包装一下？
 *   因为很多命令最终都只是返回一句文本，
 *   例如“进入目录成功”“目录不存在”“删除成功”。
 *   用这个函数可以减少重复拼包代码。
 */
static int send_text_reply(session_t *session, cmd_type_t type, status_code_t status, const char *msg) {
    uint32_t payload_len = 0;

    /*
     * 如果 msg 不为空，就把结尾 '\0' 一起发过去，
     * 这样客户端收到后可以直接当 C 字符串打印。
     */
    if (msg != NULL) {
        payload_len = (uint32_t)strlen(msg) + 1U;
    }

    return send_packet(session->peer_fd, type, status, msg, payload_len);
}

/*
 * copy_string_payload:
 * 功能：
 *   从 packet->payload 中安全地提取一个字符串。
 *
 * 为什么不能直接强转后 strcpy？
 *   因为要先确认：
 *   1. packet 是否为空
 *   2. payload 是否为空
 *   3. payload 长度是否会撑爆目标缓冲区
 */
static int copy_string_payload(const packet_t *packet, char *buf, size_t buf_sz, int allow_empty) {
    if (buf == NULL || buf_sz == 0U || packet == NULL) {
        return -1;
    }

    memset(buf, 0, buf_sz);

    /* 没有 payload 时，是否允许空串由 allow_empty 决定。 */
    if (packet->header.data_len == 0U) {
        return allow_empty ? 0 : -1;
    }

    if (packet->payload == NULL || packet->header.data_len > buf_sz) {
        return -1;
    }

    memcpy(buf, packet->payload, packet->header.data_len);

    /*
     * 即使对端没把 '\0' 发全，这里也强行保证末尾有一个 '\0'，
     * 避免后续把 buf 当字符串使用时越界。
     */
    buf[buf_sz - 1U] = '\0';

    return (buf[0] == '\0' && !allow_empty) ? -1 : 0;
}

/*
 * pop_one_level:
 * 功能：
 *   让一个虚拟路径退回上一级。
 *
 * 例如：
 *   /demo/a -> /demo
 *   /demo   -> /
 *
 * 这是处理 ".." 的辅助函数。
 */
static int pop_one_level(char *path) {
    char *slash;

    if (strcmp(path, "/") == 0) {
        return 0;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return -1;
    }

    /*
     * 如果最后一个 '/' 就是第一个字符，
     * 说明当前在 "/xxx" 这种一级目录，回上一级就是根目录 "/".
     */
    if (slash == path) {
        path[1] = '\0';
        return 0;
    }

    *slash = '\0';
    return 0;
}

/*
 * normalize_virtual_path:
 * 功能：
 *   把“当前虚拟路径 + 用户输入参数”规范化成新的虚拟路径。
 *
 * 这一步解决的问题是：
 *   - 处理 "."
 *   - 处理 ".."
 *   - 拒绝绝对路径
 *   - 把多层路径统一整理成标准形式
 *
 * 为什么不用 chdir？
 *   因为服务端是多线程的，chdir 修改的是进程级工作目录，不适合每个客户端独立维护路径。
 */
static int normalize_virtual_path(const char *current_path, const char *arg, char *out, size_t out_sz) {
    char merged[PATH_MAX] = {0};
    char work[PATH_MAX] = {0};
    char normalized[PATH_MAX] = "/";
    char candidate[PATH_MAX] = {0};
    char *token = NULL;
    char *saveptr = NULL;
    int n;

    if (current_path == NULL || arg == NULL || out == NULL || out_sz == 0U || arg[0] == '\0') {
        return -1;
    }

    /*
     * 当前阶段不接受绝对路径，
     * 因为客户端只能在服务端给定的虚拟目录树中移动。
     */
    if (arg[0] == '/') {
        return -1;
    }

    /*
     * 先把当前路径和用户参数“机械拼接”到一起。
     * 例如：
     *   current_path = /demo
     *   arg          = a/../b
     *   merged       = demo/a/../b
     */
    if (strcmp(current_path, "/") == 0) {
        n = snprintf(merged, sizeof(merged), "%s", arg);
    } else {
        n = snprintf(merged, sizeof(merged), "%s/%s", current_path + 1, arg);
    }
    if (n < 0 || (size_t)n >= sizeof(merged)) {
        return -1;
    }

    /*
     * strtok_r 会按 '/' 一层层切分路径片段。
     * 这里用 work 的原因是 strtok_r 会修改字符串内容。
     */
    snprintf(work, sizeof(work), "%s", merged);
    token = strtok_r(work, "/", &saveptr);

    while (token != NULL) {
        /* "." 代表当前目录，跳过即可。 */
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /*
         * ".." 代表回上一层。
         * 这里通过 pop_one_level 操作 normalized 字符串。
         */
        if (strcmp(token, "..") == 0) {
            if (pop_one_level(normalized) != 0) {
                return -1;
            }
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /*
         * 普通目录名则继续向下拼接。
         */
        if (strcmp(normalized, "/") == 0) {
            n = snprintf(candidate, sizeof(candidate), "/%s", token);
        } else {
            n = snprintf(candidate, sizeof(candidate), "%s/%s", normalized, token);
        }
        if (n < 0 || (size_t)n >= sizeof(candidate)) {
            return -1;
        }

        snprintf(normalized, sizeof(normalized), "%s", candidate);
        token = strtok_r(NULL, "/", &saveptr);
    }

    snprintf(out, out_sz, "%s", normalized);
    return 0;
}

/*
 * build_real_path:
 * 功能：
 *   把虚拟路径映射成真实磁盘路径。
 *
 * 例如：
 *   virtual_path = /
 *   real_path    = ./upload
 *
 *   virtual_path = /demo
 *   real_path    = ./upload/demo
 */
static int build_real_path(const char *virtual_path, char *real_path, size_t real_sz) {
    int n;

    if (virtual_path == NULL || real_path == NULL || real_sz == 0U) {
        return -1;
    }

    if (strcmp(virtual_path, "/") == 0) {
        n = snprintf(real_path, real_sz, "%s", SERVER_BASE_DIR);
    } else {
        n = snprintf(real_path, real_sz, "%s%s", SERVER_BASE_DIR, virtual_path);
    }

    if (n < 0 || (size_t)n >= real_sz) {
        return -1;
    }

    return 0;
}

/*
 * resolve_target_path:
 * 功能：
 *   一步完成：
 *   1. 计算目标虚拟路径
 *   2. 再计算目标真实路径
 *
 * 这样 cd / mkdir / rm / gets / puts 都能复用。
 */
static int resolve_target_path(session_t *session, const char *arg, char *virtual_path, size_t virtual_sz,
                               char *real_path, size_t real_sz) {
    if (normalize_virtual_path(session->current_path, arg, virtual_path, virtual_sz) != 0) {
        return -1;
    }
    if (build_real_path(virtual_path, real_path, real_sz) != 0) {
        return -1;
    }
    return 0;
}

/*
 * send_resume_packet:
 * 把断点偏移量打包发给对端。
 */
static int send_resume_packet(int peer_fd, cmd_type_t type, status_code_t status, uint64_t offset) {
    resume_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.offset = host_to_net_u64(offset);
    return send_packet(peer_fd, type, status, &payload, (uint32_t)sizeof(payload));
}

/*
 * send_file_info_packet:
 * 把文件名和文件大小作为一个元信息包发给对端。
 */
static int send_file_info_packet(int peer_fd, cmd_type_t type, status_code_t status,
                                 const char *file_name, uint64_t file_size) {
    file_info_payload_t payload;
    memset(&payload, 0, sizeof(payload));

    if (file_name != NULL) {
        snprintf(payload.file_name, sizeof(payload.file_name), "%s", file_name);
    }

    payload.file_size = host_to_net_u64(file_size);
    return send_packet(peer_fd, type, status, &payload, (uint32_t)sizeof(payload));
}

/*
 * extract_file_info:
 * 从 packet 中解析 file_info_payload_t。
 */
static int extract_file_info(const packet_t *packet, char *file_name, size_t file_name_sz, uint64_t *file_size) {
    const file_info_payload_t *payload;

    if (packet == NULL || packet->payload == NULL || packet->header.data_len != sizeof(file_info_payload_t)) {
        return -1;
    }

    payload = (const file_info_payload_t *)packet->payload;

    if (file_name != NULL && file_name_sz > 0U) {
        snprintf(file_name, file_name_sz, "%s", payload->file_name);
    }

    if (file_size != NULL) {
        *file_size = net_to_host_u64(payload->file_size);
    }

    return 0;
}

/*
 * extract_resume_offset:
 * 从断点包中提取 offset，并转回主机字节序。
 */
static int extract_resume_offset(const packet_t *packet, uint64_t *offset) {
    const resume_payload_t *payload;

    if (packet == NULL || packet->payload == NULL || packet->header.data_len != sizeof(resume_payload_t) ||
        offset == NULL) {
        return -1;
    }

    payload = (const resume_payload_t *)packet->payload;
    *offset = net_to_host_u64(payload->offset);
    return 0;
}

/*
 * send_file_data_packet:
 * 功能：
 *   发送一个文件块。
 *
 * 当前下载侧的设计是：
 *   1. 先手工发一个 FILE_DATA 的 TLV 头
 *   2. 再用 sendfile 发送这一块的真实文件字节
 *
 * 这样既保留了 TLV 包边界，又利用了 sendfile 的高效传输特性。
 */
static int send_file_data_packet(int peer_fd, int file_fd, off_t *offset, size_t chunk_size) {
    tlv_header_t header;
    size_t sent_total = 0U;

    memset(&header, 0, sizeof(header));
    header.magic = htonl(TLV_MAGIC);
    header.version = htonl(TLV_VERSION);
    header.cmd_type = htonl((uint32_t)CMD_FILE_DATA);
    header.status = htonl((uint32_t)STATUS_OK);
    header.data_len = htonl((uint32_t)chunk_size);

    /*
     * 先发头部，告诉客户端：
     * “接下来会有一个 FILE_DATA 包，它的 payload 长度是 chunk_size。”
     */
    if (send_n(peer_fd, &header, sizeof(header)) != 0) {
        return -1;
    }

    /*
     * 再循环调用 sendfile，把本块文件字节发完。
     * 为什么也要循环？
     * 因为 sendfile 一次也可能只发出一部分。
     */
    while (sent_total < chunk_size) {
        ssize_t sent = sendfile(peer_fd, file_fd, offset, chunk_size - sent_total);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        sent_total += (size_t)sent;
    }

    return 0;
}

/*
 * recv_follow_packet:
 * 当前函数主要用于上传/下载流程中的“下一包接收”。
 *
 * 如果收包失败，就把 session->should_close 置为 1，
 * 让外层处理循环尽快结束。
 */
static int recv_follow_packet(session_t *session, packet_t *packet) {
    if (recv_packet(session->peer_fd, packet) != 0) {
        LOG_WARN("fd=%d failed to receive follow-up packet", session->peer_fd);
        session->should_close = 1;
        return -1;
    }
    return 0;
}

/*
 * handle_pwd:
 * 直接把当前虚拟路径回给客户端。
 */
static int handle_pwd(session_t *session) {
    LOG_INFO("fd=%d handling pwd current_path=%s", session->peer_fd, session->current_path);
    return send_text_reply(session, CMD_ACK, STATUS_OK, session->current_path);
}

/*
 * handle_cd:
 * 切换当前连接的虚拟路径。
 *
 * 注意：
 * 当前服务端并不会真的 chdir()，
 * 而只是修改 session->current_path。
 */
static int handle_cd(session_t *session, const char *arg) {
    char next_virtual[PATH_MAX] = {0};
    char real_path[PATH_MAX] = {0};
    DIR *dir = NULL;

    if (resolve_target_path(session, arg, next_virtual, sizeof(next_virtual), real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "cd 参数非法");
    }

    /*
     * 真正用 opendir 去验证目标目录是否存在。
     */
    dir = opendir(real_path);
    if (dir == NULL) {
        LOG_WARN("fd=%d cd failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_NOT_FOUND, "目录不存在");
    }
    closedir(dir);

    /* 验证成功后，更新当前连接的虚拟路径。 */
    snprintf(session->current_path, sizeof(session->current_path), "%s", next_virtual);
    LOG_INFO("fd=%d cd success current_path=%s", session->peer_fd, session->current_path);
    return send_text_reply(session, CMD_ACK, STATUS_OK, "进入目录成功");
}

/*
 * handle_ls:
 * 列出当前目录内容。
 */
static int handle_ls(session_t *session) {
    char real_path[PATH_MAX] = {0};
    char result[MAX_TEXT_PAYLOAD] = {0};
    size_t used = 0U;
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    if (build_real_path(session->current_path, real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "当前路径过长");
    }

    dir = opendir(real_path);
    if (dir == NULL) {
        LOG_WARN("fd=%d ls failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "目录打开失败");
    }

    while ((entry = readdir(dir)) != NULL) {
        int n;

        /* 跳过当前目录和父目录。 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /*
         * 用 snprintf 累积拼接结果，避免 strcat 越界风险。
         */
        n = snprintf(result + used, sizeof(result) - used, "%s%s", entry->d_name, (used == 0U) ? "" : "");
        if (n < 0 || (size_t)n >= sizeof(result) - used) {
            closedir(dir);
            return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "目录内容过长");
        }
        used += (size_t)n;

        /* 每个目录项之间加一个换行，更便于客户端直接打印。 */
        if (used + 2U < sizeof(result)) {
            result[used++] = '\n';
            result[used] = '\0';
        }
    }

    closedir(dir);

    if (used == 0U) {
        snprintf(result, sizeof(result), "(empty)");
    }

    LOG_INFO("fd=%d ls success path=%s", session->peer_fd, session->current_path);
    return send_text_reply(session, CMD_ACK, STATUS_OK, result);
}

/*
 * handle_rm:
 * 删除一个文件。
 */
static int handle_rm(session_t *session, const char *arg) {
    char target_virtual[PATH_MAX] = {0};
    char real_path[PATH_MAX] = {0};

    if (resolve_target_path(session, arg, target_virtual, sizeof(target_virtual), real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "rm 参数非法");
    }

    if (remove(real_path) != 0) {
        LOG_WARN("fd=%d rm failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "删除失败");
    }

    LOG_INFO("fd=%d rm success path=%s", session->peer_fd, real_path);
    return send_text_reply(session, CMD_ACK, STATUS_OK, "删除成功");
}

/*
 * handle_mkdir:
 * 创建目录。
 */
static int handle_mkdir(session_t *session, const char *arg) {
    char target_virtual[PATH_MAX] = {0};
    char real_path[PATH_MAX] = {0};

    if (resolve_target_path(session, arg, target_virtual, sizeof(target_virtual), real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "mkdir 参数非法");
    }

    if (mkdir(real_path, 0755) != 0) {
        LOG_WARN("fd=%d mkdir failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "创建文件夹失败");
    }

    LOG_INFO("fd=%d mkdir success path=%s", session->peer_fd, real_path);
    return send_text_reply(session, CMD_ACK, STATUS_OK, "创建文件夹成功");
}

/*
 * handle_puts:
 * 服务端接收客户端上传文件。
 *
 * 核心流程：
 *   1. 解析 PUTS_REQ，得到文件名和总大小
 *   2. 打开服务端目标文件
 *   3. 通过 fstat 得到已有大小，作为断点
 *   4. 回复 PUTS_RESP(resume_offset)
 *   5. ftruncate 到最终目标大小
 *   6. mmap 整个文件
 *   7. 循环接收 FILE_DATA，写入映射区
 *   8. 收到 FILE_END 后检查是否完整
 *   9. 如果中断，截断为真实已收大小
 */
static int handle_puts(session_t *session, const packet_t *packet) {
    char file_name[PATH_MAX] = {0};
    char target_virtual[PATH_MAX] = {0};
    char real_path[PATH_MAX] = {0};
    uint64_t total_size = 0U;
    uint64_t write_offset = 0U;
    int file_fd = -1;
    char *map_ptr = NULL;
    int success = 0;

    /*
     * 先从请求包里提取文件名和总大小。
     */
    if (extract_file_info(packet, file_name, sizeof(file_name), &total_size) != 0 || file_name[0] == '\0') {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "puts 请求格式错误");
    }

    if (resolve_target_path(session, file_name, target_virtual, sizeof(target_virtual), real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "上传文件名非法");
    }

    /*
     * O_RDWR | O_CREAT:
     *   需要读写，因为后面要 ftruncate / mmap；
     *   不存在时则创建。
     */
    file_fd = open(real_path, O_RDWR | O_CREAT, 0666);
    if (file_fd == -1) {
        LOG_ERROR("fd=%d open upload target failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "服务端无法创建文件");
    }

    /*
     * 如果服务端本地已经存在同名文件，
     * 就把它当前大小当成“已上传断点”。
     */
    {
        struct stat st;
        if (fstat(file_fd, &st) == 0 && st.st_size > 0) {
            write_offset = (uint64_t)st.st_size;
        }
    }

    /*
     * 如果本地文件大小反而比客户端要上传的总大小还大，
     * 说明历史状态不可信，先清空再从头上传。
     */
    if (write_offset > total_size) {
        if (ftruncate(file_fd, 0) != 0) {
            close(file_fd);
            return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "服务端重置文件失败");
        }
        write_offset = 0U;
    }

    /*
     * 先把断点位置回给客户端，
     * 告诉它“从这个偏移继续发”。
     */
    if (send_resume_packet(session->peer_fd, CMD_PUTS_RESP, STATUS_OK, write_offset) != 0) {
        close(file_fd);
        session->should_close = 1;
        return -1;
    }

    /*
     * 只有总大小 > 0 时，才需要真正映射文件。
     * 空文件上传不需要映射。
     */
    if (total_size > 0U) {
        /*
         * ftruncate 的作用是先把文件长度扩展到最终目标大小。
         *
         * 为什么 mmap 前必须这么做？
         * 因为映射区最终要写入 total_size 这么多字节。
         * 如果文件实际长度还没扩到这么大，你写到超出原文件范围的位置时，
         * 可能触发 SIGBUS。
         */
        if (ftruncate(file_fd, (off_t)total_size) != 0) {
            close(file_fd);
            return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "服务端预分配文件失败");
        }

        /*
         * mmap:
         * 把文件映射进进程虚拟地址空间。
         *
         * 参数解释：
         *   NULL                  -> 让内核自己决定映射起始地址
         *   total_size            -> 映射长度
         *   PROT_READ|PROT_WRITE  -> 允许读写
         *   MAP_SHARED            -> 对映射区的修改会回写到底层文件
         *   file_fd               -> 目标文件 fd
         *   0                     -> 从文件头开始映射
         */
        map_ptr = mmap(NULL, (size_t)total_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd, 0);
        if (map_ptr == MAP_FAILED) {
            close(file_fd);
            return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "服务端映射文件失败");
        }
    }

    /*
     * 开始循环接收后续文件数据包。
     */
    while (1) {
        packet_t follow_packet;

        if (recv_follow_packet(session, &follow_packet) != 0) {
            break;
        }

        /*
         * FILE_END 表示客户端已经把这次上传流发完。
         */
        if ((cmd_type_t)follow_packet.header.cmd_type == CMD_FILE_END) {
            free_packet(&follow_packet);
            success = (write_offset == total_size);
            break;
        }

        /*
         * 上传过程中，除了 FILE_END，理论上应该只会收到 FILE_DATA。
         * 收到其他包，说明协议流程有问题。
         */
        if ((cmd_type_t)follow_packet.header.cmd_type != CMD_FILE_DATA) {
            free_packet(&follow_packet);
            session->should_close = 1;
            break;
        }

        /*
         * 防御性检查：写入后不能超出目标总大小。
         */
        if (write_offset + follow_packet.header.data_len > total_size) {
            free_packet(&follow_packet);
            session->should_close = 1;
            break;
        }

        /*
         * 如果当前文件不是空文件，就把收到的数据拷到映射区断点位置。
         * 这里的 memcpy 本质上是把网络数据复制到“映射后的文件内存”中。
         */
        if (follow_packet.header.data_len > 0U && map_ptr != NULL) {
            memcpy(map_ptr + write_offset, follow_packet.payload, follow_packet.header.data_len);
        }

        /* 更新服务端已收到的真实字节数。 */
        write_offset += follow_packet.header.data_len;
        free_packet(&follow_packet);
    }

    /*
     * 用完映射区后必须 munmap。
     * 否则映射关系会泄漏。
     */
    if (map_ptr != NULL) {
        munmap(map_ptr, (size_t)total_size);
    }

    /*
     * 如果这次没传完，就把文件截断为真实已收到大小，
     * 这样下次 fstat 读出来的大小才是真实断点。
     */
    if (!success) {
        ftruncate(file_fd, (off_t)write_offset);
        LOG_WARN("fd=%d upload interrupted path=%s saved=%llu total=%llu",
                 session->peer_fd, real_path, (unsigned long long)write_offset, (unsigned long long)total_size);
    } else {
        LOG_INFO("fd=%d upload success path=%s size=%llu",
                 session->peer_fd, real_path, (unsigned long long)total_size);
    }

    close(file_fd);

    if (!success) {
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "上传中断，已保存断点");
    }
    return send_text_reply(session, CMD_ACK, STATUS_OK, "上传完成");
}

/*
 * handle_gets:
 * 服务端向客户端下载文件。
 *
 * 核心流程：
 *   1. 根据文件名找到服务端文件
 *   2. 把文件大小发给客户端
 *   3. 等客户端发回它本地已有大小
 *   4. 从断点位置开始按块发送 FILE_DATA
 *   5. 全部发完后发送 FILE_END
 */
static int handle_gets(session_t *session, const char *arg) {
    char target_virtual[PATH_MAX] = {0};
    char real_path[PATH_MAX] = {0};
    int file_fd = -1;
    struct stat st;
    uint64_t total_size;
    packet_t resume_packet;
    uint64_t resume_offset = 0U;
    off_t send_offset;

    if (resolve_target_path(session, arg, target_virtual, sizeof(target_virtual), real_path, sizeof(real_path)) != 0) {
        return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "下载文件名非法");
    }

    file_fd = open(real_path, O_RDONLY);
    if (file_fd == -1) {
        LOG_WARN("fd=%d gets failed path=%s errno=%d", session->peer_fd, real_path, errno);
        return send_text_reply(session, CMD_ERROR, STATUS_NOT_FOUND, "服务端文件不存在");
    }

    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        return send_text_reply(session, CMD_ERROR, STATUS_IO_ERROR, "读取文件信息失败");
    }

    total_size = (uint64_t)st.st_size;

    /*
     * 先发 GETS_RESP，告诉客户端总大小。
     * 客户端必须先知道总大小，才能决定自己的断点位置是否有效。
     */
    if (send_file_info_packet(session->peer_fd, CMD_GETS_RESP, STATUS_OK, arg, total_size) != 0) {
        close(file_fd);
        session->should_close = 1;
        return -1;
    }

    /*
     * 再等客户端把它本地已有的断点偏移发回来。
     */
    if (recv_follow_packet(session, &resume_packet) != 0) {
        close(file_fd);
        return -1;
    }

    if ((cmd_type_t)resume_packet.header.cmd_type != CMD_RESUME_POS ||
        extract_resume_offset(&resume_packet, &resume_offset) != 0) {
        free_packet(&resume_packet);
        close(file_fd);
        session->should_close = 1;
        return -1;
    }
    free_packet(&resume_packet);

    /*
     * 如果客户端报告的断点超出了服务端文件大小，
     * 说明这个断点无效，退回从头发。
     */
    if (resume_offset > total_size) {
        resume_offset = 0U;
    }

    send_offset = (off_t)resume_offset;

    /*
     * 从断点开始按块发送。
     */
    while ((uint64_t)send_offset < total_size) {
        size_t chunk_size = (size_t)(total_size - (uint64_t)send_offset);

        if (chunk_size > FILE_BLOCK_SIZE) {
            chunk_size = FILE_BLOCK_SIZE;
        }

        if (send_file_data_packet(session->peer_fd, file_fd, &send_offset, chunk_size) != 0) {
            close(file_fd);
            session->should_close = 1;
            return -1;
        }
    }

    close(file_fd);
    LOG_INFO("fd=%d download success path=%s size=%llu resume=%llu",
             session->peer_fd, real_path, (unsigned long long)total_size, (unsigned long long)resume_offset);

    /*
     * 最后发送结束包，通知客户端“文件流已经结束”。
     */
    return send_packet(session->peer_fd, CMD_FILE_END, STATUS_OK, NULL, 0U);
}

/*
 * dispatch_command:
 * 协议路由函数。
 *
 * 它把 packet.header.cmd_type 映射到具体业务函数。
 * 这就是“枚举 + switch-case”路由的核心位置。
 */
static int dispatch_command(session_t *session, const packet_t *packet) {
    char arg[PATH_MAX] = {0};

    switch ((cmd_type_t)packet->header.cmd_type) {
    case CMD_PWD:
        return handle_pwd(session);

    case CMD_LS:
        return handle_ls(session);

    case CMD_CD:
        if (copy_string_payload(packet, arg, sizeof(arg), 0) != 0) {
            return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "cd 缺少目录参数");
        }
        return handle_cd(session, arg);

    case CMD_RM:
        if (copy_string_payload(packet, arg, sizeof(arg), 0) != 0) {
            return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "rm 缺少文件名");
        }
        return handle_rm(session, arg);

    case CMD_MKDIR:
        if (copy_string_payload(packet, arg, sizeof(arg), 0) != 0) {
            return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "mkdir 缺少目录名");
        }
        return handle_mkdir(session, arg);

    case CMD_PUTS_REQ:
        return handle_puts(session, packet);

    case CMD_GETS_REQ:
        if (copy_string_payload(packet, arg, sizeof(arg), 0) != 0) {
            return send_text_reply(session, CMD_ERROR, STATUS_BAD_REQUEST, "gets 缺少文件名");
        }
        return handle_gets(session, arg);

    default:
        /*
         * 未知命令类型一般说明客户端/服务端协议不同步，
         * 所以这里除了回错误包，也会要求关闭连接。
         */
        session->should_close = 1;
        return send_text_reply(session, CMD_ERROR, STATUS_PROTOCOL_ERROR, "未知命令类型");
    }
}

/*
 * handle_request:
 * 单个客户端连接的总处理循环。
 */
int handle_request(session_t *session) {
    packet_t packet;

    if (session == NULL) {
        return -1;
    }

    /*
     * 只要连接还没被要求关闭，就持续收包。
     */
    while (!session->should_close) {
        memset(&packet, 0, sizeof(packet));

        /*
         * recv_packet 一旦失败，通常意味着：
         *   1. 客户端断开了
         *   2. 协议头非法
         *   3. socket 发生错误
         */
        if (recv_packet(session->peer_fd, &packet) != 0) {
            LOG_INFO("fd=%d disconnected while waiting for packet", session->peer_fd);
            break;
        }

        LOG_INFO("fd=%d recv cmd=%s payload_len=%u",
                 session->peer_fd,
                 command_type_to_string((cmd_type_t)packet.header.cmd_type),
                 packet.header.data_len);

        /*
         * 把包路由给具体业务逻辑。
         */
        if (dispatch_command(session, &packet) != 0 && !session->should_close) {
            free_packet(&packet);
            break;
        }

        /* 每一轮处理结束都要释放 packet payload。 */
        free_packet(&packet);
    }

    return 0;
}
