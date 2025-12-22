#include "io/fd_context.h"
#include "util/zcoroutine_logger.h"
#include "scheduling/scheduler.h"
#include <cstring>

namespace zcoroutine {
    FdContext::FdContext(int fd) : fd_(fd) {
        ZCOROUTINE_LOG_DEBUG("FdContext created: fd={}", fd_);
    }

    int FdContext::add_event(Event event) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查事件是否已存在
        if (events_ & event) {
            ZCOROUTINE_LOG_WARN("FdContext::add_event event already exists: fd={}, event={}, current_events={}",
                                fd_, event, events_);
            return events_;
        }

        // 添加事件
        int old_events = events_;
        int new_events = events_ | event;
        events_ = new_events;

        ZCOROUTINE_LOG_DEBUG("FdContext::add_event success: fd={}, event={}, old_events={}, new_events={}",
                             fd_, event, old_events, new_events);

        return new_events;
    }

    int FdContext::del_event(Event event) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查事件是否存在
        if (!(events_ & event)) {
            ZCOROUTINE_LOG_DEBUG("FdContext::del_event event not exists: fd={}, event={}, current_events={}",
                                 fd_, event, events_);
            return events_;
        }

        // 删除事件
        int old_events = events_;
        int new_events = events_ & ~event;
        events_ = new_events;

        // 重置对应的事件上下文
        if (event == kRead) {
            reset_event_context(read_ctx_);
            ZCOROUTINE_LOG_DEBUG("FdContext::del_event READ context reset: fd={}", fd_);
        }
        else if (event == kWrite) {
            reset_event_context(write_ctx_);
            ZCOROUTINE_LOG_DEBUG("FdContext::del_event WRITE context reset: fd={}", fd_);
        }

        ZCOROUTINE_LOG_DEBUG("FdContext::del_event success: fd={}, event={}, old_events={}, new_events={}",
                             fd_, event, old_events, new_events);

        return new_events;
    }

    int FdContext::cancel_event(Event event) {
        int new_events = 0;
        std::function<void()> callback = nullptr;
        Fiber::ptr fiber = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 检查事件是否存在
            if (!(events_ & event)) {
                ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event event not exists: fd={}, event={}, current_events={}",
                                     fd_, event, events_);
                return events_;
            }

            // 获取事件上下文
            EventContext &ctx = get_event_context(event);

            // 先取出callback和fiber，避免在回调中重新注册事件时被后续重置
            callback = std::move(ctx.callback);
            fiber = std::move(ctx.fiber);

            // 删除事件标志并重置事件上下文
            int old_events = events_;
            new_events = events_ & ~event;
            events_ = new_events;

            reset_event_context(ctx);

            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event success: fd={}, event={}, old_events={}, new_events={}",
                                 fd_, event, old_events, new_events);
        }

        if (callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event executing callback: fd={}, event={}", fd_, event);
            callback();
        }
        else if (fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event scheduling fiber: fd={}, event={}, fiber_id={}",
                                 fd_, event, fiber->id());
            Scheduler *scheduler = Scheduler::get_this();
            if (scheduler) {
                scheduler->schedule(fiber);
            }
            else {
                ZCOROUTINE_LOG_WARN("FdContext::cancel_event no scheduler available: fd={}, event={}", fd_, event);
            }
        }
        else {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event no callback or fiber: fd={}, event={}", fd_, event);
        }

        return new_events;
    }

    void FdContext::cancel_all() {
        //读/写事件的回调和协程
        std::function<void()> read_callback = nullptr;
        Fiber::ptr read_fiber = nullptr;
        std::function<void()> write_callback = nullptr;
        Fiber::ptr write_fiber = nullptr;
        {
            int write_triggered = 0;
            int read_triggered = 0;
            std::lock_guard<std::mutex> lock(mutex_);

            if (events_ == kNone) {
                ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all no events to cancel: fd={}", fd_);
                return;
            }

            int old_events = events_;

            // 取消读事件
            if (events_ & kRead) {
                EventContext &ctx = read_ctx_;
                read_callback = std::move(ctx.callback);
                read_fiber = std::move(ctx.fiber);
                reset_event_context(ctx);
                if (read_callback || read_fiber) {
                    read_triggered = 1;
                }
            }

            // 取消写事件
            if (events_ & kWrite) {
                EventContext &ctx = write_ctx_;
                write_callback = std::move(ctx.callback);
                write_fiber = std::move(ctx.fiber);
                reset_event_context(ctx);
                if (write_callback || write_fiber) {
                    write_triggered = 1;
                }
            }

            events_ = kNone;

            ZCOROUTINE_LOG_DEBUG(
                "FdContext::cancel_all complete: fd={}, old_events={}, read_triggered={}, write_triggered={}",
                fd_, old_events, read_triggered, write_triggered);
        }

        Scheduler *scheduler = Scheduler::get_this();

        if (read_callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all executing READ callback: fd={}", fd_);
            read_callback();
        }
        else if (read_fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all scheduling READ fiber: fd={}, fiber_id={}",
                                 fd_, read_fiber->id());
            if (scheduler) {
                scheduler->schedule(read_fiber);
            }
            else {
                ZCOROUTINE_LOG_WARN("FdContext::cancel_all no scheduler for READ fiber: fd={}", fd_);
            }
        }

        if (write_callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all executing WRITE callback: fd={}", fd_);
            write_callback();
        }
        else if (write_fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all scheduling WRITE fiber: fd={}, fiber_id={}",
                                 fd_, write_fiber->id());
            if (scheduler) {
                scheduler->schedule(write_fiber);
            }
            else {
                ZCOROUTINE_LOG_WARN("FdContext::cancel_all no scheduler for WRITE fiber: fd={}", fd_);
            }
        }
    }

    void FdContext::trigger_event(Event event) {
        std::function<void()> callback = nullptr;
        Fiber::ptr fiber = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 检查事件是否存在
            if (!(events_ & event)) {
                ZCOROUTINE_LOG_DEBUG(
                    "FdContext::trigger_event event not registered: fd={}, event={}, current_events={}",
                    fd_, event, events_);
                return;
            }

            // 获取事件上下文
            EventContext &ctx = get_event_context(event);

            // 先取出callback和fiber，避免回调中重新注册时被后续del_event清空
            callback = std::move(ctx.callback);
            fiber = std::move(ctx.fiber);

            // 删除事件标志
            int old_events = events_;
            events_ = events_ & ~event;

            ZCOROUTINE_LOG_DEBUG(
                "FdContext::trigger_event deleted event: fd={}, event={}, old_events={}, new_events={}",
                fd_, event, old_events, events_);
        }


        // 触发回调或调度协程
        if (callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event executing callback: fd={}, event={}", fd_, event);
            callback();
        }
        else if (fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event scheduling fiber: fd={}, event={}, fiber_id={}",
                                 fd_, event, fiber->id());
            Scheduler *scheduler = Scheduler::get_this();
            if (scheduler) {
                scheduler->schedule(fiber);
            }
            else {
                ZCOROUTINE_LOG_WARN("FdContext::trigger_event no scheduler available: fd={}, event={}", fd_, event);
            }
        }
        else {
            ZCOROUTINE_LOG_WARN("FdContext::trigger_event no callback or fiber: fd={}, event={}", fd_, event);
        }
    }

    FdContext::EventContext &FdContext::get_event_context(Event event) {
        if (event == kRead) {
            return read_ctx_;
        }
        else if (event == kWrite) {
            return write_ctx_;
        }

        // 不应该到达这里
        ZCOROUTINE_LOG_ERROR("FdContext::get_event_context invalid event: fd={}, event={}", fd_, event);
        return read_ctx_;
    }

    void FdContext::reset_event_context(EventContext &ctx) {
        bool had_fiber = ctx.fiber != nullptr;
        bool had_callback = ctx.callback != nullptr;

        ctx.fiber.reset();
        ctx.callback = nullptr;

        if (had_fiber || had_callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::reset_event_context: fd={}, had_fiber={}, had_callback={}",
                                 fd_, had_fiber, had_callback);
        }
    }
} // namespace zcoroutine
