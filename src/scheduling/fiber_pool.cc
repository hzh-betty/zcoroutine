#include "scheduling/fiber_pool.h"

#include "util/zcoroutine_logger.h"

namespace zcoroutine {

FiberPool::ptr FiberPool::GetInstance(size_t min_size, size_t max_size) {
    static ptr instance(new FiberPool(min_size, max_size));
    return instance;
}

FiberPool::FiberPool(size_t min_size, size_t max_size)
    : min_size_(min_size)
    , max_size_(max_size)
    , total_created_(0)
    , total_reused_(0) {
    
    ZCOROUTINE_LOG_INFO("FiberPool created: min_size={}, max_size={}", min_size, max_size);
}

FiberPool::~FiberPool() {
    size_t idle_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_count = idle_fibers_.size();
    }
    clear();
    ZCOROUTINE_LOG_INFO("FiberPool destroyed: total_created={}, total_reused={}, final_idle_count={}",
                        total_created_.load(), total_reused_.load(), idle_count);
}

Fiber::ptr FiberPool::acquire(std::function<void()> func) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!idle_fibers_.empty()) {
        // 从池中取出空闲协程
        auto fiber = idle_fibers_.front();
        idle_fibers_.pop_front();
        
        // 重置协程，设置新的执行函数
        fiber->reset(std::move(func));
        
        size_t reused = total_reused_.fetch_add(1, std::memory_order_relaxed) + 1;
        
        ZCOROUTINE_LOG_DEBUG("FiberPool::acquire from pool: fiber_id={}, idle_remaining={}, total_reused={}",
                             fiber->id(), idle_fibers_.size(), reused);
        return fiber;
    }
    
    // 池为空，创建新协程
    size_t created = total_created_.fetch_add(1, std::memory_order_relaxed) + 1;
    ZCOROUTINE_LOG_DEBUG("FiberPool::acquire pool empty, need create new fiber: total_created={}",
                         created);

    auto new_fiber = std::make_shared<Fiber>(std::move(func));
    return new_fiber;
}

void FiberPool::release(Fiber::ptr fiber) {
    if (!fiber) {
        ZCOROUTINE_LOG_WARN("FiberPool::release received null fiber");
        return;
    }
    
    // 检查协程状态
    if (fiber->state() != Fiber::State::kTerminated) {
        ZCOROUTINE_LOG_WARN("FiberPool::release fiber not terminated: fiber_id={}, state={}",
                            fiber->id(), static_cast<int>(fiber->state()));
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查池是否已满
    if (idle_fibers_.size() >= max_size_) {
        ZCOROUTINE_LOG_DEBUG("FiberPool::release pool full, discard fiber: fiber_id={}, pool_size={}, max_size={}",
                             fiber->id(), idle_fibers_.size(), max_size_);
        return;
    }
    
    // 归还到池中
    idle_fibers_.push_back(fiber);
    
    ZCOROUTINE_LOG_DEBUG("FiberPool::release fiber returned to pool: fiber_id={}, idle_count={}",
                         fiber->id(), idle_fibers_.size());
}

void FiberPool::resize(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t old_max_size = max_size_;
    max_size_ = size;
    
    // 如果当前空闲数量超过新的最大值，移除多余的
    size_t removed = 0;
    while (idle_fibers_.size() > max_size_) {
        idle_fibers_.pop_back();
        removed++;
    }
    
    ZCOROUTINE_LOG_INFO("FiberPool::resize: old_max={}, new_max={}, removed={}, idle_count={}",
                        old_max_size, max_size_, removed, idle_fibers_.size());
}

void FiberPool::clear() {
    total_created_.store(0, std::memory_order_relaxed);
    total_reused_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t cleared_count = idle_fibers_.size();
    idle_fibers_.clear();
    ZCOROUTINE_LOG_INFO("FiberPool::clear: cleared {} idle fibers", cleared_count);
}

size_t FiberPool::get_idle_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_fibers_.size();
}

PoolStatistics FiberPool::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PoolStatistics stats;
    stats.total_created = total_created_.load(std::memory_order_relaxed);
    stats.total_reused = total_reused_.load(std::memory_order_relaxed);
    stats.idle_count = idle_fibers_.size();
    
    ZCOROUTINE_LOG_DEBUG("FiberPool::get_statistics: created={}, reused={}, idle={}",
                         stats.total_created, stats.total_reused, stats.idle_count);
    return stats;
}

} // namespace zcoroutine
