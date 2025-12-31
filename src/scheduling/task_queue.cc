#include "scheduling/task_queue.h"
#include <chrono>
#include <mutex>

namespace zcoroutine {

void TaskQueue::push(const Task &task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(task);
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
}

void TaskQueue::push(Task &&task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(std::move(task));
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
}

bool TaskQueue::try_pop(Task &task) {
  // 快速路径：先检查size，避免无谓加锁
  if (size_.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  
  std::lock_guard<Spinlock> lock(spinlock_);
  if (!tasks_.empty()) {
    task = std::move(tasks_.front());
    tasks_.pop();
    size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool TaskQueue::pop(Task &task, int timeout_ms) {
  // 先尝试快速路径
  if (try_pop(task)) {
    return true;
  }
  
  // 快速路径失败，进入阻塞等待
  std::unique_lock<Spinlock> lock(spinlock_);
  
  if (timeout_ms > 0) {
    // 带超时等待
    auto deadline = std::chrono::steady_clock::now() + 
                    std::chrono::milliseconds(timeout_ms);
    if (!cv_.wait_until(lock, deadline, [this] { return stopped_ || !tasks_.empty(); })) {
      // 超时
      return false;
    }
  } else {
    // 永久等待
    cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
  }

  if (!tasks_.empty()) {
    task = std::move(tasks_.front());
    tasks_.pop();
    size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

size_t TaskQueue::size() const {
  return size_.load(std::memory_order_relaxed);
}

bool TaskQueue::empty() const {
  return size_.load(std::memory_order_relaxed) == 0;
}

void TaskQueue::stop() {
  stopped_.store(true, std::memory_order_relaxed);
  cv_.notify_all();
}

} // namespace zcoroutine
