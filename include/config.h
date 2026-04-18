#ifndef __CONFIG_H__
#define __CONFIG_H__

/*
 * get_target:
 * 功能：
 *   从配置文件 config/config.ini 中按 key 查找对应 value。
 *
 * 例如配置文件内容：
 *   ip=127.0.0.1
 *   port=9090
 *   log=INFO
 *
 * 调用方式：
 *   char ip[64] = {0};
 *   get_target("ip", ip);
 *
 * 入参：
 *   key   - 要查找的键
 *   value - 输出缓冲区，用于接收配置值
 *
 * 返回值：
 *   0  表示找到并成功拷贝
 *  -1 表示配置文件不存在或 key 不存在
 */
int get_target(char *key, char *value);

#endif
