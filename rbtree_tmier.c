#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include "rbtree_tmier.h"


void hello_world(timer_entry_t *te) {
    printf("hello world, time = %u\n", te->rbnode.key);
}


int main() {
    init_timer();

    add_timer(3000, hello_world);

    int epfd = epoll_create(1);
    struct epoll_event events[512];

    while (1) {
        int nearst = find_nearst_expire_timer();
        int n = epoll_wait(epfd, events, 512, nearst);
        
        for (int i = 0; i < n; i++) {
            // 
        }
        expire_timer();
    }

    return 0;
}