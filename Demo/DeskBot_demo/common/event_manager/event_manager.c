#include "event_manager.h"
#include <stdlib.h>
#include <string.h>

/*
    初始化事件管理器和事件队列
    -指向的 EventManager 结构体的指针
    -队列容量（最大可存储的事件数）
*/
void event_manager_init(EventManager *manager, size_t queue_capacity) {
    manager->event_queue.events = (AppMessage *)malloc(queue_capacity * sizeof(AppMessage));    // 动态分配内存给事件队列
    manager->event_queue.capacity = queue_capacity; 
    manager->event_queue.head = 0;
    manager->event_queue.tail = 0;
    pthread_mutex_init(&manager->event_queue.mutex, NULL);  // 初始化互斥锁
    pthread_cond_init(&manager->event_queue.cond, NULL);    // 初始化条件变量

    for (int i = 0; i < GLOBAL_EVENT_MAX; ++i) {
        manager->handlers[i] = NULL;
    }
}

// 清理事件管理器资源
void event_manager_deinit(EventManager *manager) {
    if (manager->event_queue.events) {
        free(manager->event_queue.events);
        manager->event_queue.events = NULL;
    }
    pthread_mutex_destroy(&manager->event_queue.mutex);
    pthread_cond_destroy(&manager->event_queue.cond);
}

// 注册事件处理函数
bool event_manager_register_handler(EventManager *manager, AppEventType type, EventHandler handler) {
    if (type >= GLOBAL_EVENT_MAX) return false;

    pthread_mutex_lock(&manager->event_queue.mutex);    // 锁定互斥锁，防止并发访问
    manager->handlers[type] = handler;                  // 将处理函数赋值给对应事件的回调处理函数
    pthread_mutex_unlock(&manager->event_queue.mutex);  // 解锁互斥锁

    return true;
}

// 发送事件到队列
bool event_manager_send_event(EventManager *manager, AppEventType type, void *data) {
    pthread_mutex_lock(&manager->event_queue.mutex);
    if ((manager->event_queue.tail + 1) % manager->event_queue.capacity == manager->event_queue.head) {
        pthread_mutex_unlock(&manager->event_queue.mutex);
        return false;  // 队列已满
    }
    manager->event_queue.events[manager->event_queue.tail].type = type;     // 将事件类型赋值给队列尾部的事件
    manager->event_queue.events[manager->event_queue.tail].data = data;     // 将事件数据赋值给队列尾部的事件
    manager->event_queue.tail = (manager->event_queue.tail + 1) % manager->event_queue.capacity;    // 更新队列尾部索引，循环使用队列
    pthread_cond_signal(&manager->event_queue.cond);  // 唤醒等待的线程
    pthread_mutex_unlock(&manager->event_queue.mutex);
    return true;
}

// 从队列中取出并分发事件给相应的处理函数（将队列中所有时间全部处理）
void event_manager_dispatch_events(EventManager *manager) {
    pthread_mutex_lock(&manager->event_queue.mutex);
    while (manager->event_queue.head != manager->event_queue.tail) {
        AppMessage msg = manager->event_queue.events[manager->event_queue.head];    // 取出队列头部的事件
        manager->event_queue.head = (manager->event_queue.head + 1) % manager->event_queue.capacity;    

        if (msg.type < GLOBAL_EVENT_MAX && manager->handlers[msg.type]) {
            manager->handlers[msg.type](msg.data);  // 调用处理函数
        } else {
            printf("Unhandled or invalid event type: %d\n", msg.type);
        }
    }
    pthread_mutex_unlock(&manager->event_queue.mutex);
}
