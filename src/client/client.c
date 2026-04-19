#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "client_command_handle.h"
#include "client_socket.h"
#include "config.h"
#include "log.h"

#define BUFFER_SIZE 4096

/*
 * load_value_or_default:
 * 功能：
 *   尝试从配置文件读取某个 key；
 *   如果失败，则回落到默认值。
 *
 * 这是一个很实用的小工具函数，
 * 它让主函数不需要一遍遍写“如果读配置失败就用默认值”的重复逻辑。
 */
static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};

    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    snprintf(value, value_sz, "%s", default_value);
}

int main(int argc, char *argv[]) {
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    /*
     * 当前客户端并不使用命令行参数，
     * 但为了消除编译器关于“未使用参数”的告警，
     * 这里显式把它们标记为“我知道但当前不使用”。
     */
    (void)argc;
    (void)argv;

    /*
     * 从配置文件读取连接信息和日志配置。
     * 如果配置缺失，就使用默认值。
     */
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("client_log", log_file, sizeof(log_file), "../log/client.log");

    /*
     * 初始化日志模块。
     * 即使日志文件打开失败，内部也会尽量退回 stdout。
     */
    init_log(log_level, log_file);

    /*
     * 忽略 SIGPIPE。
     *
     * 为什么客户端也要忽略 SIGPIPE？
     * 因为如果服务端先断开连接，而客户端还在 write/send，
     * 默认行为可能是整个进程被 SIGPIPE 直接杀死。
     *
     * 忽略后，写失败会通过返回值反映出来，更容易在代码层处理错误。
     */
    signal(SIGPIPE, SIG_IGN);

    int sock_fd = 0;

    /* 创建 socket 并连接服务端。 */
    init_socket(&sock_fd, ip, port);
    LOG_INFO("client connected ip=%s port=%s", ip, port);

    char input[512];

    while (1) {
        /* 打印提示符，告诉用户可以继续输入命令。 */
        printf("> ");
        fflush(stdout);

        /*
         * fgets 从标准输入读取一整行文本。
         * 如果返回 NULL，通常表示：
         *   1. 用户输入结束（例如 Ctrl+D）
         *   2. 标准输入发生错误
         */
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        /*
         * 去掉行尾换行符。
         * 否则用户输入的 "pwd\n" 会影响后续字符串比较。
         */
        input[strcspn(input, "\n")] = '\0';

        /* 用户主动退出命令行。 */
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("再见！\n");
            break;
        }

        /* 空命令没有意义，直接忽略。 */
        if (strlen(input) == 0) {
            continue;
        }

        /*
         * 进入命令处理函数。
         * 普通命令、上传、下载都会在里面分流处理。
         */
        process_command(sock_fd, input);
    }

    /* 退出前关闭 socket，释放内核资源。 */
    close(sock_fd);
    close_log();
    return 0;
}
