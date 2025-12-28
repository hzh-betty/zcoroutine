/**
 * @file hook_integration_test.cc
 * @brief Hook机制集成测试
 * 测试系统调用hook、协程调度、超时控制、非阻塞转换等完整功能
 */

#include "hook/hook.h"
#include "io/io_scheduler.h"
#include "io/status_table.h"
#include "util/zcoroutine_logger.h"
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace zcoroutine;

class HookIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    scheduler_ = std::make_shared<IoScheduler>(2, "HookTestScheduler");
    scheduler_->start();
    // 注意：不在这里设置hook_enable，因为调度线程有自己的thread_local
  }

  void TearDown() override {
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }
  }

  std::shared_ptr<IoScheduler> scheduler_;
};

// ==================== Sleep系列Hook测试 ====================

// 测试1：sleep hook - 不阻塞线程
TEST_F(HookIntegrationTest, SleepHookNonBlocking) {
  std::atomic<bool> task1_done{false};
  std::atomic<bool> task2_done{false};

  auto start = std::chrono::steady_clock::now();

  // 协程1：sleep 1s
  scheduler_->schedule([&]() {
    set_hook_enable(true); // 在协程内部启用hook
    sleep(1);              // 被hook为定时器
    task1_done = true;
  });

  // 协程2：立即执行
  scheduler_->schedule([&]() {
    set_hook_enable(true); // 保持一致
    task2_done = true;
  });

  // task2应该能立即完成，不被task1阻塞
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(task2_done.load());
  EXPECT_FALSE(task1_done.load());

  // 等待task1完成
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  EXPECT_TRUE(task1_done.load());

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  EXPECT_GE(duration, 900);
}

// 测试2：usleep hook
TEST_F(HookIntegrationTest, UsleepHook) {
  std::atomic<bool> done{false};

  auto start = std::chrono::steady_clock::now();

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    usleep(500000); // 500ms
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  EXPECT_TRUE(done.load());

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  EXPECT_GE(duration, 500);
}

// 测试3：nanosleep hook
TEST_F(HookIntegrationTest, NanosleepHook) {
  std::atomic<bool> done{false};

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    struct timespec ts = {0, 200000000}; // 200ms
    nanosleep(&ts, nullptr);
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_TRUE(done.load());
}

// ==================== Socket创建Hook测试 ====================

// 测试4：socket hook - 自动注册到 StatusTable
TEST_F(HookIntegrationTest, SocketHookRegistration) {
  int sockfd = -1;

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GT(sockfd, 0);

    // 检查是否注册到 StatusTable
    auto fd_ctx = StatusTable::GetInstance()->get(sockfd);
    EXPECT_NE(fd_ctx, nullptr);
    EXPECT_TRUE(fd_ctx->is_socket());
    EXPECT_TRUE(fd_ctx->get_sys_nonblock());   // 应该被设为非阻塞
    EXPECT_FALSE(fd_ctx->get_user_nonblock()); // 用户视角是阻塞的

    close(sockfd);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// 测试5：accept hook - 新fd也被注册
TEST_F(HookIntegrationTest, AcceptHookRegistration) {
  std::atomic<int> accepted_fd{-1};

  // 创建监听socket
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(listen_fd, 0);

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(0); // 随机端口

  ASSERT_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
  ASSERT_EQ(listen(listen_fd, 5), 0);

  // 获取实际绑定的端口
  socklen_t addr_len = sizeof(addr);
  getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len);
  int port = ntohs(addr.sin_port);

  // 协程：accept
  scheduler_->schedule([&, listen_fd]() {
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
    accepted_fd = fd;

    if (fd > 0) {
      // 检查新fd是否注册
      auto fd_ctx = StatusTable::GetInstance()->get(fd);
      EXPECT_NE(fd_ctx, nullptr);
      EXPECT_TRUE(fd_ctx->is_socket());
      close(fd);
    }
  });

  // 客户端连接
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = htons(port);
  if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GT(accepted_fd.load(), 0);

  close(client_fd);
  close(listen_fd);
}

// ==================== Read/Write Hook测试 ====================

