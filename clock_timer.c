/**
 *   it's too damn hard
 * 
 *   该定时器有三个时间概念：1.定时器维护的内部时间time，手动增加
 *                          2.系统时间，自动增加
 *                          3.定时任务的超时时间，手动设定
 *   
 * 判断定时任务是否超时的依据是 定时器的内部时间time，系统时间只用作定时器内部时间推进的参考
 */


#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_timer.h"
#include <linux/time.h>

#include "spinlock.h"


#define SECONDS 60
#define MINUTES 60
#define HOURS   12
#define ONE_HOUR 3600
#define ONE_MINUTE 60
#define HALF_DAY 43200 // 12*3600


typedef struct link_list {
    timer_node_t head;
    timer_node_t *tail;
} link_list_t;

typedef struct timer {
    link_list_t second[SECONDS];
    link_list_t minute[MINUTES];
    link_list_t hour[HOURS];
    spinlock_t lock;
    uint32_t time;
    time_t current_point;       
} timer_st;

static timer_st * TI = NULL;

static timer_node_t * link_clear(link_list_t *list) {
    timer_node_t * ret = list->head.next;
    list->head.next = 0;
    list->tail = &(list->head);

    return ret;
}

static void link_to(link_list_t *list, timer_node_t *node) {
    list->tail->next = node;
    list->tail = node;
    node->next = 0;
}

static void add_node(timer_st *T, timer_node_t *node) {
    uint32_t time = node->expire;
    uint32_t current_time = T->time;
    uint32_t mesc = time - current_time;
    if (mesc < ONE_MINUTE) {
        link_to(&T->second[time % SECONDS], node);
    } else if (mesc < ONE_HOUR) {
        link_to(&T->minute[(uint32_t)(time/ONE_MINUTE) % MINUTES], node);
    } else {
        link_to(&T->hour[(uint32_t)(time/ONE_HOUR) % HOURS], node);
    }
}


static void remap(timer_st *T, link_list_t *level, int idx) {
    timer_node_t *current = link_clear(&level[idx]);
    while (current) {
        timer_node_t *temp = current->next;
        add_node(T, current);
        current = temp;
    }
}

// 根据当前的时间推进定时器系统，并将任务从较高层级的时间轮槽（如分钟或小时槽）移动到较低层级的时间轮槽（如秒槽），确保定时器任务在正确的时间被触发
static void timer_shift(timer_st *T) { 
    uint32_t ct = ++T->time % HALF_DAY;  // 定时器的时间time + 1
    if (ct == 0) {  // 已经过去了12小时，43200秒
        remap(T, T->hour, 0); // 将 hour 槽中的所有定时器节点移动到较低层级的槽（分钟或秒）
    } else {  // 每分钟或每小时重映射分钟或小时槽
        if (ct % SECONDS == 0) { // 当前时间是否整分钟
            {
                uint32_t idx = (uint32_t)(ct / ONE_MINUTE) % MINUTES; 
                if (idx != 0) { // 表示当前时间不是整小时
                    remap(T, T->minute, idx); // 将重新映射到秒槽中
                    return;
                }
            }
            {
                uint32_t idx = (uint32_t)(ct / ONE_HOUR) % HOURS;
                if (idx != 0) { // 表示当前时间不是整天的开始
                    remap(T, T->hour, idx); // 将重新映射到分钟槽中
                }
            }
        }
    }
    /**
     * 1.每分钟将一个分钟槽的节点映射到秒槽中
     * 2.每小时将一个小时槽的节点映射到分钟槽中
     * 3.hour的0槽需要额外判断处理，因为最后一次hour槽的映射 uint32_t idx = (uint32_t)(ct / ONE_HOUR) % HOURS;没有包括超时时间是12小时的情况
     */
}


static void dispath_list(timer_node_t *current) {
    do {
        timer_node_t * temp = current;
        current = current->next;
        if (temp->cancel == 0)
            temp->callback(temp);
        free(temp);
    } while (current);
}


static void timer_execute(timer_st *T) {
    uint32_t idx = T->time % SECONDS;   // 每一次执行最小时间单位槽-->秒 中的定时器任务

    while (T->second[idx].head.next) {
        timer_node_t *current = link_clear(&T->second[idx]);
        spinklock_unlock(&T->lock);
        dispatch_list(current);
        spinlock_lock(&T->lock);
    }
}


