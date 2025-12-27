#include "scheduling/task_queue.h"

namespace zcoroutine {

void TaskQueue::push(const Task &task) {
  {
    SpinlockGuard lock(spinlock_);
    tasks_.push_back(task);
  }
  semaphore_.notify(); // 唤醒一个等待的线程
}

bool TaskQueue::pop(Task &task) {
  while (true) {
    // 等待信号量
    semaphore_.wait();

    // 检查停止标志
    if (stopped_) {
      // 即使停止，也要处理完剩余任务
      SpinlockGuard lock(spinlock_);
      if (tasks_.empty()) {
        return false;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      return true;
    }

    SpinlockGuard lock(spinlock_);
    if (!tasks_.empty()) {
      task = std::move(tasks_.front());
      tasks_.pop_front();
      return true;
    }
    // 如果队列为空，继续等待（spurious wakeup）
  }
}

size_t TaskQueue::size() const {
  SpinlockGuard lock(spinlock_);
  return tasks_.size();
}

bool TaskQueue::empty() const {
  SpinlockGuard lock(spinlock_);
  return tasks_.empty();
}

void TaskQueue::stop() {
  stopped_ = true;
  // 唤醒所有等待的线程
  semaphore_.notify_all(1024);
}

} // namespace zcoroutine
