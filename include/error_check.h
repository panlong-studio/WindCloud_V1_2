#ifndef __ERROR_CHECK_H__
#define __ERROR_CHECK_H__

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

/*
 * ARGS_CHECK:
 * 功能：
 *   校验命令行参数数量是否符合预期。
 *
 * 如果不符合，会记录错误日志并直接退出程序。
 */
#define ARGS_CHECK(argc, expected) \
    do { \
        if ((argc) != (expected)) { \
            LOG_ERROR("Args number error! Expected %d, got %d", expected, argc); \
            exit(1); \
        } \
    } while (0)

/*
 * ERROR_CHECK:
 * 功能：
 *   检查“像系统调用那样”的返回值。
 *
 * 适用场景：
 *   open/socket/bind/listen/epoll_create 等通常以 -1 表示失败。
 *
 * 注意：
 *   它不适合 pthread_create 这种“失败返回非 0 错误码”的接口，
 *   那类接口要用 THREAD_ERROR_CHECK。
 */
#define ERROR_CHECK(ret, error_flag, msg) \
    do { \
        if ((ret) == (error_flag)) { \
            LOG_ERROR("%s: %s", msg, strerror(errno)); \
            exit(1); \
        } \
    } while (0)

/*
 * THREAD_ERROR_CHECK:
 * 功能：
 *   检查 pthread 系列函数的返回值。
 *
 * 为什么要单独写？
 *   因为 pthread_create/pthread_mutex_lock 这类函数失败时，
 *   往往不是返回 -1，而是直接返回“错误码本身”。
 */
#define THREAD_ERROR_CHECK(ret, msg) \
    do { \
        if (0 != (ret)) { \
            LOG_ERROR("%s: %s", msg, strerror(ret)); \
            exit(1); \
        } \
    } while (0)

#endif