static void timer_update(timer_st *T) {
    spinlock_lock(&T->lock);
    timer_execute(T);
    timer_shift(T);
    timer_execute(T);
    spinlock_unlock(&T->lock);
}


static timer_st * craete_timer() {
    timer_st *r = (timer_st *)malloc(sizeof(timer_st));
    memset(r, 0, sizeof(*r));

    int i;
    for(i = 0; i < SECONDS; i++) {
        link_clear(&r->second[i]);
    }
    for(i = 0; i < SECONDS; i++) {
        link_clear(&r->minute[i]);
    }
    for(i = 0; i < SECONDS; i++) {
        link_clear(&r->hour[i]);
    }

    spinlock_init(&r->lock);

    r->time = 0;

    return r;
}


void init_timer(void) {
    TI = create_timer();
    TI->current_point = now_time();
}


timer_node_t *add_timer(int time, handler_pt func) {
    timer_node_t *node = (timer_node_t *)malloc(sizeof(*node));
    spinlock_lock(&TI->lock);
    node->expire = time + TI->time;
    
    ptinrf("add timer at %u, expire at %u, now_time at %lu\n", TI->time, node->expire, now_time());

    node->callback = func;
    node->cancel = 0;
    if (time <= 0) {
        spinlock_unlock(&TI-> lock);
        node->callback(node);
        free(node);

        return NULL;
    }
    add_node(TI, node);
    spinlock_unlock(&TI->lock);

    return node;
}


void del_timer(timer_node_t *node) {
    node->cancel = 1;
}

void check_timer(int *stop) {  //  同步系统时间和定时器的当前时间
    while (*stop == 0) {
        time_t cp = now_time();
        if (cp != TI->current_point) {
            uint32_t diff = (uint32_t)(cp - TI->current_point);
            TI->current_point = cp;
            int i;
            for (i = 0; i < diff; i++) {  // 推进定时器，补偿时间差
                timer_update(TI);
            }
        }
        usleep(200000);
    }
}


void clear_timer() {
    int i;
    for (i = 0; i < SECONDS; i++) {
        link_list_t *list = &TI->second[i];
        timer_node_t *current = list->head.next;
        while (current) {
            timer_node_t *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->second[i]);
    }
    for (i = 0; i < MINUTES; i++) {
        link_list_t * list = &TI->minute[i];
        timer_node_t *current = list->head.next;
        while(current) {
            timer_node_t *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->hour[i]);
    }
    for (i = 0; i < HOURS; i++) {
        link_list_t * list = &TI->hour[i];
        timer_node_t *current = list->head.next;
        while (current) {
            timer_node_t *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->hour[i]);
    }
}


time_t now_time() {
    struct timespec ti;
    clock_gettime(CLOCK_MONOTONIC, &ti);

    return ti.tv_sec;
}



