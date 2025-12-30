#include "io/io_scheduler.h"
#include "util/zcoroutine_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace zcoroutine;
using namespace std::chrono;

// 全局配置
static int g_thread_count = 4;
static int g_timer_count = 10000;
static int g_recurring_timer_count = 1000;
static int g_test_duration = 60;
static bool g_use_shared_stack = false;
static std::atomic<bool> g_running{true};

// 性能指标
static std::atomic<uint64_t> g_timer_fired{0};
static std::vector<int64_t> g_delay_samples;  // 定时器延迟（实际触发时间 - 预期时间）
static std::atomic<size_t> g_peak_memory_kb{0};
static high_resolution_clock::time_point g_start_time;

// 获取内存占用
size_t get_memory_usage() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.substr(0, 6) == "VmRSS:") {
      return std::stoul(line.substr(7));
    }
  }
  return 0;
}

// 定时器回调
void timer_callback(high_resolution_clock::time_point expected_time) {
  auto actual_time = high_resolution_clock::now();
  int64_t delay_us = duration_cast<microseconds>(actual_time - expected_time).count();
  
  g_delay_samples.push_back(delay_us);
  g_timer_fired.fetch_add(1, std::memory_order_relaxed);
}

// 内存监控线程
void memory_monitor() {
  while (g_running.load(std::memory_order_relaxed)) {
    size_t current = get_memory_usage();
    size_t peak = g_peak_memory_kb.load(std::memory_order_relaxed);
    if (current > peak) {
      g_peak_memory_kb.store(current, std::memory_order_relaxed);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

// 计算百分位数
int64_t percentile(std::vector<int64_t> &samples, double p) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  size_t idx = static_cast<size_t>(samples.size() * p / 100.0);
  return samples[std::min(idx, samples.size() - 1)];
}

// 打印结果
void print_results(const char *stack_mode) {
  auto end_time = high_resolution_clock::now();
  double duration_sec = duration_cast<seconds>(end_time - g_start_time).count();
  
  uint64_t total_fired = g_timer_fired.load();
  double fire_rate = total_fired / duration_sec;
  
  std::cout << "========== Timer Intensive Benchmark ==========" << std::endl;
  std::cout << "Stack Mode: " << stack_mode << std::endl;
  std::cout << "Thread Count: " << g_thread_count << std::endl;
  std::cout << "One-shot Timers: " << g_timer_count << std::endl;
  std::cout << "Recurring Timers: " << g_recurring_timer_count << std::endl;
  std::cout << "Duration: " << duration_sec << "s" << std::endl;
  std::cout << "-----------------------------------------------------" << std::endl;
  std::cout << "Total Fired: " << total_fired << std::endl;
  std::cout << "Fire Rate: " << static_cast<uint64_t>(fire_rate) << "/s" << std::endl;
  std::cout << "Delay P50: " << percentile(g_delay_samples, 50) << "us" << std::endl;
  std::cout << "Delay P90: " << percentile(g_delay_samples, 90) << "us" << std::endl;
  std::cout << "Delay P99: " << percentile(g_delay_samples, 99) << "us" << std::endl;
  std::cout << "Peak Memory: " << g_peak_memory_kb.load() / 1024 << "MB" << std::endl;
  std::cout << "=====================================================" << std::endl;
}

int main(int argc, char *argv[]) {
  // 解析参数
  if (argc > 1) g_thread_count = std::atoi(argv[1]);
  if (argc > 2) g_timer_count = std::atoi(argv[2]);
  if (argc > 3) g_recurring_timer_count = std::atoi(argv[3]);
  if (argc > 4) g_test_duration = std::atoi(argv[4]);
  if (argc > 5) g_use_shared_stack = (std::string(argv[5]) == "shared");
  
  zcoroutine::init_logger(zlog::LogLevel::value::WARNING);
  
  // 创建调度器
  auto scheduler = std::make_shared<IoScheduler>(g_thread_count, "TimerBench", g_use_shared_stack);
  scheduler->start();
  
  // 启动内存监控
  std::thread mem_thread(memory_monitor);
  
  g_start_time = high_resolution_clock::now();
  
  // 创建一次性定时器
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> timeout_dist(10, 1000);  // 10ms - 1000ms
  
  for (int i = 0; i < g_timer_count; i++) {
    uint64_t timeout_ms = timeout_dist(gen);
    auto expected_time = high_resolution_clock::now() + milliseconds(timeout_ms);
    
    scheduler->add_timer(timeout_ms, [expected_time]() {
      timer_callback(expected_time);
    });
  }
  
  // 创建循环定时器
  for (int i = 0; i < g_recurring_timer_count; i++) {
    scheduler->add_timer(100, []() {
      auto now = high_resolution_clock::now();
      timer_callback(now);
    }, true);  // recurring = true
  }
  
  // 等待测试完成
  std::this_thread::sleep_for(std::chrono::seconds(g_test_duration));
  
  g_running.store(false, std::memory_order_relaxed);
  mem_thread.join();
  
  scheduler->stop();
  
  // 打印结果
  const char *mode = g_use_shared_stack ? "Shared Stack" : "Independent Stack";
  print_results(mode);
  
  return 0;
}
