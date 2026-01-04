/**
 * @file fiber_pool_test.cc
 * @brief FiberPool 类单元测试
 */

#include "runtime/fiber_pool.h"
#include "util/zcoroutine_logger.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace zcoroutine;

class FiberPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 每个测试前清空池
    auto &pool = FiberPool::get_instance();
    pool.clear();
    pool.set_max_capacity(1000);
  }

  void TearDown() override {
    // 每个测试后清理
    auto &pool = FiberPool::get_instance();
    pool.clear();
  }
};

// ============================================================================
// 基本功能测试
// ============================================================================

// 测试 1: 从池中获取协程（第一次 - 应该创建新的）
TEST_F(FiberPoolTest, GetFiberFirstTime) {
  auto &pool = FiberPool::get_instance();

  bool executed = false;
  auto fiber = pool.get_fiber([&executed]() { executed = true; });

  EXPECT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  EXPECT_FALSE(executed);

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试 2: 归还协程到池中
TEST_F(FiberPoolTest, ReturnFiber) {
  auto &pool = FiberPool::get_instance();

  auto fiber = pool.get_fiber([]() {});
  fiber->resume();

  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
  EXPECT_EQ(pool.size(), 0);

  bool returned = pool.return_fiber(fiber);
  EXPECT_TRUE(returned);
  EXPECT_EQ(pool.size(), 1);
}

// 测试 3: 从池中复用协程
TEST_F(FiberPoolTest, ReuseFiber) {
  auto &pool = FiberPool::get_instance();

  // 创建并返回第一个协程
  auto fiber1 = pool.get_fiber([]() {});
  fiber1->resume();
  pool.return_fiber(fiber1);

  uint64_t fiber1_id = fiber1.get()->id();

  // 获取第二个协程（应该复用 fiber1）
  int count = 0;
  auto fiber2 = pool.get_fiber([&count]() { count++; });

  EXPECT_EQ(pool.size(), 0); // 获取后池应该为空

  fiber2->resume();
  EXPECT_EQ(count, 1);
  EXPECT_EQ(fiber2->state(), Fiber::State::kTerminated);
}

// 测试 4: 不能归还未终止的协程
TEST_F(FiberPoolTest, CannotReturnNonTerminatedFiber) {
  auto &pool = FiberPool::get_instance();

  auto fiber = pool.get_fiber([&]() { Fiber::yield(); });

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  bool returned = pool.return_fiber(fiber);
  EXPECT_FALSE(returned);
  EXPECT_EQ(pool.size(), 0);
}

// 测试 5: 归还空协程
TEST_F(FiberPoolTest, ReturnNullFiber) {
  auto &pool = FiberPool::get_instance();

  Fiber::ptr null_fiber = nullptr;
  bool returned = pool.return_fiber(null_fiber);

  EXPECT_FALSE(returned);
  EXPECT_EQ(pool.size(), 0);
}

// ============================================================================
// 容量管理测试
// ============================================================================

// 测试 6: 设置和获取最大容量
TEST_F(FiberPoolTest, SetMaxCapacity) {
  auto &pool = FiberPool::get_instance();

  pool.set_max_capacity(100);
  EXPECT_EQ(pool.get_max_capacity(), 100);

  pool.set_max_capacity(500);
  EXPECT_EQ(pool.get_max_capacity(), 500);
}

// 测试 7: 池达到最大容量
TEST_F(FiberPoolTest, PoolMaxCapacity) {
  auto &pool = FiberPool::get_instance();
  pool.set_max_capacity(3);

  std::vector<Fiber::ptr> fibers;
  for (int i = 0; i < 5; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    fibers.push_back(fiber);
  }

  // Return all fibers
  for (auto &fiber : fibers) {
    pool.return_fiber(fiber);
  }

  // Pool should only contain max_capacity fibers
  EXPECT_EQ(pool.size(), 3);
}

// 测试 8: 缩小容量会移除多余的协程
TEST_F(FiberPoolTest, ShrinkCapacity) {
  auto &pool = FiberPool::get_instance();
  pool.set_max_capacity(10);

  // 创建并返回 5 个协程（必须保持引用以创建不同的协程）
  std::vector<Fiber::ptr> fibers;
  for (int i = 0; i < 5; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    fibers.push_back(fiber);
  }

  // 一次性返回所有协程
  for (auto &fiber : fibers) {
    pool.return_fiber(fiber);
  }

  EXPECT_EQ(pool.size(), 5);

  // 缩小容量到 3
  pool.set_max_capacity(3);
  EXPECT_EQ(pool.size(), 3);
}

// 测试 9: 清空池
TEST_F(FiberPoolTest, ClearPool) {
  auto &pool = FiberPool::get_instance();

  // 创建并返回多个协程
  for (int i = 0; i < 10; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    pool.return_fiber(fiber);
  }

  EXPECT_GT(pool.size(), 0);

  pool.clear();
  EXPECT_EQ(pool.size(), 0);
}

// ============================================================================
// 统计测试
// ============================================================================

// 测试 10: 跟踪创建总数
TEST_F(FiberPoolTest, TotalCreated) {
  auto &pool = FiberPool::get_instance();

  uint64_t initial_created = pool.total_created();

  for (int i = 0; i < 5; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
  }

  EXPECT_EQ(pool.total_created(), initial_created + 5);
}

// 测试 11: 跟踪复用总数
TEST_F(FiberPoolTest, TotalReused) {
  auto &pool = FiberPool::get_instance();

  // Create and return fibers
  for (int i = 0; i < 3; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    pool.return_fiber(fiber);
  }

  uint64_t initial_reused = pool.total_reused();

  // Reuse fibers
  for (int i = 0; i < 5; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    pool.return_fiber(fiber);
  }

  // Should have reused min(3, 5) = 3 times from existing pool
  // Plus created 2 new ones
  EXPECT_GE(pool.total_reused(), initial_reused);
}

// 测试 12: 命中率计算
TEST_F(FiberPoolTest, HitRate) {
  auto &pool = FiberPool::get_instance();

  // 初始时没有命中
  EXPECT_EQ(pool.hit_rate(), 0.0);

  // 创建 2 个不同的协程并返回到池中
  std::vector<Fiber::ptr> fibers;
  for (int i = 0; i < 2; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    fibers.push_back(fiber);
  }
  for (auto &fiber : fibers) {
    pool.return_fiber(fiber);
  }

  // 现在池中有 2 个协程。多次获取并返回它们以测试复用
  for (int i = 0; i < 4; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    pool.return_fiber(fiber);
  }

  // 命中率应该非零（至少 6 次获取中的 2 次被复用）
  EXPECT_GT(pool.hit_rate(), 0.0);
  EXPECT_LE(pool.hit_rate(), 1.0);
}

// 测试 13: 并发获取和归还
TEST_F(FiberPoolTest, ConcurrentGetReturn) {
  auto &pool = FiberPool::get_instance();
  pool.set_max_capacity(200);

  const int thread_count = 4;
  const int operations_per_thread = 50;
  std::vector<std::thread> threads;
  std::atomic<int> total_executed{0};

  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&pool, &total_executed, operations_per_thread]() {
      for (int i = 0; i < operations_per_thread; ++i) {
        auto fiber = pool.get_fiber([&total_executed]() {
          total_executed.fetch_add(1, std::memory_order_relaxed);
        });

        fiber->resume();
        pool.return_fiber(fiber);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(total_executed.load(), thread_count * operations_per_thread);
}

// 测试 14: 并发容量变化
TEST_F(FiberPoolTest, ConcurrentCapacityChanges) {
  auto &pool = FiberPool::get_instance();

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;

  // 线程 1: 获取并返回协程
  threads.emplace_back([&pool, &stop]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto fiber = pool.get_fiber([]() {});
      fiber->resume();
      pool.return_fiber(fiber);
    }
  });

  // 线程 2: 更改容量
  threads.emplace_back([&pool, &stop]() {
    for (int i = 0; i < 10 && !stop.load(std::memory_order_relaxed); ++i) {
      pool.set_max_capacity(50 + i * 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // 线程 3: 检查大小
  threads.emplace_back([&pool, &stop]() {
    for (int i = 0; i < 20 && !stop.load(std::memory_order_relaxed); ++i) {
      volatile size_t size = pool.size();
      (void)size;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  // 让它们运行
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true, std::memory_order_relaxed);

  for (auto &t : threads) {
    t.join();
  }

  // 没有崩溃意味着成功
  SUCCEED();
}

// 测试 15: 大量协程
TEST_F(FiberPoolTest, ManyFibers) {
  auto &pool = FiberPool::get_instance();
  pool.set_max_capacity(500);

  const int fiber_count = 1000;
  std::atomic<int> counter{0};

  for (int i = 0; i < fiber_count; ++i) {
    auto fiber = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    fiber->resume();
    pool.return_fiber(fiber);
  }

  EXPECT_EQ(counter.load(), fiber_count);
  EXPECT_LE(pool.size(), 500); // 应该遵守最大容量
}

// 测试 16: 包含 yield 的协程
TEST_F(FiberPoolTest, FiberWithYield) {
  auto &pool = FiberPool::get_instance();

  int step = 0;
  auto fiber = pool.get_fiber([&step]() {
    step = 1;
    Fiber::yield();
    step = 2;
  });

  fiber->resume();
  EXPECT_EQ(step, 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(step, 2);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  pool.return_fiber(fiber);
  EXPECT_EQ(pool.size(), 1);
}

// 测试 17: 零容量（无限制）
TEST_F(FiberPoolTest, UnlimitedCapacity) {
  auto &pool = FiberPool::get_instance();
  pool.set_max_capacity(0); // Unlimited

  const int many_fibers = 100;
  std::vector<Fiber::ptr> fibers;
  for (int i = 0; i < many_fibers; ++i) {
    auto fiber = pool.get_fiber([]() {});
    fiber->resume();
    fibers.push_back(fiber);
  }

  // 将所有协程返回到池中
  for (auto &fiber : fibers) {
    pool.return_fiber(fiber);
  }

  // 应该接受所有协程
  EXPECT_EQ(pool.size(), many_fibers);
}

// 测试 18: 单例实例
TEST_F(FiberPoolTest, SingletonInstance) {
  auto &pool1 = FiberPool::get_instance();
  auto &pool2 = FiberPool::get_instance();

  EXPECT_EQ(&pool1, &pool2);
}

int main(int argc, char **argv) {
  // Initialize logger
  zcoroutine::init_logger(zlog::LogLevel::value::INFO);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
