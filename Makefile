# 1. 编译器与选项
CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LIBS    = -lpthread  # 看到你有 thread_pool.h，通常需要线程库

# 2. 目录定义
SRC_DIR    = src
INC_DIR    = include
BUILD_DIR  = build

# 3. 源文件定位
# 获取各个模块的 .c 文件
SRCS_COMMON = $(wildcard $(SRC_DIR)/common/*.c)
SRCS_CLIENT = $(wildcard $(SRC_DIR)/client/*.c)
SRCS_SERVER = $(wildcard $(SRC_DIR)/server/*.c)

# 4. 目标文件定位 (将 .c 替换为 .o，并存入 build 目录)
OBJS_COMMON = $(SRCS_COMMON:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS_CLIENT = $(SRCS_CLIENT:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS_SERVER = $(SRCS_SERVER:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 5. 最终生成的可执行文件名
TARGET_CLIENT = client_app
TARGET_SERVER = server_app
TARGET_TEST_PROTOCOL = test_protocol_bin

# 默认执行全部编译
all: $(TARGET_CLIENT) $(TARGET_SERVER)

# 编译客户端：需要客户端自身的 .o 和公共部分的 .o
$(TARGET_CLIENT): $(OBJS_CLIENT) $(OBJS_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "--- Client Build Success ---"

# 编译服务端：需要服务端自身的 .o 和公共部分的 .o
$(TARGET_SERVER): $(OBJS_SERVER) $(OBJS_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "--- Server Build Success ---"

$(TARGET_TEST_PROTOCOL): tests/test_protocol.c $(BUILD_DIR)/common/protocol.o
	$(CC) $(CFLAGS) -o $@ tests/test_protocol.c $(BUILD_DIR)/common/protocol.o $(LIBS)
	@echo "--- Protocol Test Build Success ---"

# 6. 模式规则：如何从 src 中的 .c 生成 build 中的 .o
# $(BUILD_DIR)/%.o 对应 src/%.c
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)  # 自动创建 build 下的子目录
	$(CC) $(CFLAGS) -c $< -o $@

# 7. 清理
clean:
	rm -rf $(BUILD_DIR) $(TARGET_CLIENT) $(TARGET_SERVER) $(TARGET_TEST_PROTOCOL)
	@echo "--- Clean Done ---"

.PHONY: all clean test_protocol

test_protocol: $(TARGET_TEST_PROTOCOL)
	./$(TARGET_TEST_PROTOCOL)
