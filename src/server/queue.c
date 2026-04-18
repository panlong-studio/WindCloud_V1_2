#include <stdlib.h>

#include "queue.h"

/*
 * enQueue:
 * 把一个 fd 挂到队尾。
 */
int enQueue(queue_t *pQueue, int fd) {
    node_t *pNew = (node_t *)calloc(1, sizeof(node_t));
    pNew->fd = fd;

    /*
     * 如果当前队列为空，
     * 新节点同时成为 head 和 end。
     */
    if (pQueue->size == 0) {
        pQueue->head = pNew;
        pQueue->end = pNew;
    } else {
        /*
         * 否则挂到原队尾之后，并更新队尾指针。
         */
        pQueue->end->pNext = pNew;
        pQueue->end = pNew;
    }

    pQueue->size++;
    return 0;
}

/*
 * deQueue:
 * 从队头取出一个 fd。
 */
int deQueue(queue_t *pQueue) {
    if (pQueue->size == 0) {
        return -1;
    }

    node_t *p = pQueue->head;
    int fd = p->fd;

    /*
     * 队头往后移动一格。
     */
    pQueue->head = p->pNext;

    /*
     * 如果原来只有 1 个元素，出队后队尾也要置空。
     */
    if (pQueue->size == 1) {
        pQueue->end = NULL;
    }

    pQueue->size--;
    free(p);
    return fd;
}