// 测试6：read hook - EAGAIN时挂起协程
TEST_F(HookIntegrationTest, ReadHookWithEAGAIN) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);

  std::atomic<bool> read_done{false};
  std::string received_data;

  // socket pair更适合测试
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  // 协程：读取数据（会先遇到EAGAIN）
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    char buf[256];
    ssize_t n = read(socks[0], buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      received_data = buf;
      read_done = true;
    }
  });

  // 延迟写入
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const char *msg = "test message";
  ASSERT_EQ(write(socks[1], msg, strlen(msg)), strlen(msg));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(read_done.load());
  EXPECT_EQ(received_data, "test message");

  close(socks[0]);
  close(socks[1]);
}

// 测试7：write hook
TEST_F(HookIntegrationTest, WriteHook) {
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  std::atomic<bool> write_done{false};

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    const char *msg = "hello";
    ssize_t n = write(socks[0], msg, strlen(msg));
    EXPECT_EQ(n, strlen(msg));
    write_done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(write_done.load());

  // 验证数据
  char buf[64];
  ssize_t n = read(socks[1], buf, sizeof(buf));
  EXPECT_GT(n, 0);

  close(socks[0]);
  close(socks[1]);
}

// 测试8：recv/send hook
TEST_F(HookIntegrationTest, RecvSendHook) {
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  std::atomic<bool> done{false};
  std::string received;

  // 发送协程
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    const char *msg = "test send";
    send(socks[0], msg, strlen(msg), 0);
  });

  // 接收协程
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    char buf[256];
    ssize_t n = recv(socks[1], buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      received = buf;
      done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(done.load());
  EXPECT_EQ(received, "test send");

  close(socks[0]);
  close(socks[1]);
}

// ==================== Fcntl Hook测试 ====================

// 测试9：fcntl F_SETFL - 用户非阻塞设置
TEST_F(HookIntegrationTest, FcntlSetNonblock) {
  set_hook_enable(true); // 必须启用hook才能注册fd到 StatusTable
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto fd_ctx = StatusTable::GetInstance()->get(sockfd);
  ASSERT_NE(fd_ctx, nullptr);

  // 初始状态：系统非阻塞，用户视角阻塞
  EXPECT_TRUE(fd_ctx->get_sys_nonblock());
  EXPECT_FALSE(fd_ctx->get_user_nonblock());

  // 用户设置非阻塞
  int flags = fcntl(sockfd, F_GETFL, 0);
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

  // 检查状态
  EXPECT_TRUE(fd_ctx->get_user_nonblock());
  EXPECT_TRUE(fd_ctx->get_sys_nonblock());

  // F_GETFL应该返回用户设置的状态
  int new_flags = fcntl(sockfd, F_GETFL, 0);
  EXPECT_TRUE(new_flags & O_NONBLOCK);

  close(sockfd);
}

// 测试10：fcntl F_GETFL - 返回用户视角状态
TEST_F(HookIntegrationTest, FcntlGetFlags) {
  set_hook_enable(true); // 必须启用hook才能注册fd到 StatusTable
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  // 初始状态下F_GETFL应该看不到O_NONBLOCK
  int flags = fcntl(sockfd, F_GETFL, 0);
  EXPECT_FALSE(flags & O_NONBLOCK);

  // 设置后能看到
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
  flags = fcntl(sockfd, F_GETFL, 0);
  EXPECT_TRUE(flags & O_NONBLOCK);

  close(sockfd);
}

