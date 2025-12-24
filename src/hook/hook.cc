#include "hook/hook.h"
#include "util/zcoroutine_logger.h"
#include "io/io_scheduler.h"
#include "io/status_table.h"
#include "runtime/fiber.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <cstdarg>
#include <cerrno>
#include <sys/ioctl.h>

namespace zcoroutine {

// 线程本地的Hook启用标志
static thread_local bool t_hook_enable = false;

bool is_hook_enabled() {
    return t_hook_enable;
}

void set_hook_enable(bool enable) {
    t_hook_enable = enable;
}

void hook_init() {
    static bool is_inited = false;
    if (is_inited) return;
    is_inited = true;

    // 使用dlsym获取原始函数指针
#define XX(name) name##_f = (name##_func)dlsym(RTLD_NEXT, #name);
    XX(sleep)
    XX(usleep)
    XX(nanosleep)
    XX(socket)
    XX(socketpair)
    XX(connect)
    XX(accept)
    XX(read)
    XX(readv)
    XX(recv)
    XX(recvfrom)
    XX(recvmsg)
    XX(write)
    XX(writev)
    XX(send)
    XX(sendto)
    XX(sendmsg)
    XX(fcntl)
    XX(ioctl)
    XX(close)
    XX(setsockopt)
    XX(getsockopt)
#undef XX
}

// 静态初始化器，确保hook在main之前初始化
struct HookIniter {
    HookIniter() {
        hook_init();
    }
};

static HookIniter s_hook_initer;

}  // namespace zcoroutine

// 定义原始函数指针
sleep_func sleep_f = nullptr;
usleep_func usleep_f = nullptr;
nanosleep_func nanosleep_f = nullptr;
socket_func socket_f = nullptr;
socketpair_func socketpair_f = nullptr;
connect_func connect_f = nullptr;
accept_func accept_f = nullptr;
read_func read_f = nullptr;
readv_func readv_f = nullptr;
recv_func recv_f = nullptr;
recvfrom_func recvfrom_f = nullptr;
recvmsg_func recvmsg_f = nullptr;
write_func write_f = nullptr;
writev_func writev_f = nullptr;
send_func send_f = nullptr;
sendto_func sendto_f = nullptr;
sendmsg_func sendmsg_f = nullptr;
fcntl_func fcntl_f = nullptr;
ioctl_func ioctl_f = nullptr;
close_func close_f = nullptr;
setsockopt_func setsockopt_f = nullptr;
getsockopt_func getsockopt_f = nullptr;

// 定时器信息，用于超时处理
struct timer_info {
    int cancelled = 0;
};

// 默认连接超时时间（毫秒）
static uint64_t s_connect_timeout = 5000;

/**
 * @brief 通用IO Hook模板函数
 * 
 * 将阻塞IO操作转换为非阻塞+协程调度的方式
 * @param fd 文件描述符
 * @param fun 原始函数
 * @param hook_fun_name 函数名（用于日志）
 * @param event IO事件类型（读/写）
 * @param timeout_so 超时选项类型（SO_RCVTIMEO/SO_SNDTIMEO）
 * @param args 原始函数的其他参数
 * @return 返回值与原始函数相同
 */
