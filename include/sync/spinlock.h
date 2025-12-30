#ifndef ZCOROUTINE_SPINLOCK_H_
#define ZCOROUTINE_SPINLOCK_H_
#include <atomic>
#include <thread>

#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 高性能自旋锁（两阶段 + 自适应 backoff）
 *
 * 设计要点：
 * 1. load(relaxed) 只读自旋，减少 cache line 抖动
 * 2. exchange(acquire) 真正抢锁，建立同步
 * 3. unlock 使用 release，形成 happens-before
 * 4. 短自旋使用 cpu_relax，长自旋让出 CPU

 */
class alignas(64) Spinlock : public NonCopyable {
public:
  Spinlock() noexcept = default;

  void lock() noexcept {
    int spin = 0;

    for (;;) {
      // 第一阶段：只读自旋（不修改 cache line）
      while (locked_.load(std::memory_order_relaxed)) {
        if (spin < kSpinLimit) {
          cpu_relax();
          ++spin;
        } else {
          std::this_thread::yield();
        }
      }

      // 第二阶段：真正抢锁
      if (!locked_.exchange(true, std::memory_order_acquire)) {
        return;
      }
    }
  }

  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
  static constexpr int kSpinLimit = 16;

  std::atomic<bool> locked_{false};

  static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
  }
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SPINLOCK_H_
