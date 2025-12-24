#include "runtime/fiber.h"
#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"
#include <cassert>
#include <cstring>

namespace zcoroutine
{

    // ============================================================================
    // SharedStackBuffer 实现
    // ============================================================================

    SharedStackBuffer::SharedStackBuffer(size_t stack_size)
        : stack_size_(stack_size)
    {
        // 使用StackAllocator分配栈内存
        stack_buffer_ = static_cast<char*>(StackAllocator::allocate(stack_size_));
        if (!stack_buffer_)
        {
            ZCOROUTINE_LOG_ERROR("SharedStackBuffer allocation failed: size={}", stack_size_);
            return;
        }

        // 栈顶指针（栈从高地址向低地址增长）
        stack_bp_ = stack_buffer_ + stack_size_;

        ZCOROUTINE_LOG_DEBUG("SharedStackBuffer created: buffer={}, size={}, stack_top={}",
                             static_cast<void*>(stack_buffer_), stack_size_,
                             static_cast<void*>(stack_bp_));
    }

    SharedStackBuffer::~SharedStackBuffer()
    {
        if (stack_buffer_)
        {
            ZCOROUTINE_LOG_DEBUG("SharedStackBuffer destroying: buffer={}",
                                 static_cast<void*>(stack_buffer_));
            StackAllocator::deallocate(stack_buffer_, stack_size_);
            stack_buffer_ = nullptr;
            stack_bp_ = nullptr;
        }
    }

    // ============================================================================
    // SharedStack 实现
    // ============================================================================

    // 静态常量成员定义
    constexpr size_t SharedStack::kDefaultStackSize;
    constexpr int SharedStack::kDefaultStackCount;

    SharedStack::SharedStack(int count, size_t stack_size)
        : stack_size_(stack_size), count_(count)
    {
        if (count <= 0)
        {
            count_ = kDefaultStackCount;
            ZCOROUTINE_LOG_WARN("SharedStack invalid count {}, using default {}",
                                count, kDefaultStackCount);
        }

        if (stack_size == 0)
        {
            stack_size_ = kDefaultStackSize;
            ZCOROUTINE_LOG_WARN("SharedStack invalid stack_size 0, using default {}",
                                kDefaultStackSize);
        }

        // 创建栈缓冲区数组
        stack_array_.reserve(count_);
        for (int i = 0; i < count_; ++i)
        {
            stack_array_.push_back(std::make_unique<SharedStackBuffer>(stack_size_));
        }

        ZCOROUTINE_LOG_INFO("SharedStack created: count={}, stack_size={}",
                            count_, stack_size_);
    }

    SharedStackBuffer* SharedStack::allocate()
    {
        if (stack_array_.empty())
        {
            ZCOROUTINE_LOG_ERROR("SharedStack::allocate failed: no stack buffers");
            return nullptr;
        }

        // 轮询分配
        unsigned int idx = alloc_idx_.fetch_add(1, std::memory_order_relaxed) % count_;

        ZCOROUTINE_LOG_DEBUG("SharedStack::allocate: idx={}", idx);

        return stack_array_[idx].get();
    }

    // ============================================================================
    // Fiber 实现
    // ============================================================================

    // 静态成员初始化
    std::atomic<uint64_t> Fiber::s_fiber_count_{0};

    // 主协程构造函数
    Fiber::Fiber()
        : name_("main_fiber"), id_(0), state_(State::kRunning), stack_size_(0), context_(std::make_unique<Context>())
    {

        // 主协程直接获取当前上下文
        context_->get_context();

        // 设置为当前协程
        ThreadContext::set_current_fiber(this);

        ZCOROUTINE_LOG_INFO("Main fiber created: name={}, id={}", name_, id_);
    }