// 测试11：用户设置非阻塞后，read不再被hook接管
TEST_F(HookIntegrationTest, UserNonblockBypassesHook) {
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  // 用户显式设置非阻塞
  int flags = fcntl(socks[0], F_GETFL, 0);
  fcntl(socks[0], F_SETFL, flags | O_NONBLOCK);

  std::atomic<int> read_result{0};

  scheduler_->schedule([&]() {
    char buf[64];
    // 没有数据时应该立即返回EAGAIN，而不是挂起
    ssize_t n = read(socks[0], buf, sizeof(buf));
    read_result = n;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(read_result.load(), -1); // 应该返回-1 (EAGAIN)

  close(socks[0]);
  close(socks[1]);
}

// ==================== Ioctl Hook测试 ====================

// 测试12：ioctl FIONBIO设置非阻塞
TEST_F(HookIntegrationTest, IoctlFIONBIO) {

  set_hook_enable(true);
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto fd_ctx = StatusTable::GetInstance()->get(sockfd);
  EXPECT_FALSE(fd_ctx->get_user_nonblock());

  // 使用ioctl设置非阻塞
  int on = 1;
  ioctl(sockfd, FIONBIO, &on);

  EXPECT_TRUE(fd_ctx->get_user_nonblock());

  // 取消非阻塞
  int off = 0;
  ioctl(sockfd, FIONBIO, &off);

  EXPECT_FALSE(fd_ctx->get_user_nonblock());

  close(sockfd);
}

// ==================== Setsockopt Hook测试 ====================

// 测试13：setsockopt SO_RCVTIMEO设置读超时
TEST_F(HookIntegrationTest, SetsockoptRcvTimeout) {
  set_hook_enable(true);

  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  // 设置200ms读超时
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 200000; // 200ms
  setsockopt(socks[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  auto fd_ctx = StatusTable::GetInstance()->get(socks[0]);
  EXPECT_EQ(fd_ctx->get_timeout(SO_RCVTIMEO), 200);

  close(socks[0]);
  close(socks[1]);
}

// 测试14：读超时实际生效
TEST_F(HookIntegrationTest, ReadTimeoutWorks) {
  set_hook_enable(true);

  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  // 设置500ms超时
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  setsockopt(socks[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::atomic<bool> timeout_occurred{false};
  std::atomic<int> error_code{0};

  auto start = std::chrono::steady_clock::now();

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    char buf[64];
    ssize_t n = read(socks[0], buf, sizeof(buf));
    if (n == -1) {
      error_code = errno;
      if (errno == ETIMEDOUT) {
        timeout_occurred = true;
      }
    }
  });

  // 不写数据，等待超时
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();

  EXPECT_TRUE(timeout_occurred.load());
  EXPECT_EQ(error_code.load(), ETIMEDOUT);
  EXPECT_GE(duration, 500);
  EXPECT_LT(duration, 2000); // 考虑到调度和系统负载，可能会稍长

  close(socks[0]);
  close(socks[1]);
}

// 测试15：setsockopt SO_SNDTIMEO设置写超时
TEST_F(HookIntegrationTest, SetsockoptSndTimeout) {
  set_hook_enable(true);

  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 300000; // 300ms
  setsockopt(socks[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  auto fd_ctx = StatusTable::GetInstance()->get(socks[0]);
  EXPECT_EQ(fd_ctx->get_timeout(SO_SNDTIMEO), 300);

  close(socks[0]);
  close(socks[1]);
}

// ==================== Close Hook测试 ====================

// 测试16：close时自动取消IO事件
TEST_F(HookIntegrationTest, CloseCanelsIoEvents) {
  set_hook_enable(true);

  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  std::atomic<bool> read_started{false};
  std::atomic<bool> read_failed{false};
  std::atomic<bool> close_done{false};

  // 协程1：读取数据，会在IO事件上挂起
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    read_started = true;
    char buf[64];
    ssize_t n = read(socks[0], buf, sizeof(buf));
    if (n == -1 || n == 0) {
      read_failed = true;
    }
  });

  // 等待read挂起在IO事件上
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(read_started.load());

  // 协程2：关闭fd，应该取消所有事件并唤醒等待的协程
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    close(socks[0]);
    close_done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  // read应该被唤醒并失败
  EXPECT_TRUE(close_done.load());
  EXPECT_TRUE(read_failed.load());

  close(socks[1]);
}

// 测试17：close后 SocketStatus 被删除
TEST_F(HookIntegrationTest, CloseDeletesSocketFdContext) {
  set_hook_enable(true);

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto fd_ctx = StatusTable::GetInstance()->get(sockfd);
  EXPECT_NE(fd_ctx, nullptr);

  close(sockfd);

  // close后 SocketStatus 应该被删除
  fd_ctx = StatusTable::GetInstance()->get(sockfd);
  EXPECT_EQ(fd_ctx, nullptr);
}

// ==================== 组合场景测试 ====================

// 测试18：多个协程并发socket IO
TEST_F(HookIntegrationTest, ConcurrentSocketIo) {
  set_hook_enable(true);

  const int pair_count = 5;
  std::atomic<int> completed{0};

  for (int i = 0; i < pair_count; ++i) {
    scheduler_->schedule([&, i]() {
      set_hook_enable(true);
      int socks[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) != 0) {
        return;
      }

      // 写数据
      const char *msg = "test";
      ASSERT_EQ(write(socks[0], msg, strlen(msg)), strlen(msg));

      // 读数据
      char buf[64];
      ssize_t n = read(socks[1], buf, sizeof(buf));

      if (n > 0) {
        completed++;
      }

      close(socks[0]);
      close(socks[1]);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(completed.load(), pair_count);
}

// 测试19：hook启用/禁用切换
TEST_F(HookIntegrationTest, HookEnableDisable) {
  // 禁用hook
  set_hook_enable(false);

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  // 应该不会注册到 StatusTable
  auto fd_ctx = StatusTable::GetInstance()->get(sockfd);
  // 注意：socket可能还是会创建 SocketStatus，但不会自动设置非阻塞

  close(sockfd);

  // 重新启用
  set_hook_enable(true);

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  fd_ctx = StatusTable::GetInstance()->get(sockfd);
  EXPECT_NE(fd_ctx, nullptr);
  EXPECT_TRUE(fd_ctx->get_sys_nonblock());

  close(sockfd);
}

// 测试20：connect hook - 非阻塞连接
TEST_F(HookIntegrationTest, ConnectHookNonBlocking) {
  set_hook_enable(true);
  // 创建监听socket
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(listen_fd, 0);

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(0);

  ASSERT_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
  ASSERT_EQ(listen(listen_fd, 5), 0);

  socklen_t addr_len = sizeof(addr);
  getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len);

  std::atomic<bool> connect_done{false};
  std::atomic<bool> other_task_done{false};

  // 连接协程
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    int ret = connect(client_fd, (struct sockaddr *)&addr, sizeof(addr));
    connect_done = (ret == 0);
    close(client_fd);
  });

  // 另一个协程，验证connect不阻塞调度
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    other_task_done = true;
  });

  // 等待连接
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // accept
  struct sockaddr_in client_addr;
  socklen_t len = sizeof(client_addr);
  int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
  if (conn_fd > 0) {
    close(conn_fd);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_TRUE(connect_done.load());
  EXPECT_TRUE(other_task_done.load()); // 验证不阻塞

  close(listen_fd);
}

// 测试21：readv/writev hook
TEST_F(HookIntegrationTest, ReadvWritevHook) {
  set_hook_enable(true);
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  std::atomic<bool> done{false};

  // 写协程
  scheduler_->schedule([&]() {
    set_hook_enable(true);
    // 使用writev发送
    const char *msg1 = "hello";
    const char *msg2 = " world";
    struct iovec iov[2];
    iov[0].iov_base = (void *)msg1;
    iov[0].iov_len = strlen(msg1);
    iov[1].iov_base = (void *)msg2;
    iov[1].iov_len = strlen(msg2);

    ssize_t n = writev(socks[0], iov, 2);
    EXPECT_GT(n, 0);
  });

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    // 使用readv接收
    char buf1[32], buf2[32];
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 6;

    ssize_t n = readv(socks[1], iov, 2);
    if (n > 0) {
      done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(done.load());

  close(socks[0]);
  close(socks[1]);
}

// 测试22：非socket fd不被hook接管
TEST_F(HookIntegrationTest, NonSocketFdNotHooked) {
  set_hook_enable(true);
  // 打开普通文件（如果失败就跳过）
  int fd = open("/dev/null", O_RDWR);
  if (fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }

  // 普通文件fd不应该有 SocketStatus，或者有但is_socket为false
  auto fd_ctx = StatusTable::GetInstance()->get(fd);
  if (fd_ctx) {
    EXPECT_FALSE(fd_ctx->is_socket());
  }

  close(fd);
}

// 测试23：无效fd的hook调用
TEST_F(HookIntegrationTest, InvalidFdHook) {
  set_hook_enable(true);
  char buf[10];
  // 读无效fd
  EXPECT_EQ(read(-1, buf, 10), -1);
  EXPECT_EQ(errno, EBADF);

  // 写无效fd
  EXPECT_EQ(write(-1, buf, 10), -1);
  EXPECT_EQ(errno, EBADF);

  // recv/send 无效fd
  EXPECT_EQ(recv(-1, buf, 10, 0), -1);
  EXPECT_EQ(errno, EBADF);
  EXPECT_EQ(send(-1, buf, 10, 0), -1);
  EXPECT_EQ(errno, EBADF);

  // accept/connect 无效fd
  EXPECT_EQ(accept(-1, nullptr, nullptr), -1);
  EXPECT_EQ(errno, EBADF);
  EXPECT_EQ(connect(-1, nullptr, 0), -1);
  EXPECT_EQ(errno, EBADF);

  // 读已关闭fd
  // 注意：close
  // hook可能会取消事件，所以我们需要一个已关闭的fd但StatusTable中可能还有记录？
  // 不，如果close了，StatusTable中也删除了。
  // 所以这里测试的是 StatusTable 中找不到 fd 的情况

  // 创建一个临时文件，不走hook逻辑（或者走但立即返回）
  int fd = open("/dev/null", O_RDONLY);
  if (fd >= 0) {
    close(fd);
    // 现在fd关闭了，read应该返回EBADF
    EXPECT_EQ(read(fd, buf, 10), -1);
    EXPECT_EQ(errno, EBADF);
  }
}

// 测试24：sendmsg/recvmsg hook
TEST_F(HookIntegrationTest, RecvmsgSendmsgHook) {
  set_hook_enable(true);
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socks), 0);

  std::atomic<bool> done{false};

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    // sendmsg
    const char *msg = "msg";
    struct iovec iov;
    iov.iov_base = (void *)msg;
    iov.iov_len = strlen(msg);
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    ssize_t n = sendmsg(socks[0], &mh, 0);
    EXPECT_GT(n, 0);
  });

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    // recvmsg
    char buf[32];
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    ssize_t n = recvmsg(socks[1], &mh, 0);
    if (n > 0) {
      done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(done.load());

  close(socks[0]);
  close(socks[1]);
}

