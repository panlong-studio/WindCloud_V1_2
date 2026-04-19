# 现存 BUG 审计报告

本文档仅覆盖 `项目概述.md` 中**第一期/第二期**的核心功能，不涉及第三期到第五期的数据库、JWT、多点下载等后续内容。

说明：

- 下面的“修复后代码段”是为了说明修复方向，第二阶段真正落地时会统一收敛到 `enum + switch-case + TLV 协议 + session/client_info 结构体`。
- 行号基于当前工作区代码。

---

## 1. `gets` 下载协议错位，客户端与服务端会直接互相阻塞

**位置：**

- `src/client/client_command_handle.c:25-57`
- `src/server/handle.c:167-215`

**现象：**

- 输入 `gets file` 后，下载流程会卡死。
- 如果服务器文件不存在，客户端会把“长度 + 错误字符串”误当成 `off_t file_size` 读取，后续协议流彻底错位。
- 客户端即使中途收包失败，也会直接打印“下载成功”。

**分析：**

- 服务端当前流程是：`send(file_total_size)` -> `recv(offset)` -> `sendfile(...)`。
- 客户端当前流程是：`recv(file_size)` -> 直接 `recv(file content)`。
- 也就是说，客户端**从未发送断点续传 offset**，但服务端在 `handle_gets()` 中明确阻塞等待它，因此双方会死锁。
- 更严重的是，服务端文件不存在时调用 `send_msg()`，它发的是 `int len + char[]`；客户端却固定按 `off_t` 去读，TCP 是字节流，没有消息边界，这会把后续报文全部拆坏。
- 客户端还使用 `O_TRUNC` 打开本地文件，第一次就把断点文件截断了，第二期要求的断点续传实际上没有成立。

**修复对比：**

原始代码段：

```c
// client_command_handle.c
off_t file_size = 0;
recv(sock_fd, &file_size, sizeof(off_t), MSG_WAITALL);

int fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0755);
while (remaining > 0) {
    int ret = recv(sock_fd, buf, to_read, MSG_WAITALL);
    if (ret <= 0) break;
    write(fd, buf, ret);
}
```

```c
// handle.c
send(listen_fd, &file_total_size, sizeof(off_t), 0);

off_t offset = 0;
if (recv(listen_fd, &offset, sizeof(off_t), MSG_WAITALL) <= 0) {
    close(file_fd);
    return;
}
```

修复后代码段：

```c
// 方向：统一改成 TLV 响应头，先告诉客户端状态、总大小，再让客户端回传 resume_offset
tlv_header_t hdr = {
    .cmd_type = CMD_GETS_RESP,
    .status = STATUS_OK,
    .data_len = sizeof(file_meta_t)
};
file_meta_t meta = {
    .file_size = file_total_size
};

send_n(peer_fd, &hdr, sizeof(hdr));
send_n(peer_fd, &meta, sizeof(meta));

recv_n(peer_fd, &resume_offset, sizeof(resume_offset));
send_file_by_offset(peer_fd, file_fd, resume_offset);
```

```c
// 客户端必须先保留本地已下载大小，再回传 offset
off_t local_size = get_local_file_size(arg);

recv_n(sock_fd, &hdr, sizeof(hdr));
recv_n(sock_fd, &meta, sizeof(meta));

send_n(sock_fd, &local_size, sizeof(local_size));
recv_file_to_fd(sock_fd, fd, local_size, meta.file_size);
```

---

## 2. `puts` 上传协议同样错位，续传会损坏文件，成功路径还会卡死

**位置：**

- `src/client/client_command_handle.c:60-98`
- `src/server/handle.c:220-301`

**现象：**

- `puts file` 在“看起来上传完成”后，客户端仍可能永久阻塞。
- 服务器已有半截同名文件时，客户端没有从服务器给出的断点位置继续传，而是从头开始发，最终导致文件内容重复或错位。
- 如果本地文件打开失败，客户端已经把 `puts` 命令发出去了，服务器会一直阻塞在 `recv(file_len)`。

**分析：**