    // 确定切换目标：
    // - 如果当前不是scheduler_fiber，切换回scheduler_fiber
    // - 如果当前是scheduler_fiber或没有scheduler_fiber，切换回main_fiber
    // 注意：禁用栈保护，因为共享栈协程可能从这里恢复
    __attribute__((no_stack_protector))
    void Fiber::confirm_switch_target()
    {
        Fiber *scheduler_fiber = ThreadContext::get_scheduler_fiber();
        Fiber *main_fiber = ThreadContext::get_main_fiber();

        Fiber *target_fiber = nullptr;
        if (scheduler_fiber && this != scheduler_fiber)
        {
            // 当前是user_fiber，切换回scheduler_fiber
            target_fiber = scheduler_fiber;
        }
        else if (main_fiber)
        {
            // 当前是scheduler_fiber或没有scheduler_fiber，切换回main_fiber
            target_fiber = main_fiber;
        }

        if (target_fiber && target_fiber->context_)
        {
            // 使用统一的共享栈切换函数
            co_swap(this, target_fiber);
        }
        else
        {
            ZCOROUTINE_LOG_ERROR("Fiber confirm_switch_target: no valid target fiber to switch to");
        }
    }

    // 统一的协程切换函数（类似libco的co_swap）
    // 关键：必须在swap_context之前恢复目标协程的栈内容
    // 因为swapcontext恢复寄存器后会从栈上读取返回地址
    // 注意：禁用栈保护，因为栈内容会被保存和恢复，canary值会变化
    __attribute__((no_stack_protector, noinline))
    void Fiber::co_swap(Fiber* curr, Fiber* pending)
    {
        // 使用内联汇编获取当前栈指针，确保捕获到足够低的地址
        // 需要保存到比swapcontext调用更低的地址，以确保包含所有栈帧
        char* current_sp;
#if defined(__x86_64__)
        __asm__ volatile("movq %%rsp, %0" : "=r"(current_sp));
#elif defined(__i386__)
        __asm__ volatile("movl %%esp, %0" : "=r"(current_sp));
#else
        // 其他架构使用局部变量估算
        char c;
        current_sp = &c;
#endif
        // 预留一些空间给swapcontext和swap_context的栈帧
        // swapcontext大约需要200-300字节的栈空间
        curr->stack_sp_ = current_sp - 512;

        // 共享栈切换处理
        if (pending->is_shared_stack_ && pending->shared_stack_buffer_)
        {
            // 获取当前占用此共享栈的协程
            Fiber* occupy_fiber = pending->shared_stack_buffer_->occupy_fiber();

            // 设置目标协程占用此共享栈
            pending->shared_stack_buffer_->set_occupy_fiber(pending);

            // 如果有其他协程占用且不是目标协程，保存其栈内容
            if (occupy_fiber && occupy_fiber != pending)
            {
                occupy_fiber->save_stack_buffer();
            }

            // 关键：在swap_context之前恢复目标协程的栈内容
            // 这样swapcontext切换后栈上的数据（包括返回地址）才是正确的
            // 只有当当前协程不在同一共享栈上时才安全执行恢复
            // （因为恢复会覆盖共享栈内容）
            if (pending->save_buffer_ && pending->save_size_ > 0)
            {
                // 检查当前协程是否在同一共享栈上
                bool curr_on_same_stack = curr->is_shared_stack_ && 
                                          curr->shared_stack_buffer_ == pending->shared_stack_buffer_;
                if (!curr_on_same_stack)
                {
                    // 安全：当前协程不在此共享栈上，可以恢复
                    memcpy(pending->stack_sp_, pending->save_buffer_, pending->save_size_);
                    ZCOROUTINE_LOG_DEBUG("co_swap restore before swap: pending={}, size={}",
                                        pending->name(), pending->save_size_);
                }
                // 如果在同一栈上，后面的恢复逻辑会处理（但这种情况不应该发生）
            }
        }

        set_this(pending);

        // 切换上下文
        Context::swap_context(curr->context_.get(), pending->context_.get());

        // swap_context返回后，我们回到了这个协程
        // 此时可能需要处理从共享栈协程切换回来的情况
        // 但由于我们在swap前已经恢复了栈内容，这里通常不需要额外处理
    }

