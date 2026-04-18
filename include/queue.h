#ifndef _QUEUE_H_
#define _QUEUE_H_

/*
 * node_t:
 * 单链表节点。
 * 每个节点里保存一个待处理的客户端 fd。
 */
typedef struct node_s {
    int fd;                 /* 客户端连接 fd */
    struct node_s *pNext;   /* 指向下一个节点 */
} node_t;

/*
 * queue_t:
 * 一个最简单的链式队列。
 *
 * 成员说明：
 *   head - 队头，出队时从这里拿
 *   end  - 队尾，入队时往这里挂
 *   size - 当前队列里元素个数
 */
typedef struct queue_s {
    node_t *head;
    node_t *end;
    int size;
} queue_t;

/*
 * enQueue:
 * 功能：
 *   把一个客户端 fd 放入队列尾部。
 *
 * 返回值：
 *   当前实现固定返回 0。
 */
int enQueue(queue_t *pQueue, int fd);

/*
 * deQueue:
 * 功能：
 *   从队头取出一个客户端 fd。
 *
 * 返回值：
 *   成功时返回真实 fd；
 *   如果队列为空，返回 -1。
 */
int deQueue(queue_t *pQueue);

#endif
