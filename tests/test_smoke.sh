#!/usr/bin/env bash
set -euo pipefail

# 这个脚本是一个“端到端烟雾测试”。
# 它的目标不是覆盖所有边界情况，
# 而是快速确认：
# 1. 项目能成功编译
# 2. 服务端能启动
# 3. 客户端常用命令能跑通
# 4. 上传下载能成功
# 5. 日志能生成
# 6. SIGINT 退出流程没有卡死

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

cleanup() {
    # 如果服务端还活着，就尝试优雅退出。
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    # 如果还有那个“空闲客户端”测试进程，也把它收掉。
    if [[ -n "${IDLE_CLIENT_PID:-}" ]]; then
        kill "$IDLE_CLIENT_PID" 2>/dev/null || true
        wait "$IDLE_CLIENT_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# 每次测试前把环境清理干净，避免旧文件影响结果。
rm -rf upload server.log client.log tests/tmp
mkdir -p upload config tests/tmp

# 动态写一份测试配置。
printf '%s\n' \
    'ip=127.0.0.1' \
    'port=9090' \
    'log=INFO' \
    'server_log=server.log' \
    'client_log=client.log' > config/config.ini

# 准备一个待上传的小文件。
printf 'hello tlv world\n' > tests/tmp/local_upload.txt

# 先重新编译。
make clean all >/tmp/windcloud_build.log 2>&1

# 启动服务端。
./server_app >/tmp/windcloud_server_stdout.log 2>&1 &
SERVER_PID=$!

# 略等一会，给服务端一个启动时间窗口。
sleep 1

# 通过标准输入把一组命令喂给客户端。
timeout 10s bash -c "printf 'pwd\nmkdir demo\ncd demo\npwd\nputs tests/tmp/local_upload.txt\ngets local_upload.txt\nrm local_upload.txt\nls\nquit\n' | ./client_app" \
    > tests/tmp/client_output.txt 2>&1

# cmp 会逐字节比较两个文件是否完全一致。
cmp tests/tmp/local_upload.txt local_upload.txt

# 确认客户端输出里能看到关键结果。
grep -q "/" tests/tmp/client_output.txt
grep -q "demo" tests/tmp/client_output.txt
grep -q "local_upload.txt" tests/tmp/client_output.txt

# 确认日志已经产生。
grep -q "INFO" server.log
grep -q "INFO" client.log

# 再起一个“空闲客户端”，专门测试：
# 当客户端处于连接中但没有继续发业务命令时，
# Ctrl+C 关闭服务端是否仍然能快速退出。
sleep 5 | ./client_app > tests/tmp/idle_client_output.txt 2>&1 &
IDLE_CLIENT_PID=$!
sleep 1

# 给服务端发 SIGINT，模拟 Ctrl+C。
kill -INT "$SERVER_PID"

# 最多等 2 秒，检查服务端是否真的快速退出。
for _ in 1 2 3 4; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done

if kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server did not exit quickly after SIGINT" >&2
    exit 1
fi

wait "$SERVER_PID"
SERVER_PID=""

# 等待空闲客户端退出。
wait "$IDLE_CLIENT_PID" 2>/dev/null || true
IDLE_CLIENT_PID=""