    // 普通协程构造函数
    Fiber::Fiber(std::function<void()> func,
                 size_t stack_size,
                 const std::string &name,
                 bool use_shared_stack)
        : stack_size_(stack_size), context_(std::make_unique<Context>()), callback_(std::move(func))
    {
        // 检查全局配置或显式指定使用共享栈
        bool should_use_shared = use_shared_stack ||
                                 (ThreadContext::get_stack_mode() == StackMode::kShared);

        // 分配全局唯一ID
        id_ = s_fiber_count_.fetch_add(1, std::memory_order_relaxed);

        // 设置协程名称
        if (name.empty())
        {
            name_ = "fiber_" + std::to_string(id_);
        }
        else
        {
            name_ = name + "_" + std::to_string(id_);
        }

        ZCOROUTINE_LOG_DEBUG("Fiber creating: name={}, id={}, stack_size={}, shared_stack={}",
                             name_, id_, stack_size_, should_use_shared);

        if (should_use_shared)
        {
            // 共享栈模式
            is_shared_stack_ = true;
            SharedStack* shared_stack = ThreadContext::get_shared_stack();
            if (!shared_stack)
            {
                ZCOROUTINE_LOG_FATAL("Fiber shared stack not available: name={}, id={}",
                                     name_, id_);
                abort();
            }

            shared_stack_buffer_ = shared_stack->allocate();
            if (!shared_stack_buffer_)
            {
                ZCOROUTINE_LOG_FATAL("Fiber shared stack buffer allocation failed: name={}, id={}",
                                     name_, id_);
                abort();
            }

            stack_size_ = shared_stack->stack_size();
            stack_ptr_ = shared_stack_buffer_->buffer();

            ZCOROUTINE_LOG_DEBUG("Fiber using shared stack: name={}, id={}, buffer={}, size={}",
                                 name_, id_, static_cast<void*>(stack_ptr_), stack_size_);
        }
        else
        {
            // 独立栈模式
            is_shared_stack_ = false;
            stack_ptr_ = StackAllocator::allocate(stack_size_);
            if (!stack_ptr_)
            {
                ZCOROUTINE_LOG_FATAL("Fiber stack allocation failed: name={}, id={}, size={}",
                                     name_, id_, stack_size_);
                abort();
            }
            ZCOROUTINE_LOG_DEBUG("Fiber using independent stack: name={}, id={}, ptr={}, size={}",
                                 name_, id_, static_cast<void *>(stack_ptr_), stack_size_);
        }

        // 创建上下文
        context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);

