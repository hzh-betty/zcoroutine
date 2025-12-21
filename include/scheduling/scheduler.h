#ifndef ZCOROUTINE_SCHEDULER_H_
#define ZCOROUTINE_SCHEDULER_H_

#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <functional>

#include "util/zcoroutine_logger.h"
#include "scheduling/task_queue.h"
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"

namespace zcoroutine {

/**
 * @brief 调度器类
 * 基于线程池的M:N调度模型
 * 使用std::thread和std::mutex，不再封装Thread/Mutex类
 */
class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;

    /**
     * @brief 构造函数
     * @param thread_count 线程数量
     * @param name 调度器名称
     */
    explicit Scheduler(int thread_count = 1, std::string  name = "Scheduler");

    /**
     * @brief 析构函数
     */
    virtual ~Scheduler();

    /**
     * @brief 获取调度器名称
     */
    const std::string& name() const { return name_; }

    /**
     * @brief 启动调度器
     */
    void start();

    /**
     * @brief 停止调度器
     * 等待所有任务执行完毕后停止
     */
    void stop();

    /**
     * @brief 调度协程
     * @param fiber 协程指针
     */
    void schedule(Fiber::ptr fiber);

    /**
     * @brief 模板方法：调度可调用对象
     * @tparam F 函数类型
     * @tparam Args 参数类型
     */
    template<class F, class... Args>
    void schedule(F&& f, Args&&... args) {
        auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        // 从协程池获取协程执行任务
        auto fiber = FiberPool::GetInstance()->acquire(std::move(func));
        Task task(fiber);
        task_queue_->push(task);

        ZCOROUTINE_LOG_DEBUG("Scheduler[{}] scheduled fiber from pool, name={}, id={}, queue_size={}",
                             name_, fiber->name(), fiber->id(), task_queue_->size());
    }

    /**
     * @brief 是否正在运行
     */
    bool is_running() const { return !stopping_.load(std::memory_order_relaxed); }

    /**
     * @brief 获取当前调度器（线程本地）
     */
    static Scheduler* get_this();

    /**
     * @brief 设置当前调度器（线程本地）
     */
    static void set_this(Scheduler* scheduler);

protected:
    /**
     * @brief 工作线程主循环
     * 初始化main_fiber和scheduler_fiber，然后启动调度
     */
    void run();

    /**
     * @brief 调度循环
     * 运行在scheduler_fiber中，负责调度和执行用户协程
     */
    void schedule_loop();

private:
    std::string name_;                              // 调度器名称
    int thread_count_;                              // 线程数量
    std::vector<std::unique_ptr<std::thread>> threads_;  // 线程池
    std::unique_ptr<TaskQueue> task_queue_;         // 任务队列
    
    std::atomic<bool> stopping_;                    // 停止标志
    std::atomic<int> active_thread_count_;          // 活跃线程数
    std::atomic<int> idle_thread_count_;            // 空闲线程数
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SCHEDULER_H_