- 服务端逻辑是：`recv(file_len)` -> `send(local_size)` -> 从 `local_size` 偏移处写 mmap。
- 客户端逻辑是：`send(file_size)` -> 直接从头发送文件内容 -> 最后等待一个长度包 `resp_len`。
- 这里存在 3 个硬错误：
- 第一，客户端**没有先接收服务端发来的 `local_size`**，因此真正的续传同步根本没发生。
- 第二，服务端成功上传后没有发送任何统一 ACK，而客户端无条件 `recv(resp_len)`，所以会卡住。
- 第三，客户端在校验参数和本地文件存在性之前，就已经把命令发到服务器，导致服务器进入等待态。

**修复对比：**

原始代码段：

```c
// client_command_handle.c
send(sock_fd, &file_size, sizeof(off_t), 0);

while (remaining > 0) {
    int n = read(fd, buf, to_read);
    if (n <= 0) break;
    send(sock_fd, buf, n, 0);
}

int resp_len = 0;
recv(sock_fd, &resp_len, sizeof(int), MSG_WAITALL);
```

```c
// handle.c
recv(listen_fd, &file_len, sizeof(off_t), MSG_WAITALL);
send(listen_fd, &local_size, sizeof(off_t), 0);

while (received_count < remaining) {
    ssize_t ret = recv(listen_fd, write_start + received_count, remaining - received_count, 0);
    if (ret <= 0) {
        send_msg(listen_fd,"传输中断，已保存当前进度。\n");
        break;
    }
    received_count += ret;
}
```

修复后代码段：

```c
// 客户端先完成本地校验，再发请求；收到服务器 offset 后再 lseek 到断点位置
int fd = open(arg, O_RDONLY);
struct stat st;
fstat(fd, &st);

send_puts_request(sock_fd, arg, st.st_size);
recv_n(sock_fd, &server_offset, sizeof(server_offset));

lseek(fd, server_offset, SEEK_SET);
send_file_stream(sock_fd, fd, st.st_size - server_offset);

recv_puts_ack(sock_fd);
```

```c
// 服务端成功和失败都必须回统一 ACK，不能让客户端猜
recv_puts_request(peer_fd, &req);
local_size = get_existing_size(real_path);
send_n(peer_fd, &local_size, sizeof(local_size));

recv_file_to_storage(peer_fd, file_fd, local_size, req.file_size);
send_simple_ack(peer_fd, STATUS_OK, "upload complete");
```

---

## 3. 大量 `send/recv` 只调用一次，没有处理半包/短写，TCP 粘包问题没有真正解决

**位置：**

- `src/server/handle.c:16-23,40-49,187,192,205,227,247,281`
- `src/client/client_command_handle.c:21-23,31-32,49,76,84,92,94,101-105`

**现象：**

- 命令长度、响应长度、文件大小、文件内容都可能出现“只收一半/只发一半”的情况。
- 轻则客户端阻塞，重则文件内容错乱、协议边界错位。
- 在网络抖动或大文件场景下更容易暴露。

**分析：**

- TCP 只有“字节流”概念，没有“这次 `send()` 就对应对端一次 `recv()`”这种保证。
- 单次 `send()` 可能只写出一部分数据；单次 `recv()` 也可能只拿到一部分。
- 代码里虽然个别地方用了 `MSG_WAITALL`，但不是全链路统一设计；而且 `send()` 侧依然没有循环发送。
- 目前协议还直接传裸 `int` 和裸 `off_t`，既没有统一报头，也没有网络字节序转换，不同位宽/端序之间不可移植。

**修复对比：**

原始代码段：

```c
send(sock_fd, &len, sizeof(int), 0);
send(sock_fd, input, len, 0);

recv(sock_fd, &resp_len, sizeof(int), MSG_WAITALL);
recv(sock_fd, response, resp_len, MSG_WAITALL);
```

修复后代码段：