        ZCOROUTINE_LOG_INFO("Fiber created: name={}, id={}, is_shared_stack={}", 
                            name_, id_, is_shared_stack_);
    }

    // 使用指定共享栈的构造函数
    Fiber::Fiber(std::function<void()> func,
                 SharedStack* shared_stack,
                 const std::string &name)
        : context_(std::make_unique<Context>()), callback_(std::move(func)), is_shared_stack_(true)
    {
        // 分配全局唯一ID
        id_ = s_fiber_count_.fetch_add(1, std::memory_order_relaxed);

        // 设置协程名称
        if (name.empty())
        {
            name_ = "fiber_" + std::to_string(id_);
        }
        else
        {
            name_ = name + "_" + std::to_string(id_);
        }

        if (!shared_stack)
        {
            ZCOROUTINE_LOG_FATAL("Fiber constructor: shared_stack is null, name={}, id={}",
                                 name_, id_);
            abort();
        }

        shared_stack_buffer_ = shared_stack->allocate();
        if (!shared_stack_buffer_)
        {
            ZCOROUTINE_LOG_FATAL("Fiber shared stack buffer allocation failed: name={}, id={}",
                                 name_, id_);
            abort();
        }

        stack_size_ = shared_stack->stack_size();
        stack_ptr_ = shared_stack_buffer_->buffer();

        ZCOROUTINE_LOG_DEBUG("Fiber creating with explicit shared stack: name={}, id={}, buffer={}, size={}",
                             name_, id_, static_cast<void*>(stack_ptr_), stack_size_);

        // 创建上下文
        context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);

        ZCOROUTINE_LOG_INFO("Fiber created: name={}, id={}, is_shared_stack=true", name_, id_);
    }

    Fiber::~Fiber()
    {
        ZCOROUTINE_LOG_DEBUG("Fiber destroying: name={}, id={}, state={}, is_shared_stack={}",
                             name_, id_, static_cast<int>(state_), is_shared_stack_);

        if (is_shared_stack_)
        {
            // 共享栈模式：释放保存的栈内容（使用StackAllocator）
            if (save_buffer_)
            {
                StackAllocator::deallocate(save_buffer_, save_size_);
                save_buffer_ = nullptr;
                save_size_ = 0;
            }
            // 如果当前协程占用了共享栈，清除占用标记
            if (shared_stack_buffer_ && shared_stack_buffer_->occupy_fiber() == this)
            {
                shared_stack_buffer_->set_occupy_fiber(nullptr);
            }
        }
        else
        {
            // 独立栈模式：释放栈内存
            if (stack_ptr_)
            {
                StackAllocator::deallocate(stack_ptr_, stack_size_);
                stack_ptr_ = nullptr;
                ZCOROUTINE_LOG_DEBUG("Fiber stack deallocated: name={}, id={}", name_, id_);
            }
        }
    }

    void Fiber::resume()
    {
        assert(state_ != State::kTerminated && "Cannot resume terminated fiber");
        assert(state_ != State::kRunning && "Fiber is already running");

        // 获取当前协程上下文
        Fiber *prev_fiber = get_this();

        // 如果没有当前协程，自动创建main_fiber（用于独立测试场景）
        // 注意：这是为了兼容没有Scheduler的情况
        static thread_local std::unique_ptr<Fiber> t_implicit_main_fiber;
        if (!prev_fiber)
        {
            if (!t_implicit_main_fiber)
            {
                t_implicit_main_fiber = std::unique_ptr<Fiber>(new Fiber());
                ThreadContext::set_main_fiber(t_implicit_main_fiber.get());
            }
            prev_fiber = t_implicit_main_fiber.get();
            set_this(prev_fiber);
        }

        // 更新状态
        State prev_state = state_;
        state_ = State::kRunning;

        ZCOROUTINE_LOG_DEBUG("Fiber resume: name={}, id={}, prev_state={}",
                             name_, id_, static_cast<int>(prev_state));

        // 使用统一的切换函数（处理共享栈保存和恢复）
        co_swap(prev_fiber, this);

        // 协程执行完毕后会切换回来，恢复前一个协程
        set_this(prev_fiber);

        // 如果协程结束并且有异常，重新抛出
        if (exception_)
        {
            std::rethrow_exception(exception_);
        }
    }

    void Fiber::yield()
    {
        Fiber *cur_fiber = ThreadContext::get_current_fiber();
        if (!cur_fiber)
        {
            ZCOROUTINE_LOG_WARN("Fiber::yield failed: no current fiber to yield");
            return;
        }

        assert(cur_fiber->state_ == State::kRunning && "Can only yield running fiber");

        // 更新状态
        cur_fiber->state_ = State::kSuspended;

        ZCOROUTINE_LOG_DEBUG("Fiber yield: name={}, id={}", cur_fiber->name_, cur_fiber->id_);

        // 确定切换目标协程
        cur_fiber->confirm_switch_target();
    }

    void Fiber::reset(std::function<void()> func)
    {
        assert(state_ == State::kTerminated && "Can only reset terminated fiber");

        callback_ = std::move(func);
        state_ = State::kReady;
        exception_ = nullptr;

        // 共享栈模式：清理保存的栈内容
        if (is_shared_stack_)
        {
            if (save_buffer_)
            {
                StackAllocator::deallocate(save_buffer_, save_size_);
                save_buffer_ = nullptr;
                save_size_ = 0;
            }
            // 如果当前协程占用了共享栈，清除占用标记
            if (shared_stack_buffer_ && shared_stack_buffer_->occupy_fiber() == this)
            {
                shared_stack_buffer_->set_occupy_fiber(nullptr);
            }
        }

        // 重新创建上下文
        context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);

        ZCOROUTINE_LOG_DEBUG("Fiber reset: name={}, id={}", name_, id_);
    }

    void Fiber::main_func()
    {
        Fiber *cur_fiber = get_this();
        assert(cur_fiber && "No current fiber in main_func");

        ZCOROUTINE_LOG_DEBUG("Fiber main_func starting: name={}, id={}",
                             cur_fiber->name_, cur_fiber->id_);

        try
        {
            // 执行协程函数
            cur_fiber->callback_();
            cur_fiber->callback_ = nullptr;
            cur_fiber->state_ = State::kTerminated;

            ZCOROUTINE_LOG_INFO("Fiber terminated normally: name={}, id={}",
                                cur_fiber->name_, cur_fiber->id_);
        }
        catch (const std::exception &e)
        {
            // 捕获标准异常
            cur_fiber->exception_ = std::current_exception();
            cur_fiber->state_ = State::kTerminated;

            ZCOROUTINE_LOG_ERROR("Fiber terminated with exception: name={}, id={}, what={}",
                                 cur_fiber->name_, cur_fiber->id_, e.what());
        }
        catch (...)
        {
            // 捕获其他异常
            cur_fiber->exception_ = std::current_exception();
            cur_fiber->state_ = State::kTerminated;

            ZCOROUTINE_LOG_ERROR("Fiber terminated with unknown exception: name={}, id={}",
                                 cur_fiber->name_, cur_fiber->id_);
        }

        // 切换回调度器或主协程
        // 如果协程终止且使用共享栈，清除占用标记
        if (cur_fiber->state_ == State::kTerminated && cur_fiber->is_shared_stack_)
        {
            if (cur_fiber->shared_stack_buffer_ && 
                cur_fiber->shared_stack_buffer_->occupy_fiber() == cur_fiber)
            {
                cur_fiber->shared_stack_buffer_->set_occupy_fiber(nullptr);
            }
        }
        cur_fiber->confirm_switch_target();
    }

    Fiber *Fiber::get_this()
    {
        return ThreadContext::get_current_fiber();
    }

    void Fiber::set_this(Fiber *fiber)
    {
        ThreadContext::set_current_fiber(fiber);
    }

    __attribute__((no_stack_protector))
    void Fiber::save_stack_buffer()
    {
        if (!is_shared_stack_ || !shared_stack_buffer_)
        {
            return;
        }

        // 计算需要保存的栈大小
        // stack_bp_是栈顶（高地址），stack_sp_是当前栈指针（低地址）
        char* stack_bp = shared_stack_buffer_->stack_top();
        if (!stack_sp_ || stack_sp_ >= stack_bp)
        {
            ZCOROUTINE_LOG_WARN("Fiber::save_stack_buffer invalid stack_sp: name={}, id={}",
                               name_, id_);
            return;
        }

        size_t len = static_cast<size_t>(stack_bp - stack_sp_);
        if (len == 0)
        {
            return;
        }

        // 释放旧的保存缓冲区
        if (save_buffer_)
        {
            StackAllocator::deallocate(save_buffer_, save_size_);
            save_buffer_ = nullptr;
        }

        // 使用StackAllocator分配新的保存缓冲区
        save_buffer_ = static_cast<char*>(StackAllocator::allocate(len));
        if (!save_buffer_)
        {
            ZCOROUTINE_LOG_ERROR("Fiber::save_stack_buffer allocation failed: name={}, id={}, size={}",
                                name_, id_, len);
            return;
        }

        save_size_ = len;

        // 复制栈内容
        memcpy(save_buffer_, stack_sp_, len);

        ZCOROUTINE_LOG_DEBUG("Fiber::save_stack_buffer: name={}, id={}, size={}",
                            name_, id_, save_size_);
    }

    __attribute__((no_stack_protector))
    void Fiber::restore_stack_buffer()
    {
        if (!is_shared_stack_ || !save_buffer_ || save_size_ == 0)
        {
            return;
        }

        // 验证stack_sp_是否在共享栈范围内
        char* stack_base = shared_stack_buffer_->buffer();
        char* stack_top = shared_stack_buffer_->stack_top();
        if (stack_sp_ < stack_base || stack_sp_ >= stack_top)
        {
            ZCOROUTINE_LOG_ERROR("Fiber::restore_stack_buffer invalid stack_sp: name={}, id={}, stack_sp={}, base={}, top={}",
                                name_, id_, static_cast<void*>(stack_sp_), 
                                static_cast<void*>(stack_base), static_cast<void*>(stack_top));
            return;
        }

        // 恢复栈内容
        memcpy(stack_sp_, save_buffer_, save_size_);

        ZCOROUTINE_LOG_DEBUG("Fiber::restore_stack_buffer: name={}, id={}, size={}",
                            name_, id_, save_size_);
    }

} // namespace zcoroutine
