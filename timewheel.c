#include "spinlock.h"
#include "timewheel.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#else
#include <time.h>
#endif

typedef struct link_list { // 链表结构体
    timer_node_t head;  // 链表头节点
    timer_node_t *tail; // 链表尾节点的地址
} link_list_t;

typedef struct timer {  // 定时器结构体
    link_list_t near[TIME_NEAR]; // 最小精度时间轮
    link_list_t t[4][TIME_LEVEL]; // 四层时间轮
    struct spinlock lock;
    uint32_t time;    // 定时器内部时间
    uint64_t current; 
    uint64_t current_point; // 系统时间（已过期）
} s_timer_t;


static s_timer_t * TI = NULL;   // 全局定时器


timer_node_t * link_clear(link_list_t *list) { // 取出当前链表（先存副本再移除）
    timer_node_t *ret = list->head.next;
    list->head.next = 0;      // 头节点head作占位符，实际第一个节点在head.next的位置
    list->tail = &(list->head);

    return ret;
}


void link(link_list_t *list, timer_node_t *node) { // 尾插法，将新节点插入链表
    list->tail->next = node;
    list->tail = node;
    node->next = 0;
}


void add_node(s_timer_t *T, timer_node_t *node) {
    uint32_t time = node->expire; // 定时任务的绝对超时时间
    uint32_t current_time = T->time; // 定时器内部当前时间
    uint32_t msec = time - current_time; // 定时任务的相对超时时间

    if (msec < TIME_NEAR) { // [0，256） 2的8次幂
        link(&T->near[msec], node);
    } 
    else if (msec < (1 << (TIME_NEAR_SHIFT + TIME_LEVEL_SHIFT))) { // [256, 16384)   2的14次幂
        link(&T->t[0][((msec >> TIME_NEAR_SHIFT) & TIME_LEVEL_MASK)], node); // 先右移8位（除以256）再对64取模
    } 
    else if (msec < (1 << (TIME_NEAR_SHIFT + 2*TIME_LEVEL_SHIFT))) { // [16384, 1048576)  2的20次幂
        link(&T->t[1][(msec >> (TIME_NEAR_SHIFT + TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK], node); // 先右移14位 ， 再对64取模
    }
    else if (msec < (1 << (TIME_NEAR_SHIFT + 3*TIME_LEVEL_SHIFT))) { // [1048576, 67108864)  2的26次幂
        link(&T->t[2][(msec >> (TIME_NEAR_SHIFT + 2*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK], node); // 先右移20位 ， 再对64取模
    }
    else { // [67108864, 4294967295] 
        link(&T->t[2][(msec >> (TIME_NEAR_SHIFT + 3*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK], node); // 先右移26位 ， 再对64取模
    }
    // 实际上中间三个link的取模运算都是多余的
}


timer_node_t * add_timer(int time, handler_pt func, int threadid) { // 添加一个定时任务
    
    timer_node_t *node = (timer_node_t *)malloc(sizeof(*node));
    spinlock_lcok(&TI->lock);
    node->expire = time+TI->time;
    node->callback = func;
    node->id = threadid;

    if (time <= 0) {  // 如果是立即执行的任务，则立即执行
        spinlock_unlock(&TI->lock);
        node->callback(node);
        free(node);
        return NULL;
    }
    add_node(TI, node);
    spinlock_unlock(&TI->lock);
    
    return node;
}


void move_list(s_timer_t *T, int level, int idx) { // 更新一个链表所有节点的位置
    timer_node_t *current = link_clear(&T->t[level][idx]);
    while (current) {
        timer_node_t *temp = current->next;
        add_node(T, current);
        current = temp;
    }
}


void timer_shift(s_timer_t *T) {  // 推进时间轮内部时间增长
    
    int mask = TIME_NEAR;
    uint32_t ct = ++T->time; // ct是当前时间，然后将定时器内部时间 + 1
    if (ct == 0) {  // 时间轮循环了一整圈，约 12.4 天
        move_list(T, 3, 0);
    } else {  // 每256秒检查一次是否需要重新映射节点
        uint32_t time = ct >> TIME_NEAR_SHIFT; // 除以256
        int i = 0;
        // 每2的8次幂（256）、14次幂、20次幂、26次幂 重新映射一次节点
        while ((ct & (mask-1)) == 0) { // 对2的8次幂、14次幂、20次幂、26次幂取模
            int idx = time & TIME_LEVEL_MASK; // 对64取模
            if (idx != 0) {
                move_list(T, i, idx); // 重新映射节点
                break;
            }
            mask <<= TIME_LEVEL_SHIFT;
            time >>= TIME_LEVEL_SHIFT;
            ++i;
        }
    }
}


void dispath_list(timer_node_t *current) { // 执行一个链表的任务
    do {
        timer_node_t *temp = current;
        current = current->next;
        if (temp->cancel == 0)
            temp->callback(temp);
        free(temp);
    } while (current);
}


void timer_execute(s_timer_t *T) {  //  执行最小精度时间轮near的一个任务链表
    int idx = T->time & TIME_NEAR_MASK;

    while (T->near[idx].head.next) {
        timer_node_t *current = link_clear(&T->near[idx]);
        spinlock_unlock(&T->lock);
        dispath_list(current);
        spinlock_lock(&T->lock);
    }
}


void timer_update(s_timer_t *T) {  
    spinlock_lock(&T->lock);
    timer_execute(T);   // 执行当前槽中所有节点
    timer_shift(T);     // 将时间轮推进一个单位时间，并将需要重新映射的节点移到合适的时间槽中
    timer_execute(T);   // 处理由于时间轮转动后可能落入当前时间槽的新定时器节点
    spinlock_unlock(&T->lock);
}


void del_timer(timer_node_t *node) { // 删除一个任务节点，这个任务会被删除而不执行
    node->cancel = 1;
}


s_timer_t* timer_create_timer() {
    
    s_timer_t *r = (s_timer_t *)malloc(sizeof(s_timer_t));
    memset(r, 0, sizeof(*r));
    
    int i, j;
    for (i = 0; i < TIME_NEAR; i++) {
        link_clear(&r->near[i]);
    }
    for (i = 0; i < 4; i++) {
        for(j = 0; j < 4; j++) {
            link_clear(&r->t[i][j]);
        }
    }
    spinlock_init(&r->lock);
    r->current = 0;

    return r;
}


uint64_t gettime() {  // 获取系统时间
    uint64_t t;
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 1000;
	t += ti.tv_nsec / 1000000;
	// 1ns = 1/1000000000 s = 1/1000000 ms
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif

    return t;
}


void expire_timer(void) {   // 以系统时间为参照，推动定时器
    uint64_t cp = gettime();
    if (cp != TI->current_point) {
        uint32_t diff = (uint32_t)(cp - TI->current_point); // 距离上一次更新的时长
        TI->current_point = cp;
        int i;
        for (i = 0; i < diff; i++) { // 补偿时差
            timer_update(TI);
        }
    }
}


void 
init_timer(void) {
	TI = timer_create_timer();
	TI->current_point = gettime();
}


void clear_timer() {   // 销毁定时器
    int i, j;
    for (i = 0; i < TIME_NEAR; i++) {  // 遍历释放near所有的链表的节点空间
        link_list_t *list = &TI->near[i];
        timer_node_t *current = list->head.next;
        while (current) {
            timer_node_t *temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->near[i]); // 避免悬空指针
    }
    for (i = 0; i < 4; i++) {   // 遍历释放二维指针数组t的所有链表空间
        for (j = 0; j < TIME_LEVEL; j++) {
            link_list_t *list = &TI->t[i][j];
            timer_node_t *current = list->head.next;
            while (current) {
                timer_node_t *temp = current;
                current = current->next;
                free(temp);
            }
            link_clear(&TI->t[i][j]);
        }
    }
}



