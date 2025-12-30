#include "hook/hook.h"
#include "io/io_scheduler.h"
#include "util/zcoroutine_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace zcoroutine;
using namespace std::chrono;

// 全局配置
static int g_thread_count = 4;
static int g_socketpair_count = 1000;
static int g_test_duration = 30;
static bool g_use_shared_stack = false;
static std::atomic<bool> g_running{true};

// 性能指标
static std::atomic<uint64_t> g_events_processed{0};
static std::vector<uint64_t> g_latency_samples;
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

// Writer协程：高频写入数据
void writer_fiber(int write_fd, IoScheduler::ptr scheduler) {
  set_hook_enable(true);
  
  char data[64] = "benchmark_data";
  while (g_running.load(std::memory_order_relaxed)) {
    write(write_fd, data, sizeof(data));
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  
  close(write_fd);
}

// Reader协程：读取数据并记录延迟
void reader_fiber(int read_fd, IoScheduler::ptr scheduler) {
  set_hook_enable(true);
  
  char buffer[64];
  while (g_running.load(std::memory_order_relaxed)) {
    auto start = high_resolution_clock::now();
    
    int ret = read(read_fd, buffer, sizeof(buffer));
    if (ret <= 0) break;
    
    auto end = high_resolution_clock::now();
    uint64_t latency_us = duration_cast<microseconds>(end - start).count();
    g_latency_samples.push_back(latency_us);
    
    g_events_processed.fetch_add(1, std::memory_order_relaxed);
  }
  
  close(read_fd);
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
uint64_t percentile(std::vector<uint64_t> &samples, double p) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  size_t idx = static_cast<size_t>(samples.size() * p / 100.0);
  return samples[std::min(idx, samples.size() - 1)];
}

// 打印结果
void print_results(const char *stack_mode) {
  auto end_time = high_resolution_clock::now();
  double duration_sec = duration_cast<seconds>(end_time - g_start_time).count();
  
  uint64_t total_events = g_events_processed.load();
  double event_rate = total_events / duration_sec;
  
  std::cout << "========== IO Event Intensive Benchmark ==========" << std::endl;
  std::cout << "Stack Mode: " << stack_mode << std::endl;
  std::cout << "Thread Count: " << g_thread_count << std::endl;
  std::cout << "Socketpair Count: " << g_socketpair_count << std::endl;
  std::cout << "Duration: " << duration_sec << "s" << std::endl;
  std::cout << "-----------------------------------------------------" << std::endl;
  std::cout << "Total Events: " << total_events << std::endl;
  std::cout << "Event Rate: " << static_cast<uint64_t>(event_rate) << "/s" << std::endl;
  std::cout << "Latency P50: " << percentile(g_latency_samples, 50) << "us" << std::endl;
  std::cout << "Latency P90: " << percentile(g_latency_samples, 90) << "us" << std::endl;
  std::cout << "Latency P99: " << percentile(g_latency_samples, 99) << "us" << std::endl;
  std::cout << "Peak Memory: " << g_peak_memory_kb.load() / 1024 << "MB" << std::endl;
  std::cout << "=====================================================" << std::endl;
}

int main(int argc, char *argv[]) {
  // 解析参数
  if (argc > 1) g_thread_count = std::atoi(argv[1]);
  if (argc > 2) g_socketpair_count = std::atoi(argv[2]);
  if (argc > 3) g_test_duration = std::atoi(argv[3]);
  if (argc > 4) g_use_shared_stack = (std::string(argv[4]) == "shared");
  
  zcoroutine::init_logger(zlog::LogLevel::value::WARNING);
  
  // 创建调度器
  auto scheduler = std::make_shared<IoScheduler>(g_thread_count, "IOBench", g_use_shared_stack);
  scheduler->start();
  
  // 启动内存监控
  std::thread mem_thread(memory_monitor);
  
  g_start_time = high_resolution_clock::now();
  
  // 创建socketpair并启动读写协程
  std::vector<std::pair<int, int>> sockets;
  for (int i = 0; i < g_socketpair_count; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
      sockets.push_back({sv[0], sv[1]});
      
      // 启动writer
      scheduler->schedule([sv, scheduler]() {
        writer_fiber(sv[1], scheduler);
      });
      
      // 启动reader
      scheduler->schedule([sv, scheduler]() {
        reader_fiber(sv[0], scheduler);
      });
    }
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
