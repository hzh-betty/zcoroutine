#ifndef ZCOROUTINE_FIBER_H_
#define ZCOROUTINE_FIBER_H_

#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include "runtime/context.h"
#include "runtime/stack_allocator.h"

namespace zcoroutine {

/**
 * * @brief 协程类
 * 管理协程的生命周期、状态和上下文切换
 */
class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    using ptr = std::shared_ptr<Fiber>;

    /**
     * @brief 协程状态枚举
     */
    enum class State {
        kReady,         // 就绪态，等待调度
        kRunning,       // 运行态，正在执行
        kSuspended,     // 挂起态，主动让出CPU
        kTerminated     // 终止态，执行完毕
    };

    /**
     * @brief 构造函数
     * @param func 协程执行函数
     * @param stack_size 栈大小，默认128KB
     * @param name 协程名称，默认为空（自动生成fiber_id）
     */
    explicit Fiber(std::function<void()> func,
                   size_t stack_size = StackAllocator::kDefaultStackSize,
                   const std::string& name = "");

    /**
     * @brief 析构函数
     */
    ~Fiber();

    /**
     * @brief 恢复协程执行
     * 从当前协程切换到此协程
     */
    void resume();

    /**
     * @brief 挂起协程
     * 将控制权交还给调用者或主协程
     */
    static void yield();

    /**
     * @brief 重置协程
     * @param func 新的执行函数
     * 用于协程池复用协程对象
     */
    void reset(std::function<void()> func);

    /**
     * @brief 获取协程名称
     * @return 协程名称（格式：name_id或fiber_id）
     */
    std::string name() const { return name_; }

    /**
     * @brief 获取协程ID
     * @return 协程全局唯一ID
     */
    uint64_t id() const { return id_; }

    /**
     * @brief 获取协程状态
     * @return 当前状态
     */
    State state() const { return state_; }

    /**
     * @brief 协程主函数（静态）
     * 在协程上下文中执行
     */
    static void main_func();

    /**
     * @brief 获取当前执行的协程
     * @return 当前协程指针
     */
    static Fiber* get_this();

    /**
     * @brief 设置当前协程
     * @param fiber 协程指针
     */
    static void set_this(Fiber* fiber);

private:
    // Scheduler需要访问私有构造函数创建main_fiber
    friend class Scheduler;

    /**
     * @brief 主协程构造函数（私有）
     * 用于创建线程的主协程
     */
    Fiber();

    /**
     * @brief 确定切换目标协程(切换到scheduler_fiber或main_fiber)
     */
    void confirm_switch_target();

    std::string name_;                      // 协程名称
    uint64_t id_ = 0;                       // 协程唯一ID
    State state_ = State::kReady;           // 协程状态
    size_t stack_size_ = 0;                 // 栈大小

    std::unique_ptr<Context> context_;      // 上下文对象
    void* stack_ptr_ = nullptr;             // 栈指针（独立栈模式）

    std::function<void()> callback_;        // 协程执行函数
    std::exception_ptr exception_;          // 协程异常指针

    // 全局协程计数器（线程安全）
    static std::atomic<uint64_t> s_fiber_count_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_H_
