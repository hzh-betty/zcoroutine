#include "scheduling/scheduler.h"

#include <utility>

#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"

namespace zcoroutine {

Scheduler::Scheduler(int thread_count, std::string name, bool use_shared_stack)
    : name_(std::move(name)), thread_count_(thread_count),
      task_queue_(std::make_unique<TaskQueue>()), stopping_(true),
      active_thread_count_(0), idle_thread_count_(0),
      use_shared_stack_(use_shared_stack) {

  // 创建主协程（保存线程的原始上下文）
  // 注意：必须使用shared_ptr管理，因为ThreadContext使用weak_ptr持有
  static Fiber::ptr main_fiber(new Fiber());
  ThreadContext::set_main_fiber(main_fiber);
  ThreadContext::set_current_fiber(main_fiber);

  // 如果使用共享栈模式，创建共享栈
  if (use_shared_stack_) {
    shared_stack_ = std::make_shared<SharedStack>();
    ZCOROUTINE_LOG_INFO("Scheduler[{}] using shared stack mode with {} buffers",
                        name_, shared_stack_->count());
  }

  ZCOROUTINE_LOG_INFO(
      "Scheduler[{}] created with thread_count={}, shared_stack={}", name_,
      thread_count_, use_shared_stack_);
}

Scheduler::~Scheduler() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] destroying", name_);
  stop();
  ZCOROUTINE_LOG_INFO("Scheduler[{}] destroyed", name_);
}

void Scheduler::start() {
  stopping_ = false;

  if (!threads_.empty()) {
    ZCOROUTINE_LOG_WARN("Scheduler[{}] already started, skip", name_);
    return;
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] starting with {} threads...", name_,
                      thread_count_);

  // 创建工作线程
  threads_.reserve(thread_count_);
  for (int i = 0; i < thread_count_; ++i) {
    auto thread = std::make_unique<std::thread>([this, i]() {
      // 设置线程的调度器
      set_this(this);

      ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} started", name_, i);
      this->run();
      ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} exited", name_, i);
    });
    threads_.push_back(std::move(thread));
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] started successfully with {} threads",
                      name_, thread_count_);
}

void Scheduler::stop() {
  if (stopping_) {
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] already stopping, skip", name_);
    return; // 已经在停止中
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] stopping, waiting for {} pending tasks...",
                      name_, task_queue_->size());

  // 等待所有任务处理完成
  while (!task_queue_->empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] all tasks completed, stopping task queue",
                      name_);

  stopping_ = true;

  // 停止任务队列，唤醒所有等待的线程
  task_queue_->stop();

  // 等待所有线程结束
  for (size_t i = 0; i < threads_.size(); ++i) {
    auto &thread = threads_[i];
    if (thread && thread->joinable()) {
      thread->join();
      ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} joined", name_, i);
    }
  }
  threads_.clear();

  ZCOROUTINE_LOG_INFO("Scheduler[{}] stopped successfully", name_);
}

void Scheduler::schedule(Fiber::ptr fiber) {
  if (!fiber) {
    ZCOROUTINE_LOG_WARN("Scheduler[{}]::schedule received null fiber", name_);
    return;
  }

  ZCOROUTINE_LOG_DEBUG(
      "Scheduler[{}] scheduled fiber name={}, id={}, queue_size={}", name_,
      fiber->name(), fiber->id(), task_queue_->size());

  task_queue_->push(Task(std::move(fiber)));
}

Scheduler *Scheduler::get_this() { return ThreadContext::get_scheduler(); }

void Scheduler::set_this(Scheduler *scheduler) {
  ThreadContext::set_scheduler(scheduler);
}

