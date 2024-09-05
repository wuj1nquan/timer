#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <functional>
#include <chrono>
#include <set>
#include <memory>
#include <iostream>

using namespace std;

struct TimerNodeBase { //  定时器节点基类，用于红黑树（set）存储
    time_t expire;     //  超时时间
    uint64_t id;       //  唯一 id， 用于解决超时时间相同的节点存储问题
};

struct TimerNode : public TimerNodeBase {  // 子类定时器节点， 添加了一个回调函数
    using Callback = function<void(const TimerNode &node)>;
    Callback func;
    TimerNode(int64_t id, time_t expire, Callback func) : func(std::move(func)) { // 使用 move 右值引用，性能高
        this->expire = expire;
        this->id = id;
    }
};

bool operator < (const TimerNodeBase &lhd, const TimerNodeBase &rhd) { // 运算符重载，比较两个节点的大小
    // 先根据超时时间判定大小
    if (lhd.expire < rhd.expire) {
        return true;
    } else if (lhd.expire > rhd.expire) {
        return false;
    } // 超时时间相同时，根据 id 判断大小
    else return lhd.id < rhd.id;
}


class Timer {

public:
    static inline time_t GetTick() { // 获取系统当前时间戳
        return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count();
    }

    TimerNodeBase AddTimer(int msec, TimerNode::Callback func) {
        time_t expire = GetTick() + msec; // msec是相对超时时间，expire是绝对超时时间（时间戳）
        // 如果待插入节点当前不是红黑树中最大的
        if (timeouts.empty() || expire <= timeouts.crbegin()->expire) { 
            auto pairs = timeouts.emplace(GenID(), expire, std::move(func)); // emplace是在容器内部生成一个对象并插入到红黑树中，性能优于push的copy操作  2.使用move右值引用，避免copy
            // 使用static_cast将子类cast成基类
            return static_cast<TimerNodeBase>(*pairs.first); // emplace的返回值pair包含：1.创建并插入的节点 2.是否成功插入（已存在相同节点则插入失败）
        }
        // 如果待插入节点是最大的，直接插入到最右侧，时间复杂度 O(1) ，优化性能
        auto ele = timeouts.emplace_hint(timeouts.crbegin().base(), GenID(), expire, std::move(func));
       // 返回基类而不是子类
        return static_cast<TimerNodeBase>(*ele);
    }

    void DelTimer(TimerNodeBase &node) { // 从（set）红黑树中删除一个节点
        auto iter = timeouts.find(node); // 找到指定节点
        if (iter != timeouts.end())
            timeouts.erase(iter);       // 移除
    }
    
    void HandleTimer(time_t now) {     // 执行当前已超时的任务
        auto iter = timeouts.begin();
        while (iter != timeouts.end() && iter->expire <= now) {
            iter->func(*iter);
            iter = timeouts.erase(iter); // eraser返回下一个节点
        }
    }

public:
    // 更新 timerfd 的到期时间为 timeouts 集合中最早到期的定时器时间
    virtual void UpdateTimerfd(const int fd) {
        struct timespec abstime;
        auto iter = timeouts.begin();  // 最小超时时间节点
        if (iter != timeouts.end()) {
            abstime.tv_sec = iter->expire / 1000;
            abstime.tv_nsec = (iter->expire % 1000) * 1000000;
        } else {
            abstime.tv_sec = 0;
            abstime.tv_nsec = 0;
        }

        struct itimerspec its;
        its.it_interval = {};
        its.it_value = abstime;

        timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, nullptr);
    }

private:
    static inline uint64_t GenID() { // 生成一个 id
        return gid++;
    }
    static uint64_t gid; // 全局 id 变量

    set<TimerNode, std::less<> > timeouts; // less指定排序方式：从小到大
};

uint64_t Timer::gid = 0;


int main() {
    int epfd = epoll_create(1);  // epoll

    int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET};
    epoll_ctl(epfd, EPOLL_CTL_ADD, timerfd, &ev);

    unique_ptr<Timer> timer = make_unique<Timer>();
    int i = 0;
    timer->AddTimer(1000, [&](const TimerNode &node) {      //   lamda 表达式
        cout << Timer::GetTick() << "node id:" << node.id << "revoked times" << ++i << endl;
    });

    timer->AddTimer(1000, [&](const TimerNode &node) {
        cout << Timer::GetTick() << " node id:" << node.id << " revoked times:" << ++i << endl;
    });

    timer->AddTimer(3000, [&](const TimerNode &node) {
        cout << Timer::GetTick() << " node id:" << node.id << " revoked times:" << ++i << endl;
    });

    auto node = timer->AddTimer(2100, [&](const TimerNode &node) {
        cout << Timer::GetTick() << " node id:" << node.id << " revoked times:" << ++i << endl;
    });
    timer->DelTimer(node);

    cout << "now time:" << Timer::GetTick() << endl;

    struct epoll_event evs[64] = {0};
    while (true) {
        timer->UpdateTimerfd(timerfd);    // epoll中timerfd的到期时间
        int n = epoll_wait(epfd, evs, 64, -1); // 内核检测定时时间timerfd
        time_t now = Timer::GetTick();   // 当前系统时间戳
        
        for (int i = 0; i < n; i++) {     
            // for network event handle
        }
        timer->HandleTimer(now);         // 处理现在到期的定时任务
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, timerfd, &ev);
    close(timerfd);
    close(epfd);

    return 0;
}


