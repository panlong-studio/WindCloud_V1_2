#include "log.h"

/*
 * g_log_level:
 * 当前生效的全局日志级别。
 * 默认设为 INFO，意味着 INFO/WARN/ERROR 会输出，DEBUG 默认不输出。
 */
static int g_log_level = LOG_LEVEL_INFO;

/*
 * g_log_fp:
 * 全局日志文件指针。
 * 如果没有显式指定日志文件，就会退回 stdout。
 */
static FILE *g_log_fp = NULL;

/*
 * g_log_mutex:
 * 日志输出互斥锁。
 *
 * 为什么日志也要加锁？
 * 因为当前服务端是多线程的。
 * 如果多个线程同时 fprintf 到同一个日志文件，输出很容易相互穿插，导致日志不可读。
 */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * level_to_int:
 * 把 "DEBUG"/"INFO"/"WARN"/"ERROR" 这种字符串转换成内部整型级别。
 */
static int level_to_int(const char *level_str) {
    if (strcasecmp(level_str, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(level_str, "INFO") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(level_str, "WARN") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(level_str, "ERROR") == 0) return LOG_LEVEL_ERROR;

    /* 无法识别时，默认回到 INFO。 */
    return LOG_LEVEL_INFO;
}

/*
 * level_to_name:
 * 把内部整型级别转回字符串，供最终日志输出时显示。
 */
static const char *level_to_name(int level) {
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

int init_log(const char *level_str, const char *log_file) {
    /*
     * 初始化日志时先加锁，
     * 避免多个线程同时初始化日志目标。
     */
    pthread_mutex_lock(&g_log_mutex);

    /* 如果调用方传入了日志级别字符串，就更新全局级别。 */
    if (level_str) {
        g_log_level = level_to_int(level_str);
    }

    if (log_file) {
        /*
         * 如果之前已经打开过文件，并且不是 stdout，
         * 这里先关掉旧文件，避免句柄泄漏。
         */
        if (g_log_fp && g_log_fp != stdout) {
            fclose(g_log_fp);
        }

        /*
         * 以追加模式打开文件。
         * 追加模式的好处是：不会把旧日志清空，方便持续追踪程序行为。
         */
        g_log_fp = fopen(log_file, "a");
        if (!g_log_fp) {
            /*
             * 即使文件打开失败，也尽量退回 stdout，
             * 保证日志模块至少还能工作，而不是直接空指针崩溃。
             */
            g_log_fp = stdout;
            pthread_mutex_unlock(&g_log_mutex);
            perror("Failed to open log file");
            return -1;
        }
    } else {
        /* 没有日志文件时，默认输出到控制台。 */
        g_log_fp = stdout;
    }

    pthread_mutex_unlock(&g_log_mutex);
    return 0;
}

void close_log(void) {
    pthread_mutex_lock(&g_log_mutex);

    /*
     * 只有当 g_log_fp 真的是一个普通文件时，才需要 fclose。
     * 如果它是 stdout，就不应该手动关闭标准输出。
     */
    if (g_log_fp && g_log_fp != stdout) {
        fclose(g_log_fp);
    }

    /* 关闭后把日志目标重新指回 stdout，作为安全兜底。 */
    g_log_fp = stdout;

    pthread_mutex_unlock(&g_log_mutex);
    pthread_mutex_destroy(&g_log_mutex);
}

void log_write(int level, const char *file, int line, const char *func, const char *fmt, ...) {
    struct tm tm_info;
    FILE *fp = NULL;

    /*
     * 如果这条日志的级别低于当前配置级别，
     * 就直接忽略。例如当前级别是 INFO，则 DEBUG 不打印。
     */
    if (level < g_log_level) {
        return;
    }

    /*
     * 获取当前时间戳。
     * time(NULL) 返回的是“从 Unix 纪元到现在的秒数”。
     */
    time_t now = time(NULL);
    char time_str[64];

    /*
     * 先把用户传入的格式串和可变参数拼成最终日志正文。
     */
    char log_msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_msg, sizeof(log_msg), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);

    /*
     * 如果日志文件还没初始化成功，就退回 stdout。
     * 这样即使有人忘了调用 init_log，日志系统也尽量不崩。
     */
    fp = g_log_fp ? g_log_fp : stdout;

    /*
     * localtime_r 是线程安全版本。
     * 为什么不用 localtime？
     * 因为 localtime 返回的是静态共享区指针，多线程并发时可能互相覆盖。
     */
    localtime_r(&now, &tm_info);

    /*
     * 把时间结构格式化成字符串，便于直接写入日志。
     */
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);

    /*
     * 最终日志格式：
     * [时间] [级别] [文件:行号 函数名] 消息内容
     */
    fprintf(fp, "[%s] [%s] [%s:%d %s] %s\n",
            time_str,
            level_to_name(level),
            file,
            line,
            func,
            log_msg);

    /*
     * fflush 的作用是立刻把缓冲区内容刷到文件。
     * 好处是程序崩溃时，最近的日志不容易丢。
     * 代价是性能会略差，但对教学项目更利于调试。
     */
    fflush(fp);
    pthread_mutex_unlock(&g_log_mutex);
}
