#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

/*
 * 这个测试文件的定位非常明确：
 * 它不是做“大而全”的集成测试，
 * 而是专门验证协议层的一些基础能力是否正常。
 */

/*
 * test_parse_command_type:
 * 验证命令字符串是否能正确映射到协议枚举值。
 */
static void test_parse_command_type(void) {
    assert(parse_command_type("pwd") == CMD_PWD);
    assert(parse_command_type("puts") == CMD_PUTS_REQ);
    assert(parse_command_type("badcmd") == CMD_INVALID);
}

/*
 * test_send_recv_round_trip:
 * 用 socketpair 创建一对本地互通的 socket，
 * 模拟“发送端”和“接收端”。
 *
 * 为什么这里不用真的 TCP 服务端和客户端？
 * 因为这个测试只想验证 send_n / recv_n 的基本正确性，
 * 没必要把网络环境也一起引入。
 */
static void test_send_recv_round_trip(void) {
    int sv[2] = {-1, -1};

    /*
     * socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
     * 会得到一对本地互通的流式 socket。
     */
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(ret == 0);

    /* 在 sv[0] 写入 4 字节。 */
    assert(send_n(sv[0], "abcd", 4) == 0);

    /* 在 sv[1] 读取这 4 字节。 */
    char buf[4] = {0};
    assert(recv_n(sv[1], buf, sizeof(buf)) == 0);
    assert(memcmp(buf, "abcd", 4) == 0);

    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    test_parse_command_type();
    test_send_recv_round_trip();
    return 0;
}
