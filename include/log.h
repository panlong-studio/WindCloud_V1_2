#ifndef __LOG_H__
#define __LOG_H__

/*
 * 这是一个非常适合初学者理解的日志模块头文件。
 *
 * 日志模块的目的不是“把信息打印出来”这么简单，
 * 更重要的是：
 *   1. 统一日志级别
 *   2. 统一输出格式
 *   3. 在多线程环境下仍尽量保持日志可读
 *   4. 让我们后续排查问题时，能直接定位到文件、行号、函数名
 */

#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/*
 * 日志级别：
 * 数值越小，表示日志越“详细”；
 * 数值越大，表示日志越“严重”。
 *
 * 例如：
 *   如果当前日志级别设置为 INFO，
 *   那么 DEBUG 不打印，INFO/WARN/ERROR 会打印。
 */
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_ERROR 3

/*
 * init_log:
 * 功能：
 *   初始化日志系统。
 *
 * 入参：
 *   level_str - 字符串形式的日志级别，例如 "DEBUG"、"INFO"
 *   log_file  - 日志文件路径；如果传 NULL，则退化到标准输出
 *
 * 返回值：
 *   0  表示初始化成功
 *  -1 表示打开日志文件失败，但模块内部会尽量退回 stdout
 */
int init_log(const char *level_str, const char *log_file);

/*
 * close_log:
 * 功能：
 *   关闭日志系统，主要用于关闭日志文件并释放相关资源。
 */
void close_log(void);

/*
 * log_write:
 * 功能：
 *   日志系统的核心输出函数。
 *
 * 这个函数一般不直接手写调用，
 * 更推荐用下面的 LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR 宏。
 *
 * 入参：
 *   level - 日志级别
 *   file  - 调用日志的源文件名，一般由 __FILE__ 自动传入
 *   line  - 行号，一般由 __LINE__ 自动传入
 *   func  - 函数名，一般由 __FUNCTION__ 自动传入
 *   fmt   - 类似 printf 的格式串
 *   ...   - 可变参数
 */
void log_write(int level, const char *file, int line, const char *func, const char *fmt, ...);

/*
 * 日志宏：
 * 这些宏的作用是把 file/line/function 自动补上，
 * 这样调用者只需要关注“要记录什么”。
 *
 * 例如：
 *   LOG_INFO("client connected fd=%d", fd);
 *
 * 最终会自动扩展成：
 *   log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, ...);
 */
#define LOG_DEBUG(fmt, ...) do { log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...)  do { log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...)  do { log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); } while (0)
#define LOG_ERROR(fmt, ...) do { log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); } while (0)

#endif
