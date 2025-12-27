#include "util/thread_context.h"
#include "runtime/fiber.h"
#include "runtime/shared_stack.h"
#include "runtime/context.h"
#include "util/zcoroutine_logger.h"

#include <memory>

namespace zcoroutine {

constexpr int ThreadContext::kMaxCallStackDepth;

// 线程本地变量，存储当前线程的上下文
thread_local std::unique_ptr<ThreadContext> t_thread_context = nullptr;

ThreadContext* ThreadContext::get_current() {
    if (!t_thread_context) {
        t_thread_context = std::make_unique<ThreadContext>();
        // 初始化默认值
        t_thread_context->stack_mode_ = StackMode::kIndependent;
    }
    return t_thread_context.get();
}

void ThreadContext::set_main_fiber(Fiber* fiber) {
    auto* ctx = get_current();
    ctx->main_fiber_ = fiber;
    if (fiber) {
        ctx->call_stack_size_ = 0;
        if (ctx->call_stack_size_ < kMaxCallStackDepth) {
            ctx->call_stack_[ctx->call_stack_size_++] = fiber;
        }
        ctx->current_fiber_ = fiber;
    } else {
        ctx->call_stack_size_ = 0;
        ctx->current_fiber_ = nullptr;
    }
}

Fiber* ThreadContext::get_main_fiber() {
    return get_current()->main_fiber_;
}

void ThreadContext::set_current_fiber(Fiber* fiber) {
    get_current()->current_fiber_ = fiber;
}

Fiber* ThreadContext::get_current_fiber() {
    return get_current()->current_fiber_;
}

void ThreadContext::set_scheduler_fiber(Fiber* fiber) {
    get_current()->scheduler_fiber_ = fiber;
}

Fiber* ThreadContext::get_scheduler_fiber() {
    return get_current()->scheduler_fiber_;
}

void ThreadContext::set_scheduler(Scheduler* scheduler) {
    get_current()->scheduler_ = scheduler;
}

Scheduler* ThreadContext::get_scheduler() {
    return get_current()->scheduler_;
}

void ThreadContext::set_stack_mode(StackMode mode) {
    get_current()->stack_mode_ = mode;
}

StackMode ThreadContext::get_stack_mode() {
    return get_current()->stack_mode_;
}

void ThreadContext::set_shared_stack(std::shared_ptr<SharedStack> shared_stack) {
    get_current()->shared_stack_ = std::move(shared_stack);
}

SharedStack* ThreadContext::get_shared_stack() {
    auto* ctx = get_current();
    // 如果是共享栈模式但未设置共享栈，自动创建
    if (ctx->stack_mode_ == StackMode::kShared && !ctx->shared_stack_) {
        ctx->shared_stack_ = std::make_shared<SharedStack>();
    }
    return ctx->shared_stack_.get();
}

void ThreadContext::reset_shared_stack_config() {
    auto* ctx = get_current();
    ctx->stack_mode_ = StackMode::kIndependent;
    ctx->shared_stack_ = nullptr;
}

SwitchStack* ThreadContext::get_switch_stack() {
    auto* ctx = get_current();
    // 如果不存在则自动创建
    if (!ctx->switch_stack_) {
        ctx->switch_stack_ = std::make_unique<SwitchStack>();
    }
    return ctx->switch_stack_.get();
}

Context* ThreadContext::get_switch_context() {
    auto* ctx = get_current();
    // 如果不存在则自动创建
    if (!ctx->switch_context_) {
        // 确保 switch_stack 已创建
        SwitchStack* switch_stack = get_switch_stack();
        
        ctx->switch_context_ = std::make_unique<Context>();
        // 初始化切换上下文，使其运行在 switch_stack 上
        ctx->switch_context_->make_context(
            switch_stack->buffer(),
            switch_stack->size(),
            SwitchStack::switch_func
        );
        
        ZCOROUTINE_LOG_DEBUG("ThreadContext: created switch_context on switch_stack");
    }
    return ctx->switch_context_.get();
}

void ThreadContext::set_pending_fiber(Fiber* fiber) {
    get_current()->pending_fiber_ = fiber;
}

Fiber* ThreadContext::get_pending_fiber() {
    return get_current()->pending_fiber_;
}

void ThreadContext::set_hook_enable(bool enable) {
    get_current()->hook_enable_ = enable;
}

bool ThreadContext::is_hook_enabled() {
    return get_current()->hook_enable_;
}

void ThreadContext::push_call_stack(Fiber* fiber) {
    auto* ctx = get_current();
    if (!fiber) return;
    if (ctx->call_stack_size_ < kMaxCallStackDepth) {
        ctx->call_stack_[ctx->call_stack_size_++] = fiber;
    } else {
        ZCOROUTINE_LOG_WARN("Call stack depth reached max {}, fiber={}", kMaxCallStackDepth, fiber->name());
    }
}

Fiber* ThreadContext::pop_call_stack() {
    auto* ctx = get_current();
    if (ctx->call_stack_size_ <= 0) return nullptr;
    Fiber* f = ctx->call_stack_[ctx->call_stack_size_ - 1];
    ctx->call_stack_[ctx->call_stack_size_ - 1] = nullptr;
    ctx->call_stack_size_--;
    return f;
}

Fiber* ThreadContext::top_call_stack() {
    auto* ctx = get_current();
    if (ctx->call_stack_size_ <= 0) return nullptr;
    return ctx->call_stack_[ctx->call_stack_size_ - 1];
}

int ThreadContext::call_stack_size() {
    return get_current()->call_stack_size_;
}




} // namespace zcoroutine
