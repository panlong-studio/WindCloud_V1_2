# Phase 2 TLV Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the phase-1/phase-2 client-server protocol around TLV packets, fix upload/download resume behavior, wire logging into core modules, and add lightweight regression coverage without implementing later-phase features.

**Architecture:** Keep the existing `epoll + thread pool` server shape, but add a small protocol layer shared by client and server. Route commands through `enum + switch-case`, pass a `session_t` context through server handlers, and centralize all socket framing in reusable helpers so business code no longer guesses packet boundaries.

**Tech Stack:** C, POSIX sockets, pthreads, epoll, mmap, sendfile, Makefile-based build, shell smoke tests

---

### Task 1: Introduce protocol and session interfaces

**Files:**
- Create: `include/protocol.h`
- Create: `src/common/protocol.c`
- Modify: `include/handle.h`
- Modify: `Makefile`
- Test: `tests/test_protocol.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_parse_command_type(void) {
    assert(parse_command_type("pwd") == CMD_PWD);
    assert(parse_command_type("puts") == CMD_PUTS_REQ);
    assert(parse_command_type("badcmd") == CMD_INVALID);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_protocol`
Expected: build failure because `protocol.h` and related symbols do not exist yet

- [ ] **Step 3: Write minimal implementation**

```c
typedef enum {
    CMD_INVALID = 0,
    CMD_PWD,
    CMD_CD,
    CMD_LS,
    CMD_RM,
    CMD_MKDIR,
    CMD_PUTS_REQ,
    CMD_PUTS_RESP,
    CMD_GETS_REQ,
    CMD_GETS_RESP,
    CMD_RESUME_POS,
    CMD_FILE_DATA,
    CMD_FILE_END,
    CMD_ACK,
    CMD_ERROR
} cmd_type_t;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_protocol`
Expected: PASS and exit code `0`

- [ ] **Step 5: Commit**

```bash
git add include/protocol.h src/common/protocol.c include/handle.h Makefile tests/test_protocol.c
git commit -m "refactor: add tlv protocol interfaces"
```

### Task 2: Add blocking-safe socket helpers and packet framing

**Files:**
- Modify: `include/protocol.h`
- Modify: `src/common/protocol.c`
- Test: `tests/test_protocol.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_send_recv_u32_round_trip(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(send_n(sv[0], "abcd", 4) == 0);
    char buf[4];
    assert(recv_n(sv[1], buf, 4) == 0);
    assert(memcmp(buf, "abcd", 4) == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_protocol`
Expected: FAIL because `send_n`/`recv_n` are missing

- [ ] **Step 3: Write minimal implementation**

```c
int send_n(int fd, const void *buf, size_t len);
int recv_n(int fd, void *buf, size_t len);
int send_packet(int fd, cmd_type_t type, uint32_t status, const void *payload, uint32_t payload_len);
int recv_packet(int fd, packet_t *packet);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_protocol`
Expected: PASS and exit code `0`

- [ ] **Step 5: Commit**

```bash
git add include/protocol.h src/common/protocol.c tests/test_protocol.c
git commit -m "refactor: add robust socket framing helpers"
```

### Task 3: Refactor server request handling onto `session_t` and enum dispatch

**Files:**
- Modify: `include/handle.h`
- Modify: `src/server/handle.c`
- Modify: `src/server/worker.c`
- Modify: `src/server/thread_pool.c`
- Test: `tests/test_protocol.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_parse_command_payload(void) {
    command_request_t req;
    assert(parse_command_request("mkdir demo", &req) == 0);
    assert(req.type == CMD_MKDIR);
    assert(strcmp(req.arg, "demo") == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_protocol`
Expected: FAIL because parser/session routing helpers do not exist

- [ ] **Step 3: Write minimal implementation**

```c
void handle_request(session_t *session) {
    packet_t packet;
    while (!session->should_close) {
        if (recv_packet(session->peer_fd, &packet) != 0) {
            break;
        }
        dispatch_command(session, &packet);
        free_packet(&packet);
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_protocol`
Expected: PASS and exit code `0`

- [ ] **Step 5: Commit**

```bash
git add include/handle.h src/server/handle.c src/server/worker.c src/server/thread_pool.c tests/test_protocol.c
git commit -m "refactor: route server commands through session context"
```