#if 0
/* 这是chatgpt做的优化 :
主要修改点
T->time 的初始化:

在 create_timer 函数中添加了 T->time 的初始化为 0。
Remap 逻辑的优化:

timer_shift 函数中的逻辑已经优化，确保了小时槽在12小时和非整点时正确重新映射。

*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "spinlock.h"

#define SECONDS 60
#define MINUTES 60
#define HOURS 12
#define ONE_HOUR 3600
#define ONE_MINUTE 60
#define HALF_DAY 43200 // 12*3600

typedef void (*handler_pt)(struct timer_node *);

struct timer_node {
    struct timer_node *next;
    uint32_t expire;
    handler_pt callback;
    uint8_t cancel;
};

typedef struct link_list {
    struct timer_node head;
    struct timer_node *tail;
} link_list_t;

typedef struct timer {
    link_list_t second[SECONDS];
    link_list_t minute[MINUTES];
    link_list_t hour[HOURS];
    spinlock_t lock;
    uint32_t time;
    time_t current_point;
} timer_st;

static timer_st *TI = NULL;

static struct timer_node *
link_clear(link_list_t *list) {
    struct timer_node *ret = list->head.next;
    list->head.next = NULL;
    list->tail = &(list->head);
    return ret;
}

static void
link_to(link_list_t *list, struct timer_node *node) {
    list->tail->next = node;
    list->tail = node;
    node->next = NULL;
}

static void
add_node(timer_st *T, struct timer_node *node) {
    uint32_t time = node->expire;
    uint32_t current_time = T->time;
    uint32_t msec = time - current_time;
    if (msec < ONE_MINUTE) {
        link_to(&T->second[time % SECONDS], node);
    } else if (msec < ONE_HOUR) {
        link_to(&T->minute[(time / ONE_MINUTE) % MINUTES], node);
    } else {
        link_to(&T->hour[(time / ONE_HOUR) % HOURS], node);
    }
}

static void
remap(timer_st *T, link_list_t *level, int idx) {
    struct timer_node *current = link_clear(&level[idx]);
    while (current) {
        struct timer_node *temp = current->next;
        add_node(T, current);
        current = temp;
    }
}

static void
timer_shift(timer_st *T) {
    uint32_t ct = ++T->time % HALF_DAY;
    if (ct % SECONDS == 0) {  // 当前时间为整分钟
        // 每分钟重新分配一次
        uint32_t minute_idx = (ct / ONE_MINUTE) % MINUTES;
        if (minute_idx != 0) {
            remap(T, T->minute, minute_idx);
        }

        // 每小时重新分配一次
        if (ct % ONE_HOUR == 0) {
            uint32_t hour_idx = (ct / ONE_HOUR) % HOURS;
            remap(T, T->hour, hour_idx);
        }
    }
}


static void
dispatch_list(struct timer_node *current) {
    while (current) {
        struct timer_node *temp = current;
        current = current->next;
        if (!temp->cancel) {
            temp->callback(temp);
        }
        free(temp);
    }
}

static void
timer_execute(timer_st *T) {
    uint32_t idx = T->time % SECONDS;
    while (T->second[idx].head.next) {
        struct timer_node *current = link_clear(&T->second[idx]);
        spinlock_unlock(&T->lock);
        dispatch_list(current);
        spinlock_lock(&T->lock);
    }
}

static void
timer_update(timer_st *T) {
    spinlock_lock(&T->lock);
    timer_execute(T);
    timer_shift(T);
    timer_execute(T);
    spinlock_unlock(&T->lock);
}

static timer_st *
create_timer() {
    timer_st *r = (timer_st *)malloc(sizeof(timer_st));
    memset(r, 0, sizeof(*r));
    
    r->time = 0;  // 初始化 time 为 0
    r->current_point = now_time(); // 初始化 current_point 为当前系统时间

    for (int i = 0; i < SECONDS; i++) {
        link_clear(&r->second[i]);
    }
    for (int i = 0; i < MINUTES; i++) {
        link_clear(&r->minute[i]);
    }
    for (int i = 0; i < HOURS; i++) {
        link_clear(&r->hour[i]);
    }
    spinlock_init(&r->lock);
    return r;
}

void
init_timer(void) {
    TI = create_timer();
}

struct timer_node *
add_timer(int time, handler_pt func) {
    struct timer_node *node = (struct timer_node *)malloc(sizeof(*node));
    spinlock_lock(&TI->lock);
    node->expire = time + TI->time;
    printf("add timer at %u, expire at %u, now_time at %lu\n", TI->time, node->expire, now_time());
    node->callback = func;
    node->cancel = 0;
    if (time <= 0) {
        spinlock_unlock(&TI->lock);
        node->callback(node);
        free(node);
        return NULL;
    }
    add_node(TI, node);
    spinlock_unlock(&TI->lock);
    return node;
}

void
del_timer(struct timer_node *node) {
    node->cancel = 1;
}

void
check_timer(int *stop) {
    while (*stop == 0) {
        time_t cp = now_time();
        if (cp != TI->current_point) {
            uint32_t diff = (uint32_t)(cp - TI->current_point);
            TI->current_point = cp;
            for (uint32_t i = 0; i < diff; i++) {
                timer_update(TI);
            }
        }
        usleep(200000); // 200ms
    }
}

void
clear_timer() {
    for (int i = 0; i < SECONDS; i++) {
        link_list_t *list = &TI->second[i];
        struct timer_node *current = list->head.next;
        while (current) {
            struct timer_node *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->second[i]);
    }
    for (int i = 0; i < MINUTES; i++) {
        link_list_t *list = &TI->minute[i];
        struct timer_node *current = list->head.next;
        while (current) {
            struct timer_node *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->minute[i]);
    }
    for (int i = 0; i < HOURS; i++) {
        link_list_t *list = &TI->hour[i];
        struct timer_node *current = list->head.next;
        while (current) {
            struct timer_node *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->hour[i]);
    }
}

time_t
now_time() {
    struct timespec ti;
    clock_gettime(CLOCK_MONOTONIC, &ti);
    return ti.tv_sec;
}

#else
#endif