```c
int send_n(int fd, const void *buf, size_t len);
int recv_n(int fd, void *buf, size_t len);

tlv_header_t hdr;
hdr.cmd_type = htonl(cmd_type);
hdr.data_len = htonl(payload_len);

send_n(fd, &hdr, sizeof(hdr));
send_n(fd, payload, payload_len);

recv_n(fd, &hdr, sizeof(hdr));
payload_len = ntohl(hdr.data_len);
recv_n(fd, payload, payload_len);
```

---

## 4. 服务端对 `cmd_len` 和字符串解析完全信任，存在栈溢出风险

**位置：**

- `src/server/handle.c:38-55`

**现象：**

- 客户端只要伪造一个超大的 `cmd_len`，服务端就会把任意长度数据写进 `cmd_buf[512]`，造成栈溢出。
- 即使 `cmd_buf` 没炸，`sscanf(cmd_buf, "%s %s", cmd, arg)` 也没有字段宽度限制，超长 token 仍可覆盖 `cmd[64]` 和 `arg[256]`。

**分析：**

- 这里是典型的“网络长度字段未校验 + 固定栈缓冲区”错误。
- `recv()` 在 `MSG_WAITALL` 下会尽量凑满 `cmd_len`，一旦 `cmd_len > sizeof(cmd_buf)` 就直接越界。
- `sscanf("%s %s")` 同样不会自动帮你做边界保护。

**修复对比：**

原始代码段：

```c
int cmd_len = 0;
recv(listen_fd, &cmd_len, sizeof(int), MSG_WAITALL);

char cmd_buf[512] = {0};
recv(listen_fd, cmd_buf, cmd_len, MSG_WAITALL);

char cmd[64] = {0};
char arg[256] = {0};
sscanf(cmd_buf, "%s %s", cmd, arg);
```

修复后代码段：

```c
if (recv_n(peer_fd, &cmd_len_net, sizeof(cmd_len_net)) != 0) {
    return;
}

cmd_len = ntohl(cmd_len_net);
if (cmd_len == 0 || cmd_len >= sizeof(cmd_buf)) {
    send_simple_ack(peer_fd, STATUS_BAD_REQUEST, "invalid command length");
    return;
}

recv_n(peer_fd, cmd_buf, cmd_len);
cmd_buf[cmd_len] = '\0';
sscanf(cmd_buf, "%63s %255s", cmd, arg);
```

---

## 5. 路径拼接没有做归一化检查，任意客户端都能逃出 `./upload`；“伪多用户”功能也没有实现

**位置：**

- `src/server/handle.c:15,26-31,36,78-165,167-223`

**现象：**

- 客户端可以构造 `../`、绝对路径、深层目录等参数，访问或删除 `./upload` 之外的真实文件。
- 所有连接都共享同一个 `SERVER_BASE_DIR`，不存在“zs/ls”这类伪用户隔离目录。
- 一个客户端上传/删除的文件，所有其他客户端都直接可见。

**分析：**

- `get_real_path()` 只是 `sprintf("%s/%s", SERVER_BASE_DIR, arg)`，完全没有路径规范化，也没有校验结果是否仍位于根目录之下。
- `current_path` 只是每个连接的字符串状态，不代表用户身份，更不代表用户根目录。
- `项目概述.md` 第一阶段明确提到“暂时使用虚假的用户模拟登录和区分”，当前代码并未实现该要求。

**修复对比：**

原始代码段：

```c
#define SERVER_BASE_DIR "./upload"

void get_real_path(char *res, const char *path, const char *arg) {
    if (strcmp(path, "/") == 0) {
        sprintf(res, "%s/%s", SERVER_BASE_DIR, arg);
    } else {
        sprintf(res, "%s%s/%s", SERVER_BASE_DIR, path, arg);
    }
}
```

修复后代码段：

```c
typedef struct session_s {
    int peer_fd;
    int user_id;
    char user_root[PATH_MAX];
    char current_path[PATH_MAX];
} session_t;

int build_safe_path(session_t *sess, const char *arg, char *out, size_t out_sz) {
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/%s", sess->current_path, arg);

    if (realpath(candidate, out) == NULL) {
        return -1;
    }
    if (strncmp(out, sess->user_root, strlen(sess->user_root)) != 0) {
        return -1;
    }
    return 0;
}
```

