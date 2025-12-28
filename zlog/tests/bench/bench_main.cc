
#include "logger.h"

#include <benchmark/benchmark.h>
#include <memory>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

// spdlog
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

// glog
#include <glog/logging.h>

// 确保日志目录存在 (C++14 兼容)
void prepare_log_dir() {
  struct stat st = {0};
  if (stat("bench_logs", &st) == -1) {
    mkdir("bench_logs", 0755);
  }
}

// 生成指定长度的日志内容
std::string make_string(size_t len) { return std::string(len, 'x'); }

// Zlog 同步模式
static void BM_Zlog_Sync(benchmark::State &state) {
  prepare_log_dir();
  auto formatter = std::make_shared<zlog::Formatter>();
  std::vector<zlog::LogSink::ptr> sinks;
  std::string filename =
      "bench_logs/zlog_sync_" + std::to_string(state.thread_index()) + ".log";
  sinks.push_back(std::make_shared<zlog::FileSink>(filename));
  // 使用 SyncLogger 配合 FileSink
  zlog::SyncLogger logger("bench_sync", zlog::LogLevel::value::INFO, formatter,
                          sinks);

  std::string msg = make_string(state.range(0));

  for (auto _ : state) {
    logger.logImpl(zlog::LogLevel::value::INFO, __FILE__, __LINE__,
                   msg.c_str());
  }
}

// Zlog 异步模式
static void BM_Zlog_Async(benchmark::State &state) {
  prepare_log_dir();
  auto formatter = std::make_shared<zlog::Formatter>();
  std::vector<zlog::LogSink::ptr> sinks;
  // 所有线程共享同一个 Sink (模拟实际生产环境: 多线程 -> 1 Logger -> 1 File)
  static auto sink =
      std::make_shared<zlog::FileSink>("bench_logs/zlog_async.log");
  sinks.push_back(sink);

  // 确保 Logger 只初始化一次
  static std::shared_ptr<zlog::AsyncLogger> logger;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    logger = std::make_shared<zlog::AsyncLogger>(
        "bench_async", zlog::LogLevel::value::INFO, formatter, sinks,
        zlog::AsyncType::ASYNC_SAFE, std::chrono::milliseconds(100));
  });

  std::string msg = make_string(state.range(0));

  for (auto _ : state) {
    logger->logImpl(zlog::LogLevel::value::INFO, __FILE__, __LINE__,
                    msg.c_str());
  }
}

// Spdlog 同步模式
static void BM_Spdlog_Sync(benchmark::State &state) {
  prepare_log_dir();
  std::string filename =
      "bench_logs/spdlog_sync_" + std::to_string(state.thread_index()) + ".log";

  // 每个线程使用独立的 Logger 和文件，避免文件锁竞争，专注于测试同步写磁盘性能
  auto name = "bench_spd_sync_" + std::to_string(state.thread_index());
  auto logger = spdlog::get(name);
  if (!logger) {
    try {
      logger = spdlog::basic_logger_mt(name, filename, true);
    } catch (...) {
      logger = spdlog::get(name);
    }
  }
  logger->set_pattern("%+");
  std::string msg = make_string(state.range(0));

  for (auto _ : state) {
    logger->info(msg);
  }
}

// Spdlog 异步模式
static void BM_Spdlog_Async(benchmark::State &state) {
  prepare_log_dir();
  static std::once_flag pool_flag;
  std::call_once(pool_flag, []() { spdlog::init_thread_pool(8192, 1); });

  // 异步模式共享同一个 Logger
  static std::shared_ptr<spdlog::logger> logger;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    logger = spdlog::basic_logger_mt<spdlog::async_factory>(
        "bench_spd_async", "bench_logs/spdlog_async.log", true);
    logger->set_pattern("%+");
  });

  std::string msg = make_string(state.range(0));

  for (auto _ : state) {
    logger->info(msg);
  }
}

// Glog (仅支持同步写文件)
static void BM_Glog(benchmark::State &state) {
  prepare_log_dir();
  if (state.thread_index() == 0) {
    static bool glog_init = false;
    if (!glog_init) {
      google::InitGoogleLogging("bench_glog");
      FLAGS_logtostderr = 0;        // 不输出到 stderr
      FLAGS_alsologtostderr = 0;    // 不同时输出到 stderr
      FLAGS_log_dir = "bench_logs"; // 输出目录
      glog_init = true;
    }
  }

  std::string msg = make_string(state.range(0));

  for (auto _ : state) {
    LOG(INFO) << msg;
  }
}

// 注册基准测试
// 线程数: 1, 16
// 负载范围: 8, 4096 字节
// 使用 RangeMultiplier(512) 减少测试点，聚焦于小包和大包

BENCHMARK(BM_Zlog_Sync)
    ->RangeMultiplier(512)
    ->Range(8, 4096)
    ->ThreadRange(1, 16);
BENCHMARK(BM_Zlog_Async)
    ->RangeMultiplier(512)
    ->Range(8, 4096)
    ->ThreadRange(1, 16);
BENCHMARK(BM_Spdlog_Sync)
    ->RangeMultiplier(512)
    ->Range(8, 4096)
    ->ThreadRange(1, 16);
BENCHMARK(BM_Spdlog_Async)
    ->RangeMultiplier(512)
    ->Range(8, 4096)
    ->ThreadRange(1, 16);
BENCHMARK(BM_Glog)->RangeMultiplier(512)->Range(8, 4096)->ThreadRange(1, 16);

BENCHMARK_MAIN();
