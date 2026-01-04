#include "runtime/fiber_pool.h"

#include <algorithm>
#include <mutex>

#include "util/zcoroutine_logger.h"

namespace zcoroutine {

// 默认最大容量：1000个协程
static constexpr size_t kDefaultMaxCapacity = 1000;

FiberPool::FiberPool()
    : max_capacity_(kDefaultMaxCapacity), total_created_(0), total_reused_(0) {
  ZCOROUTINE_LOG_INFO("FiberPool initialized with max_capacity={}",
                      max_capacity_);
}

FiberPool &FiberPool::get_instance() {
  static FiberPool instance;
  return instance;
}

Fiber::ptr FiberPool::get_fiber(std::function<void()> func, size_t stack_size,
                                const std::string &name,
                                bool use_shared_stack) {
  Fiber::ptr fiber = nullptr;

  // 尝试从池中获取可复用的协程
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找栈大小匹配的协程
    auto it = std::find_if(pool_.begin(), pool_.end(),
                           [stack_size, use_shared_stack](const Fiber::ptr &f) {
                             // 检查栈模式是否匹配
                             bool stack_mode_match =
                                 (use_shared_stack && f->is_shared_stack()) ||
                                 (!use_shared_stack && !f->is_shared_stack());

                             if (use_shared_stack) {
                               return stack_mode_match;
                             } else {
                               return stack_mode_match;
                             }
                           });

    if (it != pool_.end()) {
      fiber = *it;
      pool_.erase(it);
      total_reused_.fetch_add(1, std::memory_order_relaxed);

      ZCOROUTINE_LOG_DEBUG("FiberPool: reused fiber, fiber_id={}, "
                           "pool_size={}, hit_rate={:.2f}%",
                           fiber->id(), pool_.size(), hit_rate() * 100.0);
    }
  }

  // 如果池中没有可用的协程，创建新的
  if (!fiber) {
    fiber = std::make_shared<Fiber>(func, stack_size, name, use_shared_stack);
    total_created_.fetch_add(1, std::memory_order_relaxed);

    ZCOROUTINE_LOG_DEBUG("FiberPool: created new fiber, fiber_id={}, "
                         "stack_size={}, total_created={}",
                         fiber->id(), stack_size, total_created_.load());
  } else {
    // 从池中复用协程，重置为新任务
    fiber->reset(func);

    ZCOROUTINE_LOG_DEBUG("FiberPool: reused fiber after reset, fiber_id={}, "
                         "pool_size={}",
                         fiber->id(), pool_.size());
  }

  return fiber;
}

bool FiberPool::return_fiber(const Fiber::ptr &fiber) {
  if (!fiber) {
    ZCOROUTINE_LOG_WARN("FiberPool::return_fiber: null fiber");
    return false;
  }

  // 只有终止状态的协程才能归还
  if (fiber->state() != Fiber::State::kTerminated) {
    ZCOROUTINE_LOG_WARN("FiberPool::return_fiber: fiber not terminated, "
                        "fiber_id={}, state={}",
                        fiber->id(), Fiber::state_to_string(fiber->state()));
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // 检查池是否已满
  if (max_capacity_ > 0 && pool_.size() >= max_capacity_) {
    ZCOROUTINE_LOG_DEBUG("FiberPool::return_fiber: pool full, discard "
                         "fiber_id={}, pool_size={}, max_capacity={}",
                         fiber->id(), pool_.size(), max_capacity_);
    return false;
  }

  // 归还到池中
  pool_.push_back(fiber);

  ZCOROUTINE_LOG_DEBUG("FiberPool::return_fiber: fiber_id={}, pool_size={}",
                       fiber->id(), pool_.size());
  return true;
}

void FiberPool::set_max_capacity(size_t capacity) {
  std::lock_guard<std::mutex> lock(mutex_);
  max_capacity_ = capacity;

  // 如果当前池大小超过新容量，移除多余的协程
  if (max_capacity_ > 0 && pool_.size() > max_capacity_) {
    size_t to_remove = pool_.size() - max_capacity_;
    pool_.erase(pool_.end() - to_remove, pool_.end());

    ZCOROUTINE_LOG_INFO(
        "FiberPool::set_max_capacity: capacity={}, removed {} fibers",
        max_capacity_, to_remove);
  }
}

size_t FiberPool::get_max_capacity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return max_capacity_;
}

size_t FiberPool::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pool_.size();
}

void FiberPool::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t old_size = pool_.size();
  pool_.clear();

  total_created_.store(0, std::memory_order_relaxed);
  total_reused_.store(0, std::memory_order_relaxed);

  ZCOROUTINE_LOG_INFO("FiberPool::clear: cleared {} fibers", old_size);
}

double FiberPool::hit_rate() const {
  uint64_t created = total_created_.load(std::memory_order_relaxed);
  uint64_t reused = total_reused_.load(std::memory_order_relaxed);

  uint64_t total_get = created + reused;
  if (total_get == 0) {
    return 0.0;
  }

  return static_cast<double>(reused) / static_cast<double>(total_get);
}

} // namespace zcoroutine