---

## 6. `ls` 成功路径没有 `closedir()`，而且结果字符串会被 `strcat()` 撑爆

**位置：**

- `src/server/handle.c:114-135`

**现象：**

- 反复执行 `ls` 会持续泄漏目录文件描述符，最终可能出现“too many open files”。
- 某个目录下文件很多或文件名很长时，`result[4096]` 会越界，导致栈破坏。

**分析：**

- `opendir()` 成功后，只在失败分支直接返回，没有在正常遍历结束后 `closedir(dir)`。
- `strcat(result, file->d_name)` 与 `strcat(result, " ")` 都没有剩余空间判断。

**修复对比：**

原始代码段：

```c
DIR *dir;
struct dirent *file;
dir = opendir(real_path);

char result[4096] = {0};
while ((file = readdir(dir)) != NULL) {
    strcat(result, file->d_name);
    strcat(result, " ");
}
send_msg(listen_fd, result);
```

修复后代码段：

```c
char result[4096] = {0};
size_t used = 0;

while ((file = readdir(dir)) != NULL) {
    int n = snprintf(result + used, sizeof(result) - used, "%s ", file->d_name);
    if (n < 0 || (size_t)n >= sizeof(result) - used) {
        closedir(dir);
        send_simple_ack(peer_fd, STATUS_TOO_LARGE, "ls result too large");
        return;
    }
    used += (size_t)n;
}

closedir(dir);
send_msg(peer_fd, result);
```

---

## 7. 线程池退出机制不完整，`Ctrl+C` 时只会唤醒空闲线程，忙线程会把进程卡死

**位置：**

- `src/server/server.c:20-25,42-47,74-87`
- `src/server/worker.c:12-34`

**现象：**

- 服务器收到 `SIGINT` 后，如果某个工作线程正阻塞在 `handle_request()` 的 `recv()`/文件传输里，主线程 `pthread_join()` 会一直等，程序无法退出。
- `func()` 里调用 `printf()` 还可能引入异步信号安全问题。

**分析：**

- `pool.exitFlag` 只在线程处于 `pthread_cond_wait()` 时有效；一旦线程已经取到 `client_fd` 进入 `handle_request()`，它完全看不到退出标志。
- 主线程广播条件变量后马上 `pthread_join()`，但忙线程仍阻塞在网络 I/O。
- `printf()` 不属于 async-signal-safe 函数，放在信号处理函数里是未定义行为。

**修复对比：**

原始代码段：

```c
void func(int num){
    printf("num=%d\n",num);
    write(pipe_fd[1],"1",1);
}

pool.exitFlag = 1;
pthread_cond_broadcast(&pool.cond);
for(int i = 0; i < pool.num; i++){
    pthread_join(pool.thread_id_arr[i], NULL);
}
```

修复后代码段：

```c
static void sigint_handler(int signo) {
    (void)signo;
    write(pipe_fd[1], "1", 1);
}

// 主线程收到退出事件后：
pool.exitFlag = 1;
close(listen_fd);
shutdown_all_clients(&pool);
pthread_cond_broadcast(&pool.cond);

for (int i = 0; i < pool.num; ++i) {
    pthread_join(pool.thread_id_arr[i], NULL);
}
```

---

## 8. 文件传输路径上的 `send/sendfile` 没有处理 `SIGPIPE`，对端断开时可能直接打死整个进程

**位置：**

- `src/server/handle.c:187,205,247`
- `src/client/client_command_handle.c:22-23,76,84`

**现象：**

- 客户端下载到一半断开时，服务端在 `send()` / `sendfile()` 上可能收到 `SIGPIPE` 并被默认行为终止。
- 客户端在服务端已断开的情况下继续 `send()` 命令或文件内容，也可能被 `SIGPIPE` 杀掉。

**分析：**

- 只有 `send_msg()` 用了 `MSG_NOSIGNAL`，其他发送路径全是裸 `send()` 或 `sendfile()`。
- Linux 下向一个已经被对端关闭的流式 socket 写数据，默认会触发 `SIGPIPE`。
- 在多线程服务器里，这种错误不是“某个连接失败”，而是**整个进程直接退出**。

