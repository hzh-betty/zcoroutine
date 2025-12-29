#ifndef ZCOROUTINE_SEMAPHORE_H_
#define ZCOROUTINE_SEMAPHORE_H_

#include <semaphore.h>

#include "util/noncopyable.h"


namespace zcoroutine {

/**
 * @brief 信号量类
 * 使用 Linux 原生 sem_t 实现
 */
class Semaphore : public NonCopyable {
public:
  /**
   * @brief 构造函数
   * @param count 初始计数值
   */
  explicit Semaphore(unsigned int count = 0) {
    sem_init(&semaphore_, 0, count);
  }

  ~Semaphore() { sem_destroy(&semaphore_); }

  /**
   * @brief 等待信号量（P操作）
   * 阻塞直到信号量大于0，然后减1
   */
  void wait() {
    while (sem_wait(&semaphore_) != 0) {
      // 如果被信号中断，重试
    }
  }

  /**
   * @brief 发送信号量（V操作）
   * 信号量加1，唤醒等待的线程
   */
  void notify() { sem_post(&semaphore_); }

  /**
   * @brief 批量发送信号量
   * @param count 发送次数
   */
  void notify_all(int count) {
    for (int i = 0; i < count; ++i) {
      sem_post(&semaphore_);
    }
  }

private:
  sem_t semaphore_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SEMAPHORE_H_
