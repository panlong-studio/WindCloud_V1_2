#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "log.h"

/*
 * get_target:
 * 从配置文件中查找 key 对应的 value。
 *
 * 当前配置文件路径固定为：
 *   ./config/config.ini
 *
 * 每一行格式类似：
 *   ip=127.0.0.1
 *   port=9090
 *   log=INFO
 */
int get_target(char *key, char *value) {
    FILE *file = fopen("./config/config.ini", "r");

    /*
     * 如果连配置文件都打不开，说明程序环境不完整，
     * 这里直接返回 -1，让上层决定是否使用默认值。
     */
    if (file == NULL) {
        LOG_WARN("config.ini is NULL");
        return -1;
    }

    /*
     * line 用来逐行读取配置。
     * 100 字节不算很大，但对当前这种简单配置文件已经够用。
     */
    char line[100];

    while (fgets(line, sizeof(line), file)) {
        /*
         * fgets 会把换行符一起读进来，
         * 所以这里先把 \r / \n 去掉，方便后续比较。
         */
        line[strcspn(line, "\r\n")] = '\0';

        /* 空行没有处理意义，直接跳过。 */
        if (line[0] == '\0') {
            continue;
        }

        /*
         * 以 # 或 ; 开头的行视为注释行。
         * 这是配置文件里常见的约定。
         */
        if (line[0] == '#' || line[0] == ';') {
            continue;
        }

        /*
         * strtok(line, "=") 会把这一行按 '=' 拆成两段：
         *   line_key   -> 左边的键
         *   line_value -> 右边的值
         *
         * 注意：
         * strtok 会修改原字符串，所以它适合用在 line 这种临时缓冲区上，
         * 不适合直接用在只读字符串常量上。
         */
        char *line_key = strtok(line, "=");
        if (line_key != NULL && strcmp(key, line_key) == 0) {
            char *line_value = strtok(NULL, "=");
            if (line_value != NULL) {
                /*
                 * 找到目标 key 后，把 value 拷贝给调用方。
                 * 这里假设调用方提供的缓冲区足够大。
                 */
                strcpy(value, line_value);
                LOG_INFO("config loaded key=%s value=%s", key, value);
                fclose(file);
                return 0;
            }
        }
    }

    fclose(file);
    LOG_WARN("config key not found key=%s", key);
    return -1;
}
