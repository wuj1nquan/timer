#ifndef _MARK_TIMEWHEEL_
#define _MARK_TIMEWHEEL_
#include <time.h>
#include <stdint.h>
#include <stddef.h>


typedef struct timer_node timer_node_t;
typedef void (*handler_pt) (struct timer_node *node);
struct timer_node {
    struct timer_node *next;
    uint32_t expire;
    handler_pt callback;
    uint8_t cancel;
};


void init_timer(void);
timer_node_t* add_timer(int time, handler_pt func);
void del_timer(timer_node_t *node);
void check_timer(int *stop);
void clear_timer();
time_t now_time();

#endif