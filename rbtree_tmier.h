#ifndef _MARK_RBT_
#define _MARK_RBT_

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#else
#include <time.h>
#endif

#include "rbtree.h"
#include <linux/time.h>

ngx_rbtree_t              timer;
static ngx_rbtree_node_t  sentinel;

typedef struct timer_entry_s timer_entry_t;
typedef void (*timer_handler_pt)(timer_entry_t *ev);

struct timer_entry_s {
    ngx_rbtree_node_t rbnode;
    timer_handler_pt handler;
};


static uint32_t current_time() {
    uint32_t t;
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
    clock_gettime(CLOCK_MONOTONIC, &ti);
    t = (uint32_t)ti.tv_sec * 1000;
    t += ti.tv_nsec / 1000000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    t = (uint32_t)tv.tv_sec * 1000;
    t += tv.tv_usec / 1000;
#endif
    return t;
}


ngx_rbtree_t* init_timer() {
    ngx_rbtree_init(&timer, &sentinel, ngx_rbtree_insert_value);
    return &timer;
}

timer_entry_t* add_timer(uint32_t msec, timer_handler_pt func) {  // 向红黑树添加一个定时任务，指定 超时时间 和任务的 回调函数 
    timer_entry_t *te = (timer_entry_t *)malloc(sizeof(*te));
    memset(te, 0, sizeof(*te));
    
    te->handler = func;
    msec += current_time();
    printf("add_timer expire at msec = %u\n", msec);
    te->rbnode.key = msec;
    ngx_rbtree_insert(&timer, &te->rbnode);

    return te;
}


void del_timer(timer_entry_t *te) {
    ngx_rbtree_delete(&timer, &te->rbnode);
    free(te);
}


int find_nearst_expire_timer() {
    ngx_rbtree_node_t *node;
    if (timer.root == &sentinel) {
        return -1;
    }
    node = ngx_rbtree_min(timer.root, timer.sentinel);
    int diff = (int)node->key - (int)current_time();

    return diff > 0 ? diff : 0;
}


void expire_timer() {
    timer_entry_t *te;
    ngx_rbtree_node_t *sentinel, *root, *node;
    sentinel = timer.sentinel;
    uint32_t now = current_time();
    while (1) {
        root = timer.root;
        if (root == sentinel) break;
        node = ngx_rbtree_min(root, sentinel);
        if (node->key > now) break;
        printf("touch timer expire time=%u, now = %u\n", node->key, now);
        te = (timer_entry_t *) ((char *)node - offsetof(timer_entry_t, rbnode));
        te->handler(te);
        ngx_rbtree_delete(&timer, &te->rbnode);
        free(te);
    }
}

#endif