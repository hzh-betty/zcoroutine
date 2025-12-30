#include "hook/hook.h"
#include "io/io_scheduler.h"
#include "util/zcoroutine_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace zcoroutine;
using namespace std::chrono;

// 全局配置
static int g_port = 8081;
static int g_thread_count = 4;
static int g_connection_count = 1000;
static int g_requests_per_conn = 1000;
static bool g_use_shared_stack = false;
static std::atomic<bool> g_running{true};

// 性能指标
static std::atomic<uint64_t> g_total_requests{0};
static std::vector<uint64_t> g_latency_samples;
static std::atomic<size_t> g_peak_memory_kb{0};
static high_resolution_clock::time_point g_start_time;

// HTTP响应
static const char *HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: keep-alive\r\n"
                                   "\r\n"
                                   "Hello, World!";

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

// 处理长连接客户端
void handle_long_conn(int client_fd) {
  set_hook_enable(true);
  char buffer[4096];
  
  while (g_running.load(std::memory_order_relaxed)) {
    int ret = recv(client_fd, buffer, sizeof(buffer), 0);
    if (ret <= 0) break;
    
    send(client_fd, HTTP_RESPONSE, strlen(HTTP_RESPONSE), 0);
  }
  
  close(client_fd);
}

// 服务端接受连接
void accept_loop(int listen_fd, IoScheduler::ptr scheduler) {
  set_hook_enable(true);
  
  while (g_running.load(std::memory_order_relaxed)) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      break;
    }
    
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    scheduler->schedule([client_fd]() { handle_long_conn(client_fd); });
  }
}

// 客户端保持长连接并发送多个请求
void long_conn_client(int request_count) {
  set_hook_enable(true);
  
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return;
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(g_port);
  server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  
  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
    for (int i = 0; i < request_count && g_running.load(std::memory_order_relaxed); i++) {
      auto start = high_resolution_clock::now();
      
      const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
      send(sock, request, strlen(request), 0);
      
      char buffer[4096];
      recv(sock, buffer, sizeof(buffer), 0);
      
      auto end = high_resolution_clock::now();
      uint64_t latency_us = duration_cast<microseconds>(end - start).count();
      g_latency_samples.push_back(latency_us);
      
      g_total_requests.fetch_add(1, std::memory_order_relaxed);
    }
  }
  
  close(sock);
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
  
  uint64_t total_req = g_total_requests.load();
  double rps = total_req / duration_sec;
  
  std::cout << "========== HTTP Long Connection Benchmark ==========" << std::endl;
  std::cout << "Stack Mode: " << stack_mode << std::endl;
  std::cout << "Thread Count: " << g_thread_count << std::endl;
  std::cout << "Connections: " << g_connection_count << std::endl;
  std::cout << "Duration: " << duration_sec << "s" << std::endl;
  std::cout << "-----------------------------------------------------" << std::endl;
  std::cout << "Total Requests: " << total_req << std::endl;
  std::cout << "RPS: " << static_cast<uint64_t>(rps) << std::endl;
  std::cout << "Latency P50: " << percentile(g_latency_samples, 50) << "us" << std::endl;
  std::cout << "Latency P90: " << percentile(g_latency_samples, 90) << "us" << std::endl;
  std::cout << "Latency P99: " << percentile(g_latency_samples, 99) << "us" << std::endl;
  std::cout << "Peak Memory: " << g_peak_memory_kb.load() / 1024 << "MB" << std::endl;
  std::cout << "=====================================================" << std::endl;
}

int main(int argc, char *argv[]) {
  // 解析参数
  if (argc > 1) g_port = std::atoi(argv[1]);
  if (argc > 2) g_thread_count = std::atoi(argv[2]);
  if (argc > 3) g_connection_count = std::atoi(argv[3]);
  if (argc > 4) g_requests_per_conn = std::atoi(argv[4]);
  if (argc > 5) g_use_shared_stack = (std::string(argv[5]) == "shared");
  
  zcoroutine::init_logger(zlog::LogLevel::value::WARNING);
  
  // 创建监听socket
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(g_port);
  server_addr.sin_addr.s_addr = INADDR_ANY;
  
  if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    std::cerr << "Bind failed" << std::endl;
    return 1;
  }
  
  listen(listen_fd, 1024);
  
  int flags = fcntl(listen_fd, F_GETFL, 0);
  fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  
  // 创建调度器
  auto scheduler = std::make_shared<IoScheduler>(g_thread_count, "HttpServer", g_use_shared_stack);
  scheduler->start();
  
  // 启动服务端
  scheduler->schedule([listen_fd, scheduler]() { accept_loop(listen_fd, scheduler); });
  
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  // 启动内存监控
  std::thread mem_thread(memory_monitor);
  
  // 启动客户端
  g_start_time = high_resolution_clock::now();
  
  for (int i = 0; i < g_connection_count; i++) {
    scheduler->schedule([=]() { long_conn_client(g_requests_per_conn); });
    if (i % 100 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  
  // 等待所有请求完成
  while (g_total_requests.load() < static_cast<uint64_t>(g_connection_count * g_requests_per_conn)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  
  g_running.store(false, std::memory_order_relaxed);
  mem_thread.join();
  
  scheduler->stop();
  close(listen_fd);
  
  // 打印结果
  const char *mode = g_use_shared_stack ? "Shared Stack" : "Independent Stack";
  print_results(mode);
  
  return 0;
}
