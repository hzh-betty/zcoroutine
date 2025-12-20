#ifndef ZCOROUTINE_THREAD_CONTEXT_H_
#define ZCOROUTINE_THREAD_CONTEXT_H_
namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;

/**
 * @brief 线程上下文类
 * 集中管理线程本地状态，包括主协程、当前协程、调度器协程、调度器指针等
 * 使用thread_local变量存储，每个线程独立
 * 
 * 协程切换层次结构：
 *   main_fiber <---> scheduler_fiber <---> user_fiber
 *   - main_fiber: 线程入口协程，保存线程的原始上下文
 *   - scheduler_fiber: 调度器协程，运行Scheduler::run()
 *   - user_fiber: 用户协程，执行用户任务
 */
class ThreadContext {
public:
    /**
     * @brief 获取当前线程的上下文
     * @return 当前线程的ThreadContext指针，如果不存在则创建
     */
    static ThreadContext* get_current();

    /**
     * @brief 设置主协程
     * @param fiber 主协程指针
     */
    static void set_main_fiber(Fiber* fiber);

    /**
     * @brief 获取主协程
     * @return 主协程指针
     */
    static Fiber* get_main_fiber();

    /**
     * @brief 设置当前执行的协程
     * @param fiber 协程指针
     */
    static void set_current_fiber(Fiber* fiber);

    /**
     * @brief 获取当前执行的协程
     * @return 当前协程指针
     */
    static Fiber* get_current_fiber();

    /**
     * @brief 设置调度器协程
     * @param fiber 调度器协程指针
     */
    static void set_scheduler_fiber(Fiber* fiber);

    /**
     * @brief 获取调度器协程
     * @return 调度器协程指针
     */
    static Fiber* get_scheduler_fiber();

    /**
     * @brief 设置当前调度器
     * @param scheduler 调度器指针
     */
    static void set_scheduler(Scheduler* scheduler);

    /**
     * @brief 获取当前调度器
     * @return 调度器指针
     */
    static Scheduler* get_scheduler();

private:
    Fiber* main_fiber_ = nullptr;         // 主协程（线程入口协程）
    Fiber* current_fiber_ = nullptr;      // 当前执行的协程
    Fiber* scheduler_fiber_ = nullptr;    // 调度器协程
    Scheduler* scheduler_ = nullptr;      // 当前调度器
};

} // namespace zcoroutine

#endif // ZCOROUTINE_THREAD_CONTEXT_H_
