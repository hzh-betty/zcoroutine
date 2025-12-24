#ifndef ZCOROUTINE_FIBER_H_
#define ZCOROUTINE_FIBER_H_

#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include <vector>
#include "runtime/context.h"
#include "runtime/stack_allocator.h"
#include "util/noncopyable.h"

namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;

/**
 * @brief 栈模式枚举
 * 定义协程使用的栈模式
 */
enum class StackMode {
    kIndependent,   // 独立栈模式（默认）：每个协程拥有独立的栈空间
    kShared         // 共享栈模式：多个协程共享同一个栈空间，切换时保存/恢复栈内容
};

/**
 * @brief 共享栈缓冲区类
 * 表示一个可被多个协程共享的栈空间
 * 同一时刻只有一个协程可以占用此栈空间
 */
class SharedStackBuffer : public NonCopyable {
public:
    using ptr = std::shared_ptr<SharedStackBuffer>;

    /**
     * @brief 构造函数
     * @param stack_size 栈大小
     */
    explicit SharedStackBuffer(size_t stack_size);

    /**
     * @brief 析构函数
     */
    ~SharedStackBuffer();

    /**
     * @brief 获取栈缓冲区指针
     * @return 栈缓冲区起始地址
     */
    char* buffer() const { return stack_buffer_; }

    /**
     * @brief 获取栈顶指针（高地址端）
     * @return 栈顶地址
     */
    char* stack_top() const { return stack_bp_; }

    /**
     * @brief 获取栈大小
     * @return 栈大小
     */
    size_t size() const { return stack_size_; }

    /**
     * @brief 获取当前占用此栈的协程
     * @return 占用协程指针，如果无协程占用则返回nullptr
     * @note 使用原始指针避免循环引用
     */
    Fiber* occupy_fiber() const { return occupy_fiber_; }

    /**
     * @brief 设置占用此栈的协程
     * @param fiber 协程指针
     */
    void set_occupy_fiber(Fiber* fiber) { occupy_fiber_ = fiber; }

private:
    char* stack_buffer_ = nullptr;      // 栈缓冲区起始地址（低地址）
    char* stack_bp_ = nullptr;          // 栈顶地址（高地址，栈从高往低增长）
    size_t stack_size_ = 0;             // 栈大小
    Fiber* occupy_fiber_ = nullptr;     // 当前占用此栈的协程（原始指针避免循环引用）
};

/**
 * @brief 共享栈池类
 * 管理一组共享栈缓冲区，提供轮询分配策略
 * 可以被多个协程共享使用
 */
class SharedStack : public NonCopyable {
public:
    using ptr = std::shared_ptr<SharedStack>;

    /**
     * @brief 默认共享栈大小：128KB
     */
    static constexpr size_t kDefaultStackSize = 128 * 1024;

    /**
     * @brief 默认共享栈数量
     */
    static constexpr int kDefaultStackCount = 4;

    /**
     * @brief 构造函数
     * @param count 共享栈缓冲区数量
     * @param stack_size 每个栈的大小
     */
    explicit SharedStack(int count = kDefaultStackCount, 
                        size_t stack_size = kDefaultStackSize);

    /**
     * @brief 析构函数
     */
    ~SharedStack() = default;

    /**
     * @brief 获取一个共享栈缓冲区（轮询分配）
     * @return 共享栈缓冲区指针
     */
    SharedStackBuffer* allocate();

    /**
     * @brief 获取栈大小
     * @return 栈大小
     */
    size_t stack_size() const { return stack_size_; }

    /**
     * @brief 获取栈缓冲区数量
     * @return 缓冲区数量
     */
    int count() const { return count_; }

private:
    std::vector<std::unique_ptr<SharedStackBuffer>> stack_array_;   // 栈缓冲区数组
    size_t stack_size_ = 0;                                          // 每个栈的大小
    int count_ = 0;                                                  // 栈缓冲区数量
    std::atomic<unsigned int> alloc_idx_{0};                         // 轮询分配索引
};

/**
 * @brief 协程类
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
     * @param use_shared_stack 是否强制使用共享栈（忽略全局配置）
     */
    explicit Fiber(std::function<void()> func,
                   size_t stack_size = StackAllocator::kDefaultStackSize,
                   const std::string& name = "",
                   bool use_shared_stack = false);

    /**
     * @brief 构造函数（使用指定共享栈）
     * @param func 协程执行函数
     * @param shared_stack 共享栈指针
     * @param name 协程名称，默认为空（自动生成fiber_id）
     */
    Fiber(std::function<void()> func,
          SharedStack* shared_stack,
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
     * @brief 检查是否使用共享栈
     * @return true表示使用共享栈，false表示使用独立栈
     */
    bool is_shared_stack() const { return is_shared_stack_; }

    /**
     * @brief 获取栈模式
     * @return 当前使用的栈模式
     */
    StackMode stack_mode() const { return is_shared_stack_ ? StackMode::kShared : StackMode::kIndependent; }

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

    /**
     * @brief 统一的协程切换函数（类似libco的co_swap）
     * 将栈指针记录和恢复逻辑放在同一个栈帧中，保证共享栈切换的正确性
     * @param curr 当前协程
     * @param pending 目标协程
     */
    static void co_swap(Fiber* curr, Fiber* pending);

    std::string name_;                      // 协程名称
    uint64_t id_ = 0;                       // 协程唯一ID
    State state_ = State::kReady;           // 协程状态
    size_t stack_size_ = 0;                 // 栈大小

    std::unique_ptr<Context> context_;      // 上下文对象
    void* stack_ptr_ = nullptr;             // 栈指针

    std::function<void()> callback_;        // 协程执行函数
    std::exception_ptr exception_;          // 协程异常指针

    // 共享栈相关成员
    bool is_shared_stack_ = false;          // 是否使用共享栈
    SharedStackBuffer* shared_stack_buffer_ = nullptr;  // 共享栈缓冲区
    char* stack_sp_ = nullptr;              // 协程栈指针（用于共享栈保存）
    char* save_buffer_ = nullptr;           // 栈内容保存缓冲区
    size_t save_size_ = 0;                  // 保存的栈大小

    /**
     * @brief 保存栈内容到save_buffer
     * 用于共享栈切换时保存当前协程的栈内容
     */
    void save_stack_buffer();

    /**
     * @brief 恢复栈内容
     * 用于共享栈切换时恢复协程的栈内容
     */
    void restore_stack_buffer();

    // 全局协程计数器（线程安全）
    static std::atomic<uint64_t> s_fiber_count_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_H_
