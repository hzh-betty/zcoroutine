#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"
#include "util/zcoroutine_logger.h"

using namespace zcoroutine;

class FiberPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 使用单例获取协程池
        pool_ = FiberPool::GetInstance(5, 50);
    }

    void TearDown() override {
        pool_->clear();
    }

    FiberPool::ptr pool_;
};

// ==================== 基础功能测试 ====================

// 测试1：创建协程池
TEST_F(FiberPoolTest, CreatePool) {
    ASSERT_NE(pool_, nullptr);
}

// 测试2：从池中获取协程
TEST_F(FiberPoolTest, AcquireFiber) {
    bool executed = false;
    auto fiber = pool_->acquire([&executed]() {
        executed = true;
    });
    
    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    
    fiber->resume();
    EXPECT_TRUE(executed);
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试3：归还协程到池
TEST_F(FiberPoolTest, ReleaseFiber) {
    size_t before = pool_->get_idle_count();
    auto fiber = pool_->acquire([]() {});
    fiber->resume();
    
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
    
    pool_->release(fiber);
    EXPECT_EQ(pool_->get_idle_count(), before + 1);
}

// 测试4：协程复用
TEST_F(FiberPoolTest, FiberReuse) {
    pool_->clear();
    int count1 = 0;
    int count2 = 0;
    
    // 第一次使用
    auto fiber1 = pool_->acquire([&count1]() { count1++; });
    auto fiber1_id = fiber1->id();
    fiber1->resume();
    pool_->release(fiber1);
    
    // 第二次使用（应该复用）
    auto fiber2 = pool_->acquire([&count2]() { count2++; });
    auto fiber2_id = fiber2->id();
    fiber2->resume();
    
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
    EXPECT_EQ(fiber1_id, fiber2_id); // 复用同一个协程对象
}

// 测试5：多个协程的获取和归还
TEST_F(FiberPoolTest, MultipleAcquireAndRelease) {
    pool_->clear();
    std::vector<Fiber::ptr> fibers;
    
    // 获取10个协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    size_t before = pool_->get_idle_count();
    
    // 归还所有协程
    for (auto& fiber : fibers) {
        pool_->release(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), before + 10);
}

// ==================== 边界条件测试 ====================

// 测试6：空池获取协程
TEST_F(FiberPoolTest, AcquireFromEmptyPool) {
    pool_->clear();
    EXPECT_EQ(pool_->get_idle_count(), 0);
    
    auto fiber = pool_->acquire([]() {});
    ASSERT_NE(fiber, nullptr);
}

// 测试7：归还未终止的协程（应该失败或忽略）
TEST_F(FiberPoolTest, ReleaseNonTerminatedFiber) {
    auto fiber = pool_->acquire([]() {
        Fiber::yield();
    });
    
    fiber->resume();
    EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);
    
    size_t before_count = pool_->get_idle_count();
    pool_->release(fiber); // 应该被忽略
    size_t after_count = pool_->get_idle_count();
    
    // 未终止的协程不应该被归还
    EXPECT_EQ(before_count, after_count);
}

// 测试8：归还nullptr（应该安全处理）
TEST_F(FiberPoolTest, ReleaseNullptr) {
    EXPECT_NO_THROW({
        pool_->release(nullptr);
    });
}

// 测试9：清空池
TEST_F(FiberPoolTest, ClearPool) {
    // 添加多个空闲协程
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->acquire([]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    EXPECT_GT(pool_->get_idle_count(), 0);
    
    pool_->clear();
    EXPECT_EQ(pool_->get_idle_count(), 0);
}

// 测试10：调整池大小
TEST_F(FiberPoolTest, ResizePool) {
    pool_->clear();
    // 添加多个空闲协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->acquire([]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), 10);
    
    // 调整为更小的大小
    pool_->resize(5);
    EXPECT_LE(pool_->get_idle_count(), 5);
}

// ==================== 统计信息测试 ====================

// 测试11：统计协程创建数
TEST_F(FiberPoolTest, StatisticsCreated) {
    pool_->clear();
    auto stats_before = pool_->get_statistics();
    
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
    }
    
    auto stats_after = pool_->get_statistics();
    EXPECT_EQ(stats_after.total_created - stats_before.total_created, 10);
}

