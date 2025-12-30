#include "scheduling/task_queue.h"
#include <mutex>

namespace zcoroutine {

void TaskQueue::push(const Task &task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(task);
  }
  cv_.notify_one();
}

void TaskQueue::push(Task &&task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

bool TaskQueue::pop(Task &task) {
  std::unique_lock<Spinlock> lock(spinlock_);
  cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });

  if (!tasks_.empty()) {
    task = std::move(tasks_.front());
    tasks_.pop();
    return true;
  }

  return false;
}

size_t TaskQueue::size() const {
  std::lock_guard<Spinlock> lock(spinlock_);
  return tasks_.size();
}

bool TaskQueue::empty() const {
  std::lock_guard<Spinlock> lock(spinlock_);
  return tasks_.empty();
}

void TaskQueue::stop() {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    stopped_ = true;
  }
  cv_.notify_all();
}

} // namespace zcoroutine
