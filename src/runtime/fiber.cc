#include "runtime/fiber.h"
#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"
#include <cassert>
#include <cstring>

namespace zcoroutine
{

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
            set_this(target_fiber);
            Context::swap_context(this->context_.get(), target_fiber->context_.get());
        }
        else
        {
            ZCOROUTINE_LOG_ERROR("Fiber confirm_switch_target: no valid target fiber to switch to");
        }
    }

    // 普通协程构造函数
    Fiber::Fiber(std::function<void()> func,
                 size_t stack_size,
                 const std::string &name)
        : callback_(std::move(func)), stack_size_(stack_size), context_(std::make_unique<Context>())
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

        ZCOROUTINE_LOG_DEBUG("Fiber creating: name={}, id={}, stack_size={}",
                             name_, id_, stack_size_);

        // 分配栈内存
        stack_ptr_ = StackAllocator::allocate(stack_size_);
        if (!stack_ptr_)
        {
            ZCOROUTINE_LOG_FATAL("Fiber stack allocation failed: name={}, id={}, size={}",
                                 name_, id_, stack_size_);
            abort();
        }
        ZCOROUTINE_LOG_DEBUG("Fiber using independent stack: name={}, id={}, ptr={}, size={}",
                             name_, id_, static_cast<void *>(stack_ptr_), stack_size_);

        // 创建上下文
        context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);

        ZCOROUTINE_LOG_INFO("Fiber created: name={}, id={}", name_, id_);
    }

    Fiber::~Fiber()
    {
        ZCOROUTINE_LOG_DEBUG("Fiber destroying: name={}, id={}, state={}",
                             name_, id_, static_cast<int>(state_));

        // 释放栈内存
        if (stack_ptr_)
        {
            StackAllocator::deallocate(stack_ptr_, stack_size_);
            stack_ptr_ = nullptr;
            ZCOROUTINE_LOG_DEBUG("Fiber stack deallocated: name={}, id={}", name_, id_);
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

        // 设置为当前协程
        set_this(this);

        // 更新状态
        State prev_state = state_;
        state_ = State::kRunning;

        ZCOROUTINE_LOG_DEBUG("Fiber resume: name={}, id={}, prev_state={}",
                             name_, id_, static_cast<int>(prev_state));

        // 切换上下文
        Context::swap_context(prev_fiber->context_.get(), context_.get());

        // 协程执行完毕后会切换回来，恢复前一个协程
        set_this(prev_fiber);

        // 如果协程结束并且有异常，重新抛出
        if (state_ == State::kTerminated && exception_)
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

} // namespace zcoroutine