// 测试12：统计协程复用数
TEST_F(FiberPoolTest, StatisticsReused) {
    pool_->clear();
    std::vector<Fiber::ptr> fibers;
    
    // 创建5个协程
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 归还
    for (auto& fiber : fibers) {
        pool_->release(fiber);
    }
    fibers.clear();
    
    auto stats_before = pool_->get_statistics();
    
    // 再次获取（复用）
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
    }
    
    auto stats_after = pool_->get_statistics();
    EXPECT_EQ(stats_after.total_reused - stats_before.total_reused, 5);
}

// 测试13：统计空闲协程数
TEST_F(FiberPoolTest, StatisticsIdleCount) {
    pool_->clear();
    std::vector<Fiber::ptr> fibers;
    
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 逐个归还，检查空闲数
    for (size_t i = 0; i < fibers.size(); ++i) {
        size_t before = pool_->get_idle_count();
        pool_->release(fibers[i]);
        EXPECT_EQ(pool_->get_idle_count(), before + 1);
    }
}

// ==================== 并发安全测试 ====================

// 测试14：并发获取协程
TEST_F(FiberPoolTest, ConcurrentAcquire) {
    const int thread_num = 10;
    const int fibers_per_thread = 10;
    std::vector<std::thread> threads;
    std::atomic<int> total_executed{0};
    
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &total_executed, fibers_per_thread]() {
            for (int i = 0; i < fibers_per_thread; ++i) {
                auto fiber = pool_->acquire([&total_executed]() {
                    total_executed.fetch_add(1);
                });
                ASSERT_NE(fiber, nullptr);
                fiber->resume();
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(total_executed.load(), thread_num * fibers_per_thread);
}

// 测试15：并发归还协程
TEST_F(FiberPoolTest, ConcurrentRelease) {
    const int thread_num = 10;
    const int fibers_per_thread = 10;
    std::vector<std::thread> threads;
    std::vector<std::vector<Fiber::ptr>> thread_fibers(thread_num);
    
    // 先创建协程
    for (int t = 0; t < thread_num; ++t) {
        for (int i = 0; i < fibers_per_thread; ++i) {
            auto fiber = pool_->acquire([]() {});
            fiber->resume();
            thread_fibers[t].push_back(fiber);
        }
    }
    
    size_t before = pool_->get_idle_count();
    
    // 并发归还
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &thread_fibers, t]() {
            for (auto& fiber : thread_fibers[t]) {
                pool_->release(fiber);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(pool_->get_idle_count(), before + thread_num * fibers_per_thread);
}

// 测试16：并发获取和归还
TEST_F(FiberPoolTest, ConcurrentAcquireAndRelease) {
    const int thread_num = 8;
    const int operations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> total_ops{0};
    
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &total_ops, operations]() {
            for (int i = 0; i < operations; ++i) {
                auto fiber = pool_->acquire([&total_ops]() {
                    total_ops.fetch_add(1);
                });
                fiber->resume();
                pool_->release(fiber);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(total_ops.load(), thread_num * operations);
}

// ==================== 容量限制测试 ====================

// 测试17：最小容量限制
TEST_F(FiberPoolTest, MinSizeConstraint) {
    auto fiber = pool_->acquire([]() {});
    ASSERT_NE(fiber, nullptr);
    fiber->resume();
    pool_->release(fiber);
    
    EXPECT_GE(pool_->get_idle_count(), 0);
}

// 测试18：最大容量限制
TEST_F(FiberPoolTest, MaxSizeConstraint) {
    pool_->clear();
    pool_->resize(5);
    std::vector<Fiber::ptr> fibers;
    
    // 创建超过最大容量的协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 归还所有协程
    for (auto& fiber : fibers) {
        pool_->release(fiber);
    }
    
    // 空闲协程数不应超过最大容量
    EXPECT_LE(pool_->get_idle_count(), 5);
}

// ==================== 协程重置测试 ====================

// 测试19：协程重置后复用
TEST_F(FiberPoolTest, FiberResetAndReuse) {
    pool_->clear();
    int first_count = 0;
    int second_count = 0;
    
    auto fiber = pool_->acquire([&first_count]() {
        first_count++;
    });
    fiber->resume();
    EXPECT_EQ(first_count, 1);
    
    pool_->release(fiber);
    
    // 再次获取（应该复用并重置）
    auto reused = pool_->acquire([&second_count]() {
        second_count++;
    });
    reused->resume();
    
    EXPECT_EQ(second_count, 1);
    EXPECT_EQ(first_count, 1); // 第一个计数器不应该改变
}

// 测试20：多次重置复用
TEST_F(FiberPoolTest, MultipleResetAndReuse) {
    pool_->clear();
    std::vector<int> counts(5, 0);
    
    auto fiber = pool_->acquire([&counts]() { counts[0]++; });
    fiber->resume();
    pool_->release(fiber);
    
    for (int i = 1; i < 5; ++i) {
        auto reused = pool_->acquire([&counts, i]() { counts[i]++; });
        reused->resume();
        pool_->release(reused);
    }
    
    // 每个计数器都应该是1
    for (int count : counts) {
        EXPECT_EQ(count, 1);
    }
}

// ==================== 异常处理测试 ====================

// 测试21：协程内部异常不影响池
TEST_F(FiberPoolTest, ExceptionInFiber) {
    auto fiber = pool_->acquire([]() {
        throw std::runtime_error("Test exception");
    });
    
    EXPECT_THROW({
        fiber->resume();
    }, std::runtime_error);
    
    // 池应该仍然可用
    auto another = pool_->acquire([]() {});
    ASSERT_NE(another, nullptr);
    another->resume();
}

// 测试22：多个协程异常
TEST_F(FiberPoolTest, MultipleExceptions) {
    const int count = 5;
    int exception_count = 0;
    
    for (int i = 0; i < count; ++i) {
        auto fiber = pool_->acquire([i]() {
            if (i % 2 == 0) {
                throw std::runtime_error("Exception");
            }
        });
        
        try {
            fiber->resume();
        } catch (...) {
            exception_count++;
        }
    }
    
    EXPECT_EQ(exception_count, 3); // 0, 2, 4抛出异常
    
    // 池仍然可用
    auto normal = pool_->acquire([]() {});
    ASSERT_NE(normal, nullptr);
}

// ==================== 性能相关测试 ====================

// 测试23：大量协程创建
TEST_F(FiberPoolTest, MassiveCreation) {
    const int count = 1000;
    std::vector<Fiber::ptr> fibers;
    
    for (int i = 0; i < count; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fibers.push_back(fiber);
    }
    
    EXPECT_EQ(fibers.size(), count);
    
    for (auto& fiber : fibers) {
        fiber->resume();
    }
}

// 测试24：大量协程复用
TEST_F(FiberPoolTest, MassiveReuse) {
    pool_->clear();
    const int rounds = 100;
    const int batch_size = 10;
    
    for (int r = 0; r < rounds; ++r) {
        std::vector<Fiber::ptr> batch;
        
        for (int i = 0; i < batch_size; ++i) {
            auto fiber = pool_->acquire([r, i]() {});
            fiber->resume();
            batch.push_back(fiber);
        }
        
        for (auto& fiber : batch) {
            pool_->release(fiber);
        }
    }
    
    auto stats = pool_->get_statistics();
    EXPECT_GT(stats.total_reused, 0);
}

// 测试25：空闲协程清理
TEST_F(FiberPoolTest, IdleFiberCleanup) {
    // 创建大量协程
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->acquire([i]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    size_t before = pool_->get_idle_count();
    EXPECT_GT(before, 0);
    
    // 调整大小触发清理
    pool_->resize(5);
    
    size_t after = pool_->get_idle_count();
    EXPECT_LE(after, 5);
}

int main(int argc, char** argv)
{
    // 初始化日志系统
    zcoroutine::init_logger(zlog::LogLevel::value::INFO);
    
    ::testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}