void Scheduler::run() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread entering run loop", name_);

  // 如果使用共享栈模式，设置线程本地的栈模式和共享栈
  if (use_shared_stack_ && shared_stack_) {
    ThreadContext::set_stack_mode(StackMode::kShared);
    ThreadContext::set_shared_stack(shared_stack_);
  }

  // 创建调度器协程，它将运行调度循环
  auto scheduler_fiber =
      std::make_shared<Fiber>([this]() { this->schedule_loop(); },
                              StackAllocator::kDefaultStackSize, "scheduler");
  ThreadContext::set_scheduler_fiber(scheduler_fiber);

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] main_fiber and scheduler_fiber created",
                       name_);

  // 启动调度器协程
  try {
    scheduler_fiber->resume();
  } catch (const std::exception &e) {
    ZCOROUTINE_LOG_ERROR(
        "Scheduler[{}] fiber execution exception: name={}, id={}, error={}",
        name_, scheduler_fiber->name(), scheduler_fiber->id(), e.what());
  } catch (...) {
    ZCOROUTINE_LOG_ERROR(
        "Scheduler[{}] fiber execution unknown exception: name={}, id={}",
        name_, scheduler_fiber->name(), scheduler_fiber->id());
  }

  // 调度器协程结束后，清理
  ThreadContext::set_scheduler_fiber(nullptr);
  ThreadContext::set_main_fiber(nullptr);
  ThreadContext::set_current_fiber(nullptr);

  // 如果使用了共享栈模式，重置线程本地配置
  if (use_shared_stack_) {
    ThreadContext::reset_shared_stack_config();
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread exiting run loop", name_);
}

void Scheduler::schedule_loop() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] schedule_loop starting", name_);

  // 批量处理优化：减少锁竞争
  static constexpr size_t kBatchSize = 8;
  Task tasks[kBatchSize];
  
  while (true) {
    // 如果正在停止且任务队列为空，则退出循环
    if (stopping_ && task_queue_->empty())
      break;

    // 尝试批量获取任务
    size_t batch_count = 0;
    for (size_t i = 0; i < kBatchSize; ++i) {
      if (task_queue_->try_pop(tasks[i])) {
        ++batch_count;
      } else {
        break;
      }
    }
    
    // 如果批量获取失败，阻塞等待一个任务
    if (batch_count == 0) {
      Task task;
      if (!task_queue_->pop(task)) {
        // 队列已停止
        ZCOROUTINE_LOG_DEBUG(
            "Scheduler[{}] task queue stopped, exiting schedule_loop", name_);
        break;
      }
      
      if (!task.is_valid()) {
        ZCOROUTINE_LOG_DEBUG("Scheduler[{}] received invalid task, skipping",
                             name_);
        continue;
      }
      
      tasks[0] = std::move(task);
      batch_count = 1;
    }

    // 批量执行任务
    for (size_t i = 0; i < batch_count; ++i) {
      Task &task = tasks[i];
      
      if (!task.is_valid()) {
        continue;
      }

      // 增加活跃线程计数
      int active =
          active_thread_count_.fetch_add(1, std::memory_order_relaxed) + 1;

      // 执行任务
      if (task.fiber) {
        // 执行协程
        const Fiber::ptr fiber = task.fiber;

        ZCOROUTINE_LOG_DEBUG(
            "Scheduler[{}] executing fiber name={}, id={}, active_threads={}",
            name_, fiber->name(), fiber->id(), active);

        try {
          fiber->resume();
        } catch (const std::exception &e) {
          ZCOROUTINE_LOG_ERROR(
              "Scheduler[{}] fiber execution exception: name={}, id={}, error={}",
              name_, fiber->name(), fiber->id(), e.what());
        } catch (...) {
          ZCOROUTINE_LOG_ERROR(
              "Scheduler[{}] fiber execution unknown exception: name={}, id={}",
              name_, fiber->name(), fiber->id());
        }

        // 如果协程终止，归还到池中
        if (fiber->state() == Fiber::State::kTerminated) {
          ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber terminated: name={}, id={}",
                               name_, fiber->name(), fiber->id());
        }
        // 如果协程挂起，说明在等待外部事件（IO、定时器等）
        else if (fiber->state() == Fiber::State::kSuspended) {
          ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber suspended, waiting for "
                               "external event: name={}, id={}",
                               name_, fiber->name(), fiber->id());
        }
      } else if (task.callback) {
        // 执行回调函数
        ZCOROUTINE_LOG_DEBUG(
            "Scheduler[{}] executing callback, active_threads={}", name_, active);

        try {
          task.callback();
        } catch (const std::exception &e) {
          ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback exception: error={}",
                               name_, e.what());
        } catch (...) {
          ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback unknown exception", name_);
        }
      }

      // 减少活跃线程计数
      active_thread_count_.fetch_sub(1, std::memory_order_relaxed);
      
      // 清理任务
      task.reset();
    }
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] schedule_loop ended", name_);
}

} // namespace zcoroutine
