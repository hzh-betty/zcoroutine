#ifndef ZCOROUTINE_TASK_QUEUE_H_
#define ZCOROUTINE_TASK_QUEUE_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <queue>

#include "runtime/fiber.h"
#include "sync/spinlock.h"

namespace zcoroutine {

/**
 * @brief 任务结构
 * 任务可以是协程或回调函数
 */
struct Task {
  Fiber::ptr fiber;               // 任务协程
  std::function<void()> callback; // 任务回调函数

  Task() = default;

  explicit Task(Fiber::ptr f) : fiber(std::move(f)) {}

  explicit Task(std::function<void()> cb) : callback(std::move(cb)) {}

  /**
   * @brief 重置任务
   */
  void reset() {
    fiber = nullptr;
    callback = nullptr;
  }

  /**
   * @brief 任务是否有效
   */
  bool is_valid() const { return fiber != nullptr || callback != nullptr; }
};

/**
 * @brief 任务队列类
 * 使用SpinLock + Linux信号量实现的线程安全任务队列
 * SpinLock保护队列操作，信号量实现阻塞等待
 */
class TaskQueue {
public:
  TaskQueue() = default;
  ~TaskQueue() = default;

  /**
   * @brief 添加任务
   * @param task 任务对象
   */
  void push(const Task &task);

  /**
   * @brief 阻塞取出任务
   * @param task 输出参数，取出的任务
   * @return true表示成功取出，false表示队列已停止
   */
  bool pop(Task &task);

  /**
   * @brief 获取队列大小
   * @return 队列中任务数量
   */
  size_t size() const;

  /**
   * @brief 判断队列是否为空
   * @return true表示空，false表示非空
   */
  bool empty() const;

  /**
   * @brief 停止队列
   * 唤醒所有等待的线程
   */
  void stop();

private:
  mutable Spinlock spinlock_;        // 自旋锁保护队列
  std::condition_variable_any cv_;   // 条件变量
  std::queue<Task> tasks_;           // 任务队列
  std::atomic<bool> stopped_{false}; // 停止标志
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TASK_QUEUE_H_