**修复对比：**

原始代码段：

```c
send(listen_fd, &file_total_size, sizeof(off_t), 0);
send(listen_fd, &local_size, sizeof(off_t), 0);
ssize_t sent = sendfile(listen_fd, file_fd, &offset, remaining);
```

修复后代码段：

```c
signal(SIGPIPE, SIG_IGN);

if (send_n(peer_fd, &file_total_size, sizeof(file_total_size)) != 0) {
    close(file_fd);
    return;
}

while (remaining > 0) {
    ssize_t sent = sendfile(peer_fd, file_fd, &offset, chunk);
    if (sent < 0 && errno == EPIPE) {
        break;
    }
}
```

---

## 9. 日志系统没有初始化就被 `ERROR_CHECK` 使用，错误路径上会先崩日志；并且时间格式化存在线程竞争

**位置：**

- `src/common/log.c:3-5,31-45,59-85`
- `include/error_check.h:18-30`
- `src/server/server.c:35-103`
- `src/client/client.c:25-63`

**现象：**

- 当前客户端和服务端启动流程都没有调用 `init_log()`。
- 一旦 `ERROR_CHECK` 触发，`LOG_ERROR` 会进入 `fprintf(g_log_fp, ...)`，而此时 `g_log_fp == NULL`，进程可能直接崩溃。
- 多线程同时写日志时，`localtime()` 使用静态共享区，时间字符串可能被并发覆盖。

**分析：**

- `g_log_fp` 初始化为 `NULL`，只有 `init_log()` 成功后才会变成有效 `FILE*`。
- 代码全局使用 `ERROR_CHECK`，但从未在 `main()` 调用 `init_log()`，导致错误路径比业务路径更脆弱。
- `localtime()` 不是线程安全函数，而锁是在它之后才加的，保护范围不对。

**修复对比：**

原始代码段：

```c
static FILE* g_log_fp = NULL;

void log_write(...) {
    struct tm* tm_info = localtime(&now);
    pthread_mutex_lock(&g_log_mutex);
    fprintf(g_log_fp, "[%s] ...", time_str);
    pthread_mutex_unlock(&g_log_mutex);
}
```

修复后代码段：

```c
static FILE *g_log_fp = NULL;

int init_log(const char *level_str, const char *log_file) {
    g_log_fp = stdout;
    ...
}

void log_write(...) {
    struct tm tm_info;

    pthread_mutex_lock(&g_log_mutex);
    localtime_r(&now, &tm_info);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
    fprintf(g_log_fp ? g_log_fp : stdout, "[%s] ...", time_str);
    pthread_mutex_unlock(&g_log_mutex);
}
```

---

## 10. 线程创建错误检查写错了，线程池可能“半初始化成功”

**位置：**

- `src/server/thread_pool.c:16-20`

**现象：**

- 当系统线程资源不足时，`pthread_create()` 返回的是**非 0 错误码**，但当前代码只检查 `-1`。
- 结果是：线程创建失败不会被发现，线程池里可能混着未创建成功的线程 ID，后面 `pthread_join()` 或任务分发会出现未定义行为。

**分析：**

- POSIX 线程接口与 `open()`、`socket()` 这类系统调用不同，它不通过 `errno + -1` 表示错误。
- 当前 `ERROR_CHECK(ret, -1, "pthread_create")` 是错误的检查方式。

**修复对比：**

原始代码段：

```c
pool->thread_id_arr = (pthread_t*)malloc(num * sizeof(pthread_t));

for (int idx = 0; idx < num; ++idx) {
    int ret = pthread_create(&pool->thread_id_arr[idx], NULL, thread_func, (void*) pool);
    ERROR_CHECK(ret, -1, "pthread_create");
}
```

修复后代码段：