### Task 4: Rewrite client command flow around TLV packets

**Files:**
- Modify: `src/client/client_command_handle.c`
- Modify: `include/client_command_handle.h`
- Test: `tests/test_protocol.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_client_command_parser(void) {
    command_request_t req;
    assert(build_command_request("pwd", &req) == 0);
    assert(req.type == CMD_PWD);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_protocol`
Expected: FAIL because client-side command build helpers do not exist

- [ ] **Step 3: Write minimal implementation**

```c
int build_command_request(const char *input, command_request_t *req);
int process_command(int sock_fd, const char *input);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_protocol`
Expected: PASS and exit code `0`

- [ ] **Step 5: Commit**

```bash
git add src/client/client_command_handle.c include/client_command_handle.h tests/test_protocol.c
git commit -m "refactor: switch client commands to tlv packets"
```

### Task 5: Fix upload/download resume logic with explicit packet flow

**Files:**
- Modify: `src/server/handle.c`
- Modify: `src/client/client_command_handle.c`
- Test: `tests/test_smoke.sh`

- [ ] **Step 1: Write the failing test**

```sh
#!/usr/bin/env bash
printf "pwd\nquit\n" | ./client_app
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tests/test_smoke.sh`
Expected: one or more commands hang or file transfer checks fail with current protocol

- [ ] **Step 3: Write minimal implementation**

```c
// puts:
// request -> response(offset) -> file_data* -> file_end -> ack
// gets:
// request -> response(file_size) -> resume_pos -> file_data* -> file_end
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tests/test_smoke.sh`
Expected: `pwd/ls/mkdir/cd/puts/gets/rm` all succeed and files match

- [ ] **Step 5: Commit**

```bash
git add src/server/handle.c src/client/client_command_handle.c tests/test_smoke.sh
git commit -m "fix: restore resumable upload and download over tlv"
```

### Task 6: Wire logging across startup, socket, worker, and handler modules

**Files:**
- Modify: `src/common/log.c`
- Modify: `src/common/config.c`
- Modify: `src/client/client.c`
- Modify: `src/client/client_socket.c`
- Modify: `src/server/server.c`
- Modify: `src/server/server_socket.c`
- Modify: `src/server/worker.c`
- Modify: `src/server/thread_pool.c`
- Modify: `src/server/handle.c`
- Modify: `include/error_check.h`
- Test: `tests/test_smoke.sh`

- [ ] **Step 1: Write the failing test**

```sh
grep -q "handle_request" server.log
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tests/test_smoke.sh`
Expected: FAIL because logs are missing or incomplete

- [ ] **Step 3: Write minimal implementation**

```c
init_log("INFO", "server.log");
LOG_INFO("accepted client fd=%d", conn_fd);
LOG_WARN("client disconnected during upload");
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tests/test_smoke.sh`
Expected: log file contains module/file/line tagged entries for major flows

- [ ] **Step 5: Commit**

```bash
git add src/common/log.c src/common/config.c src/client/client.c src/client/client_socket.c src/server/server.c src/server/server_socket.c src/server/worker.c src/server/thread_pool.c src/server/handle.c include/error_check.h tests/test_smoke.sh
git commit -m "chore: add structured logging across core modules"
```

### Task 7: Harden server shutdown and final verification

**Files:**
- Modify: `src/server/server.c`
- Modify: `src/server/worker.c`
- Modify: `src/server/thread_pool.c`
- Test: `tests/test_smoke.sh`

- [ ] **Step 1: Write the failing test**

```sh
kill -INT "$server_pid"
wait "$server_pid"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tests/test_smoke.sh`
Expected: FAIL if server hangs or exits uncleanly

- [ ] **Step 3: Write minimal implementation**

```c
signal(SIGPIPE, SIG_IGN);
// self-pipe wakes epoll
// close listening socket
// broadcast exit condition
// worker observes shutdown and exits cleanly
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make clean all && make test_protocol && bash tests/test_smoke.sh`
Expected: build passes, protocol tests pass, smoke test passes, server exits cleanly

- [ ] **Step 5: Commit**

```bash
git add src/server/server.c src/server/worker.c src/server/thread_pool.c tests/test_smoke.sh
git commit -m "fix: make server shutdown deterministic"
```
