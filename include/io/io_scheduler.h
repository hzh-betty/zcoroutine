#ifndef ZCOROUTINE_IO_SCHEDULER_H_
#define ZCOROUTINE_IO_SCHEDULER_H_

#include <memory>
#include <atomic>
#include <vector>
#include "scheduling/scheduler.h"
#include "io/epoll_poller.h"
#include "io/fd_context.h"
#include "timer/timer_manager.h"
#include "util/noncopyable.h"
#include "sync/rw_mutex.h"

namespace zcoroutine {

/**
 * @brief IO调度器
 * 
 * 组合Scheduler、EpollPoller和TimerManager，提供IO事件调度功能。
 * 使用组合而非继承的设计模式，职责更加清晰。
 */
class IoScheduler : public NonCopyable {
public:
    using ptr = std::shared_ptr<IoScheduler>;

    IoScheduler(int thread_count, const std::string &name);
    /**
     * @brief 析构函数
     */
    ~IoScheduler();
    
    /**
     * @brief 启动IO调度器
     */
    void start();
    
    /**
     * @brief 停止IO调度器
     */
    void stop();
    
    /**
     * @brief 调度协程
     * @param fiber 协程指针
     */
    void schedule(Fiber::ptr fiber);
    
    /**
     * @brief 调度函数
     * @param func 函数对象
     */
    void schedule(std::function<void()> func);
    
    /**
     * @brief 添加IO事件
     * @param fd 文件描述符
     * @param event 事件类型（FdContext::kRead或kWrite）
     * @param callback 事件回调函数
     * @return 成功返回0，失败返回-1
     */
    int add_event(int fd, FdContext::Event event, std::function<void()> callback = nullptr);
    
    /**
     * @brief 删除IO事件
     * @param fd 文件描述符
     * @param event 事件类型
     * @return 成功返回0，失败返回-1
     */
    int del_event(int fd, FdContext::Event event);
    
    /**
     * @brief 取消IO事件
     * @param fd 文件描述符
     * @param event 事件类型
     * @return 成功返回0，失败返回-1
     */
    int cancel_event(int fd, FdContext::Event event);

    /**
     * @brief 取消文件描述符上的所有事件
     * @param fd 文件描述符
     * @return 成功返回0，失败返回-1
     */
    int cancel_all(int fd);
    
    /**
     * @brief 添加定时器
     * @param timeout 超时时间（毫秒）
     * @param callback 定时器回调
     * @param recurring 是否循环
     * @return 定时器指针
     */
    Timer::ptr add_timer(uint64_t timeout, std::function<void()> callback, bool recurring = false);

    /**
     * @brief 添加条件定时器
     * @param timeout 超时时间（毫秒）
     * @param callback 定时器回调
     * @param weak_cond 弱引用条件
     * @param recurring 是否循环
     * @return 定时器指针
     */
    Timer::ptr add_condition_timer(uint64_t timeout, std::function<void()> callback,
                                   std::weak_ptr<void> weak_cond, bool recurring = false);
    
    /**
     * @brief 获取Scheduler
     */
    Scheduler::ptr scheduler() const { return scheduler_; }
    
    /**
     * @brief 获取TimerManager
     */
    TimerManager::ptr timer_manager() const { return timer_manager_; }
    
    /**
     * @brief 获取单例
     */
    static ptr GetInstance();

    /**
     * @brief 获取当前线程的IoScheduler
     * @return 当前线程的IoScheduler指针
     */
    static IoScheduler* get_this();

private:
    /**
     * @brief IO线程运行函数
     */
    void io_thread_func();
    
    /**
     * @brief 唤醒IO线程
     */
    void wake_up() const;

    // 获取/创建 fd 对应的 FdContext（epoll 事件上下文）
    FdContext::ptr get_fd_context(int fd, bool auto_create);

private:
    Scheduler::ptr scheduler_;              // 任务调度器
    EpollPoller::ptr epoll_poller_;         // Epoll封装
    TimerManager::ptr timer_manager_;       // 定时器管理器

    std::vector<FdContext::ptr> fd_contexts_;

    std::unique_ptr<std::thread> io_thread_;  // IO线程
    std::atomic<bool> stopping_;              // 停止标志

    int wake_fd_[2]{};                          // 用于唤醒epoll的管道

    static IoScheduler::ptr s_instance_;      // 单例
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_IO_SCHEDULER_H_
