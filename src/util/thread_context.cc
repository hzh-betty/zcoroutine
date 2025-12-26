#include "util/thread_context.h"
#include "runtime/fiber.h"
#include "runtime/shared_stack.h"
#include "runtime/context.h"
#include "util/zcoroutine_logger.h"

#include <memory>

namespace zcoroutine {

// 前向声明切换函数
static void switch_func();

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
            switch_func
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

/**
 * @brief 切换函数 - 运行在 switch stack 上
 * 
 * 这个函数在专用切换栈上执行，负责：
 * 1. 从当前协程的 context 中获取 rsp（已由 swapcontext 保存）
 * 2. 保存当前协程的栈内容（如果是共享栈）
 * 3. 恢复目标协程的栈内容（如果是共享栈）
 * 4. 切换到目标协程
 * 
 * 整个过程不使用任何 magic number，所有栈操作都在独立的 switch stack 上进行
 */
static void switch_func() {
    while (true) {
        Fiber* curr = ThreadContext::get_current_fiber();
        Fiber* target = ThreadContext::get_pending_fiber();
        Context* switch_ctx = ThreadContext::get_switch_context();
        
        if (!curr || !target) {
            ZCOROUTINE_LOG_ERROR("switch_func: invalid curr or target fiber");
            return;
        }
        
        ZCOROUTINE_LOG_DEBUG("switch_func: curr={}, target={}", 
                            curr->name(), target->name());
        
        // 处理当前协程的栈保存（在 switch stack 上执行，安全）
        if (curr->is_shared_stack()) {
            SharedContext* curr_stack_ctx = curr->get_shared_context();
            if (curr_stack_ctx) {
                // 从 curr 的 context 中获取 rsp（已由 swapcontext 保存）
                void* curr_rsp = curr->context()->get_stack_pointer();
                
                // 保存当前协程的栈内容
                curr_stack_ctx->save_stack_buffer(curr_rsp);
                
                ZCOROUTINE_LOG_DEBUG("switch_func: saved curr stack, rsp={}", curr_rsp);
            }
        }
        
        // 处理目标协程的栈恢复（在 switch stack 上执行，安全）
        if (target->is_shared_stack()) {
            SharedContext* target_stack_ctx = target->get_shared_context();
            if (target_stack_ctx) {
                SharedStackBuffer* buffer = target_stack_ctx->shared_stack_buffer();
                if (buffer) {
                    // 获取当前占用此共享栈的协程
                    Fiber* occupy_fiber = buffer->occupy_fiber();
                    
                    // 设置目标协程占用此共享栈
                    buffer->set_occupy_fiber(target);
                    
                    // 如果有其他协程占用且不是目标协程且不是当前协程，保存其栈内容
                    if (occupy_fiber && occupy_fiber != target && occupy_fiber != curr) {
                        SharedContext* occupy_stack_ctx = occupy_fiber->get_shared_context();
                        if (occupy_stack_ctx) {
                            void* occupy_rsp = occupy_fiber->context()->get_stack_pointer();
                            occupy_stack_ctx->save_stack_buffer(occupy_rsp);
                        }
                    }
                    
                    // 恢复目标协程的栈内容
                    if (target_stack_ctx->save_buffer() && target_stack_ctx->save_size() > 0) {
                        target_stack_ctx->restore_stack_buffer();
                        ZCOROUTINE_LOG_DEBUG("switch_func: restored target stack, size={}",
                                            target_stack_ctx->save_size());
                    }
                }
            }
        }
        
        // 设置当前协程为目标
        ThreadContext::set_current_fiber(target);
        
        // 切换到目标协程
        Context::swap_context(switch_ctx, target->context());
        
        // 当有人切换回 switch_context 时，继续循环处理下一次切换
    }
}

} // namespace zcoroutine
