#ifndef ZCOROUTINE_THREAD_CONTEXT_H_
#define ZCOROUTINE_THREAD_CONTEXT_H_

#include <memory>

namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;
class SharedStack;
class SwitchStack;
class Context;

// 栈模式枚举前向声明
enum class StackMode;

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

    /**
     * @brief 设置当前线程的栈模式
     * @param mode 栈模式
     */
    static void set_stack_mode(StackMode mode);

    /**
     * @brief 获取当前线程的栈模式
     * @return 栈模式
     */
    static StackMode get_stack_mode();

    /**
     * @brief 设置当前线程的共享栈
     * @param shared_stack 共享栈指针
     */
    static void set_shared_stack(std::shared_ptr<SharedStack> shared_stack);

    /**
     * @brief 获取当前线程的共享栈
     * @return 共享栈指针，如果未设置且模式为共享栈则自动创建
     */
    static SharedStack* get_shared_stack();

    /**
     * @brief 重置共享栈配置为默认值（独立栈模式）
     */
    static void reset_shared_stack_config();

    /**
     * @brief 获取当前线程的专用切换栈
     * @return 切换栈指针，如果不存在则自动创建
     */
    static SwitchStack* get_switch_stack();

    /**
     * @brief 获取当前线程的切换上下文（运行在 switch stack 上）
     * @return 切换上下文指针，如果不存在则自动创建
     */
    static Context* get_switch_context();

    /**
     * @brief 设置待切换的目标协程
     * @param fiber 目标协程
     */
    static void set_pending_fiber(Fiber* fiber);

    /**
     * @brief 获取待切换的目标协程
     * @return 目标协程
     */
    static Fiber* get_pending_fiber();

    /**
     * @brief 设置Hook启用状态
     * @param enable true表示启用，false表示禁用
     */
    static void set_hook_enable(bool enable);

    /**
     * @brief 获取Hook启用状态
     * @return true表示启用，false表示禁用
     */
    static bool is_hook_enabled();

private:
    Fiber* main_fiber_ = nullptr;         // 主协程（线程入口协程）
    Fiber* current_fiber_ = nullptr;      // 当前执行的协程
    Fiber* scheduler_fiber_ = nullptr;    // 调度器协程
    Scheduler* scheduler_ = nullptr;      // 当前调度器
    
    // 共享栈相关
    StackMode stack_mode_;                // 当前线程的栈模式
    std::shared_ptr<SharedStack> shared_stack_ = nullptr;  // 当前线程的共享栈
    std::unique_ptr<SwitchStack> switch_stack_ = nullptr;  // 专用切换栈
    std::unique_ptr<Context> switch_context_ = nullptr;    // 切换上下文（运行在 switch_stack_ 上）
    Fiber* pending_fiber_ = nullptr;      // 待切换的目标协程
    
    // Hook相关
    bool hook_enable_ = false;            // Hook启用标志
};

} // namespace zcoroutine

#endif // ZCOROUTINE_THREAD_CONTEXT_H_