// 测试25：sendto/recvfrom hook
TEST_F(HookIntegrationTest, RecvfromSendtoHook) {
  set_hook_enable(true);
  int socks[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, socks), 0);

  std::atomic<bool> done{false};

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    const char *msg = "udp";
    ssize_t n = sendto(socks[0], msg, strlen(msg), 0, nullptr, 0);
    EXPECT_GT(n, 0);
  });

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    char buf[32];
    ssize_t n = recvfrom(socks[1], buf, sizeof(buf), 0, nullptr, nullptr);
    if (n > 0) {
      done = true;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(done.load());

  close(socks[0]);
  close(socks[1]);
}

// 测试26：getsockopt/setsockopt hook
TEST_F(HookIntegrationTest, GetsockoptHook) {
  set_hook_enable(true);
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  int type = 0;
  socklen_t len = sizeof(type);
  EXPECT_EQ(getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &type, &len), 0);
  EXPECT_EQ(type, SOCK_STREAM);

  // setsockopt 普通选项
  int keepalive = 1;
  EXPECT_EQ(setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
                       sizeof(keepalive)),
            0);

  close(sockfd);
}

// 测试27：fcntl 其他命令覆盖
TEST_F(HookIntegrationTest, FcntlOtherCmds) {
  set_hook_enable(true);
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  // F_DUPFD
  int new_fd = fcntl(sockfd, F_DUPFD, 0);
  EXPECT_GT(new_fd, 0);
  close(new_fd);

  // F_GETFD / F_SETFD
  int flags = fcntl(sockfd, F_GETFD);
  EXPECT_GE(flags, 0);
  fcntl(sockfd, F_SETFD, flags | FD_CLOEXEC);

  close(sockfd);
}

// 测试28：connect 非阻塞 - 立即成功
TEST_F(HookIntegrationTest, ConnectImmediateSuccess) {
  // 模拟连接本地监听端口
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(0);
  ASSERT_GE(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)),0);
  listen(listen_fd, 1);

  socklen_t len = sizeof(addr);
  getsockname(listen_fd, (struct sockaddr *)&addr, &len);

  scheduler_->schedule([&]() {
    set_hook_enable(true);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    // 连接本地通常非常快，可能立即返回0
    ASSERT_GE(connect(fd, (struct sockaddr *)&addr, sizeof(addr)),0);
    close(fd);
  });

  // Accept
  int client_fd = accept(listen_fd, nullptr, nullptr);
  if (client_fd > 0)
    close(client_fd);
  close(listen_fd);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