```c
pool->thread_id_arr = calloc((size_t)num, sizeof(pthread_t));
if (pool->thread_id_arr == NULL) {
    return -1;
}

for (int idx = 0; idx < num; ++idx) {
    int ret = pthread_create(&pool->thread_id_arr[idx], NULL, thread_func, pool);
    THREAD_ERROR_CHECK(ret, "pthread_create");
}
```

---

## 11. 客户端在校验参数之前就把命令发出去了，错误输入会把连接状态拖坏

**位置：**

- `src/client/client_command_handle.c:21-29,60-69`

**现象：**

- 输入 `gets`（不带文件名）或 `puts`（不带文件名）时，客户端本地虽然打印了用法提示，但命令其实已经发给服务端。
- `puts no_such_file` 时，本地 `open()` 失败后直接返回，但服务端已经进入 `handle_puts()` 并阻塞等待 `file_len`。
- 下一条命令再发出去时，会和上一条残留状态混在一起。

**分析：**

- `process_command()` 先执行：
  - `send(sock_fd, &len, sizeof(int), 0);`
  - `send(sock_fd, input, len, 0);`
- 然后才进入 `gets/puts` 的参数和文件检查逻辑。
- 对 TCP 协议来说，这意味着“应用层已经承诺发起该命令”，但后续业务负载又没发完，连接状态必然不一致。

**修复对比：**

原始代码段：

```c
int len = strlen(input);
send(sock_fd, &len, sizeof(int), 0);
send(sock_fd, input, len, 0);

if (strcmp(cmd, "puts") == 0) {
    int fd = open(arg, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        return;
    }
}
```

修复后代码段：

```c
if (strcmp(cmd, "puts") == 0) {
    if (strlen(arg) == 0) {
        fprintf(stderr, "用法: puts <文件名>\n");
        return;
    }

    int fd = open(arg, O_RDONLY);
    if (fd == -1) {
        perror("打开文件失败");
        return;
    }
}

send_command_frame(sock_fd, cmd, arg);
```

---

## 12. `cd`/`mkdir`/`rm`/路径缓存大量使用 `sprintf/strcat`，深路径下可触发缓冲区覆盖

**位置：**

- `src/server/handle.c:26-31,92-107,142-158`

**现象：**

- 恶意或异常长文件名、目录名会覆盖 `real_path[1024]` 或 `current_path[512]`。
- 轻则当前连接崩溃，重则线程栈被破坏。

**分析：**

- 当前路径和真实路径全靠 `sprintf()`/`strcat()` 直接拼接。
- 没有任何剩余空间检查，也没有路径长度上限反馈。
- 这和第 4 条“命令长度不校验”叠加时，属于非常危险的远程输入面。

**修复对比：**

原始代码段：

```c
sprintf(real_path, "%s%s/%s", SERVER_BASE_DIR, current_path, arg);
strcat(current_path, "/");
strcat(current_path, arg);
```

修复后代码段：

```c
int n = snprintf(real_path, sizeof(real_path), "%s%s/%s", base_dir, current_path, arg);
if (n < 0 || (size_t)n >= sizeof(real_path)) {
    send_simple_ack(peer_fd, STATUS_BAD_REQUEST, "path too long");
    return;
}

n = snprintf(current_path, current_path_sz, "%s/%s", current_path, arg);
if (n < 0 || (size_t)n >= current_path_sz) {
    send_simple_ack(peer_fd, STATUS_BAD_REQUEST, "virtual path too long");
    return;
}
```

---

## 审计结论

当前代码的主要问题不是某一两个函数写错，而是**协议层、连接生命周期、路径安全、退出模型**四个基础面同时不稳：

- 文件上传/下载协议已经发生双向错位，`gets/puts` 在当前版本下都不能稳定工作。
- TCP 收发没有做统一封装，第一期“命令 + 文本响应”和第二期“断点续传 + 文件流”被混在同一条裸 socket 字节流里，天然容易粘包/半包/阻塞。
- 伪多用户隔离没有落地，当前所有连接共享同一真实目录。
- 线程池退出机制只能退出“空闲线程”，不能退出“正在阻塞收包的线程”。

第二阶段修复时，建议严格围绕这 4 个面收口，不要扩散到第三期及之后的功能。
