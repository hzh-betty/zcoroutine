#include "timer/timer_manager.h"

#include <sys/time.h>

#include "util/zcoroutine_logger.h"

namespace zcoroutine {

// 获取当前时间（毫秒）
static uint64_t get_current_ms() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

Timer::ptr TimerManager::add_timer(uint64_t timeout, std::function<void()> callback, bool recurring) {
    if (!callback) {
        ZCOROUTINE_LOG_WARN("TimerManager::add_timer: null callback provided, timeout={}ms", timeout);
        // 仍然创建 Timer，但会在 execute 时被忽略
    }
    
    auto timer = std::make_shared<Timer>(timeout, callback, recurring);
    
    std::lock_guard<std::mutex> lock(mutex_);
    timers_.insert(timer);
    
    ZCOROUTINE_LOG_DEBUG("TimerManager::add_timer: timeout={}ms, recurring={}, next_time={}, total_timers={}", 
                         timeout, recurring, timer->get_next_time(), timers_.size());
    return timer;
}

Timer::ptr TimerManager::add_condition_timer(uint64_t timeout, std::function<void()> callback,
                                             std::weak_ptr<void> weak_cond, bool recurring) {
    if (!callback) {
        ZCOROUTINE_LOG_WARN("TimerManager::add_condition_timer: null callback provided, timeout={}ms", timeout);
        return add_timer(timeout, nullptr, recurring);
    }
    
    // 包装回调函数，添加条件检查
    auto wrapper_callback = [weak_cond, callback]() {
        // 检查条件是否仍然有效
        if (weak_cond.lock()) {
            callback();
        }
    };
    
    ZCOROUTINE_LOG_DEBUG("TimerManager::add_condition_timer: timeout={}ms, recurring={}", timeout, recurring);
    return add_timer(timeout, wrapper_callback, recurring);
}

int TimerManager::get_next_timeout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (timers_.empty()) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: no timers, returning -1");
        return -1;
    }
    
    uint64_t now = get_current_ms();
    auto it = timers_.begin();
    uint64_t next_time = (*it)->get_next_time();
    
    if (next_time <= now) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: timer already expired, returning 0");
        return 0;  // 已经到期
    }
    
    int timeout = static_cast<int>(next_time - now);
    ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: next_timeout={}ms, total_timers={}", 
                         timeout, timers_.size());
    return timeout;
}

std::vector<std::function<void()>> TimerManager::list_expired_callbacks() {
    std::vector<std::function<void()>> callbacks;
    uint64_t now = get_current_ms();
    ZCOROUTINE_LOG_DEBUG("TimerManager::list_expired_callbacks: checking for expired timers at time={}ms", now);

    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t initial_count = timers_.size();
    size_t expired_count = 0;
    size_t cancelled_count = 0;
    
    auto it = timers_.begin();
    while (it != timers_.end()) {
        if ((*it)->get_next_time() > now) {
            ZCOROUTINE_LOG_DEBUG("TimerManager::list_expired_callbacks: "
                                "timer expired at time={}ms",(*it)->get_next_time());
            break;
        }
        
        auto timer = *it;
        it = timers_.erase(it);
        
        if (!timer->cancelled_) {
            callbacks.emplace_back([timer]() {
                timer->execute();
            });
            expired_count++;
            
            // 如果是循环定时器，重新插入
            if (timer->is_recurring() && !timer->cancelled_) {
                timers_.insert(timer);
            }
        } else {
            cancelled_count++;
        }
    }
    
    if (expired_count > 0 || cancelled_count > 0) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::list_expired_callbacks: expired={}, cancelled={}, remaining={}, initial={}", 
                             expired_count, cancelled_count, timers_.size(), initial_count);
    }
    
    return callbacks;
}

} // namespace zcoroutine
