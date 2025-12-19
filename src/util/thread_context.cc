#include "util/thread_context.h"

#include<memory>

namespace zcoroutine {

// 线程本地变量，存储当前线程的上下文
thread_local std::unique_ptr<ThreadContext> t_thread_context = nullptr;

ThreadContext* ThreadContext::get_current() {
    if (!t_thread_context) {
        t_thread_context = std::make_unique<ThreadContext>();
    }
    return t_thread_context.get();
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

} // namespace zcoroutine
