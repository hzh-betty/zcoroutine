#ifndef ZCOROUTINE_FIBER_POOL_H_
#define ZCOROUTINE_FIBER_POOL_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "runtime/fiber.h"
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 全局协程池
 *
 * 用于复用已终止的协程对象，
 * 避免频繁创建和销毁协程带来的开销。
 */
class FiberPool : public NonCopyable {
public:
  /**
   * @brief 获取全局单例
   * @return 协程池单例引用
   */
  static FiberPool &get_instance();

  /**
   * @brief 从池中获取或创建一个协程
   * @param func 协程执行函数
   * @param stack_size 栈大小，默认128KB
   * @param name 协程名称前缀，默认为空
   * @param use_shared_stack 是否使用共享栈，默认false
   * @return 协程智能指针
   */
  Fiber::ptr get_fiber(std::function<void()> func,
                       size_t stack_size = StackAllocator::kDefaultStackSize,
                       const std::string &name = "",
                       bool use_shared_stack = false);

  /**
   * @brief 归还协程到池中
   * @param fiber 要归还的协程指针
   * @return true表示成功归还，false表示池已满直接丢弃
   *
   * @note 只有处于Terminated状态的协程才能归还
   * @note 通常不需要手动调用，协程执行完毕后会自动归还
   */
  bool return_fiber(const Fiber::ptr &fiber);

  /**
   * @brief 设置池的最大容量
   * @param capacity 最大容量，0表示不限制
   */
  void set_max_capacity(size_t capacity);

  /**
   * @brief 获取池的最大容量
   * @return 最大容量，0表示不限制
   */
  size_t get_max_capacity() const;

  /**
   * @brief 获取当前池中可用协程数量
   * @return 可用协程数量
   */
  size_t size() const;

  /**
   * @brief 清空池中所有协程
   */
  void clear();

  /**
   * @brief 获取累计创建的协程总数（包括从池中获取和新创建的）
   * @return 累计创建总数
   */
  uint64_t total_created() const {
    return total_created_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 获取累计复用的协程次数
   * @return 累计复用次数
   */
  uint64_t total_reused() const {
    return total_reused_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 获取命中率（复用次数 / 总获取次数）
   * @return 命中率，范围[0.0, 1.0]
   */
  double hit_rate() const;

private:
  FiberPool();
  ~FiberPool() = default;

  // 使用互斥锁保护池（临界区包含 O(n) 操作，互斥锁更合适）
  mutable std::mutex mutex_;

  // 协程池（使用vector存储空闲协程）
  std::vector<Fiber::ptr> pool_;

  // 最大容量，0表示不限制
  size_t max_capacity_;

  // 统计信息
  std::atomic<uint64_t> total_created_; // 累计创建的协程总数
  std::atomic<uint64_t> total_reused_;  // 累计复用的协程次数
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_POOL_H_
