#include "util/thread_context.h"
#include "runtime/fiber.h"

#include <memory>

namespace zcoroutine {

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
    get_current()->main_fiber_ = fiber;
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
    ctx->pending_fiber_ = nullptr;
    ctx->occupy_fiber_ = nullptr;
}

void ThreadContext::set_pending_fiber(Fiber* fiber) {
    get_current()->pending_fiber_ = fiber;
}

Fiber* ThreadContext::get_pending_fiber() {
    return get_current()->pending_fiber_;
}

void ThreadContext::set_occupy_fiber(Fiber* fiber) {
    get_current()->occupy_fiber_ = fiber;
}

Fiber* ThreadContext::get_occupy_fiber() {
    return get_current()->occupy_fiber_;
}

} // namespace zcoroutine