template<typename OriginFun, typename... Args>
static ssize_t do_io_hook(int fd, OriginFun fun, const char* hook_fun_name,
                          uint32_t event, int timeout_so, Args&&... args) {
    // 如果hook未启用，直接调用原始函数
    if (!zcoroutine::is_hook_enabled()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取文件描述符上下文
    zcoroutine::StatusTable::ptr status_table = zcoroutine::StatusTable::GetInstance();
    zcoroutine::SocketStatus::ptr fd_ctx = status_table->get(fd);
    if (!fd_ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 如果文件描述符已关闭，设置errno并返回错误
    if (fd_ctx->is_closed()) {
        errno = EBADF;
        return -1;
    }

    // 如果不是socket或者用户设置为非阻塞，直接调用原始函数
    if (!fd_ctx->is_socket() || fd_ctx->get_user_nonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取超时时间
    uint64_t timeout = fd_ctx->get_timeout(timeout_so);
    std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();

    while (true) {
        // 尝试执行IO操作
        ssize_t ret = fun(fd, std::forward<Args>(args)...);

        // 如果被系统中断，重试
        while (ret == -1 && errno == EINTR) {
            ret = fun(fd, std::forward<Args>(args)...);
        }

        // 如果资源暂时不可用（EAGAIN），进行协程调度
        if (ret == -1 && errno == EAGAIN) {
            zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
            if (!iom) {
                return fun(fd, std::forward<Args>(args)...);
            }

            zcoroutine::Timer::ptr timer = nullptr;
            std::weak_ptr<timer_info> winfo(tinfo);

            // 如果设置了超时时间，添加条件定时器
            if (timeout != static_cast<uint64_t>(-1) && timeout != 0) {
                timer = iom->add_condition_timer(timeout, [winfo, fd, iom, event]() {
                    auto it = winfo.lock();
                    if (!it || it->cancelled) {
                        return;
                    }
                    it->cancelled = ETIMEDOUT;
                    // 取消IO事件
                    iom->cancel_event(fd, static_cast<zcoroutine::FdContext::Event>(event));
                }, winfo);
            }

            // 添加IO事件监听
            int add_event_ret = iom->add_event(fd, static_cast<zcoroutine::FdContext::Event>(event));
            if (add_event_ret != 0) {
                ZCOROUTINE_LOG_WARN("{} add_event failed, fd={}, event={}, ret={}",
                                   hook_fun_name, fd, event, add_event_ret);
                if (timer) {
                    timer->cancel();
                }
                return -1;
            }

            // 让出协程，等待事件就绪
            zcoroutine::Fiber::yield();

            // 协程被唤醒后，取消定时器
            if (timer) {
                timer->cancel();
            }

            // 检查是否超时
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }

            // 重新尝试IO操作
            continue;
        }

        return ret;
    }
}

extern "C" {

// ==================== Sleep系列 ====================

unsigned int sleep(unsigned int seconds) {
    if (!zcoroutine::is_hook_enabled()) {
        return sleep_f(seconds);
    }

    zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
    if (!iom) {
        return sleep_f(seconds);
    }

    zcoroutine::Fiber* cur_fiber = zcoroutine::Fiber::get_this();
    if (!cur_fiber) {
        return sleep_f(seconds);
    }

    // 使用定时器和协程实现非阻塞sleep
    // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
    auto fiber_ptr = cur_fiber->shared_from_this();
    iom->add_timer(seconds * 1000, [iom, fiber_ptr]() {
        iom->schedule(fiber_ptr);
    });
    zcoroutine::Fiber::yield();

    return 0;
}

int usleep(useconds_t usec) {
    if (!zcoroutine::is_hook_enabled()) {
        return usleep_f(usec);
    }

    zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
    if (!iom) {
        return usleep_f(usec);
    }

    zcoroutine::Fiber* cur_fiber = zcoroutine::Fiber::get_this();
    if (!cur_fiber) {
        return usleep_f(usec);
    }

    // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
    auto fiber_ptr = cur_fiber->shared_from_this();
    iom->add_timer(usec / 1000, [iom, fiber_ptr]() {
        iom->schedule(fiber_ptr);
    });
    zcoroutine::Fiber::yield();

    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!zcoroutine::is_hook_enabled()) {
        return nanosleep_f(req, rem);
    }

    zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
    if (!iom) {
        return nanosleep_f(req, rem);
    }

    zcoroutine::Fiber* cur_fiber = zcoroutine::Fiber::get_this();
    if (!cur_fiber) {
        return nanosleep_f(req, rem);
    }

    uint64_t timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
    auto fiber_ptr = cur_fiber->shared_from_this();
    iom->add_timer(timeout_ms, [iom, fiber_ptr]() {
        iom->schedule(fiber_ptr);
    });
    zcoroutine::Fiber::yield();

    return 0;
}

// ==================== Socket系列 ====================

int socket(int domain, int type, int protocol) {
    if (!zcoroutine::is_hook_enabled()) {
        return socket_f(domain, type, protocol);
    }

    int fd = socket_f(domain, type, protocol);
    if (fd < 0) {
        return fd;
    }

    // 获取 StatusTable 并注册该fd
    zcoroutine::StatusTable::ptr status_table = zcoroutine::StatusTable::GetInstance();
    status_table->get(fd, true);  // auto_create = true

    ZCOROUTINE_LOG_DEBUG("hook::socket fd={}", fd);
    return fd;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!zcoroutine::is_hook_enabled()) {
        return socketpair_f(domain, type, protocol, sv);
    }

    int ret = socketpair_f(domain, type, protocol, sv);
    if (ret < 0) {
        return ret;
    }

    // 获取 StatusTable 并注册两个fd
    zcoroutine::StatusTable::ptr status_table = zcoroutine::StatusTable::GetInstance();
    status_table->get(sv[0], true);  // auto_create = true
    status_table->get(sv[1], true);  // auto_create = true

    ZCOROUTINE_LOG_DEBUG("hook::socketpair sv[0]={}, sv[1]={}", sv[0], sv[1]);
    return ret;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) {
    if (!zcoroutine::is_hook_enabled()) {
        return connect_f(fd, addr, addrlen);
    }

    // 获取文件描述符上下文
    zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(fd);
    if (!ctx || ctx->is_closed()) {
        errno = EBADF;
        return -1;
    }

    // 不是socket，调用原始connect函数
    if (!ctx->is_socket()) {
        return connect_f(fd, addr, addrlen);
    }

    // 用户设置为非阻塞，直接调用
    if (ctx->get_user_nonblock()) {
        return connect_f(fd, addr, addrlen);
    }

    // 尝试连接
    int n = connect_f(fd, addr, addrlen);
    if (n == 0) {
        return 0;
    }
    if (n != -1 || errno != EINPROGRESS) {
        return n;
    }

    // 连接正在进行中，等待可写事件
    zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
    if (!iom) {
        return n;
    }

    zcoroutine::Timer::ptr timer = nullptr;
    std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();
    std::weak_ptr<timer_info> winfo(tinfo);

    // 如果设置了超时时间，添加定时器
    if (timeout_ms != static_cast<uint64_t>(-1)) {
        timer = iom->add_condition_timer(timeout_ms, [winfo, fd, iom]() {
            auto it = winfo.lock();
            if (!it || it->cancelled) {
                return;
            }
            it->cancelled = ETIMEDOUT;
            iom->cancel_event(fd, zcoroutine::FdContext::kWrite);
        }, winfo);
    }

    // 添加写事件监听
    int add_event_ret = iom->add_event(fd, zcoroutine::FdContext::kWrite);
    if (add_event_ret != 0) {
        if (timer) {
            timer->cancel();
        }
        ZCOROUTINE_LOG_ERROR("connect_with_timeout add_event failed, fd={}", fd);
        return -1;
    }

    // 让出协程
    zcoroutine::Fiber::yield();

    // 取消定时器
    if (timer) {
        timer->cancel();
    }

    // 检查是否超时
    if (tinfo->cancelled) {
        errno = tinfo->cancelled;
        return -1;
    }

    // 检查连接结果
    int sock_err = 0;
    socklen_t len = sizeof(sock_err);
    if (getsockopt_f(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == -1) {
        return -1;
    }

    if (sock_err != 0) {
        errno = sock_err;
        return -1;
    }

    return 0;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int fd = static_cast<int>(do_io_hook(sockfd, accept_f, "accept",
                                          zcoroutine::FdContext::kRead, SO_RCVTIMEO, addr, addrlen));
    if (fd >= 0) {
        // 注册新连接的fd
        zcoroutine::StatusTable::GetInstance()->get(fd, true);
    }
    return fd;
}

// ==================== Read系列 ====================

ssize_t read(int fd, void* buf, size_t count) {
    return do_io_hook(fd, read_f, "read",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    return do_io_hook(fd, readv_f, "readv",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    return do_io_hook(sockfd, recv_f, "recv",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* addrlen) {
    return do_io_hook(sockfd, recvfrom_f, "recvfrom",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) {
    return do_io_hook(sockfd, recvmsg_f, "recvmsg",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, msg, flags);
}

// ==================== Write系列 ====================

ssize_t write(int fd, const void* buf, size_t count) {
    return do_io_hook(fd, write_f, "write",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    return do_io_hook(fd, writev_f, "writev",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    return do_io_hook(sockfd, send_f, "send",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, len, flags);
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen) {
    return do_io_hook(sockfd, sendto_f, "sendto",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags) {
    return do_io_hook(sockfd, sendmsg_f, "sendmsg",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, msg, flags);
}

// ==================== close ====================

int close(int fd) {
    if (!zcoroutine::is_hook_enabled()) {
        return close_f(fd);
    }

    // 获取文件描述符上下文
    zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(fd);
    if (ctx) {
        zcoroutine::IoScheduler* iom = zcoroutine::IoScheduler::get_this();
        if (iom) {
            // 取消该fd上的所有事件
            iom->cancel_all(fd);
        }
        // 从 StatusTable 中删除
        zcoroutine::StatusTable::GetInstance()->del(fd);
    }

    return close_f(fd);
}

// ==================== fcntl ====================

int fcntl(int fd, int cmd, ... /* arg */) {
    va_list va;
    va_start(va, cmd);

    switch (cmd) {
        // 带int参数的命令
        case F_SETFL: {
            int arg = va_arg(va, int);
            va_end(va);

            zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(fd);
            if (!ctx || ctx->is_closed() || !ctx->is_socket()) {
                return fcntl_f(fd, cmd, arg);
            }

            // 记录用户设置的非阻塞状态
            ctx->set_user_nonblock(arg & O_NONBLOCK);
            
            // 系统层面始终保持非阻塞
            if (ctx->get_sys_nonblock()) {
                arg |= O_NONBLOCK;
            } else {
                arg &= ~O_NONBLOCK;
            }
            return fcntl_f(fd, cmd, arg);
        }

        case F_GETFL: {
            va_end(va);
            int ret = fcntl_f(fd, cmd);
            
            zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(fd);
            if (!ctx || ctx->is_closed() || !ctx->is_socket()) {
                return ret;
            }

            // 返回用户视角的非阻塞状态
            if (ctx->get_user_nonblock()) {
                return ret | O_NONBLOCK;
            } else {
                return ret & ~O_NONBLOCK;
            }
        }

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        {
            int arg = va_arg(va, int);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }

        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
        {
            va_end(va);
            return fcntl_f(fd, cmd);
        }

        case F_SETLK:
        case F_SETLKW:
        case F_GETLK: {
            struct flock* arg = va_arg(va, struct flock*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }

        case F_GETOWN_EX:
        case F_SETOWN_EX: {
            struct f_owner_ex* arg = va_arg(va, struct f_owner_ex*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }

        default: {
            va_end(va);
            return fcntl_f(fd, cmd);
        }
    }
}

// ==================== ioctl ====================

int ioctl(int fd, unsigned long request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    // 处理非阻塞设置
    if (request == FIONBIO) {
        bool user_nonblock = !!*static_cast<int*>(arg);
        
        zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(fd);
        if (ctx && !ctx->is_closed() && ctx->is_socket()) {
            ctx->set_user_nonblock(user_nonblock);
        }
    }

    return ioctl_f(fd, request, arg);
}

// ==================== setsockopt/getsockopt ====================

int setsockopt(int sockfd, int level, int optname,
               const void* optval, socklen_t optlen) {
    if (!zcoroutine::is_hook_enabled()) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 处理超时设置
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            zcoroutine::SocketStatus::ptr ctx = zcoroutine::StatusTable::GetInstance()->get(sockfd);
            if (ctx) {
                const timeval* tv = static_cast<const timeval*>(optval);
                uint64_t timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
                ctx->set_timeout(optname, timeout_ms);
            }
        }
    }

    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

int getsockopt(int sockfd, int level, int optname,
               void* optval, socklen_t* optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

}  // extern "C"
