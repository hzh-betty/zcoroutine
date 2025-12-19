#include "hook/hook.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/ioctl.h>

#include "util/zcoroutine_logger.h"
#include "io/fd_context.h"
#include "io/io_scheduler.h"
#include "runtime/fiber.h"
#include "io/fd_manager.h"


namespace zcoroutine {

// 线程本地的Hook启用标志
static thread_local bool t_hook_enable = false;

bool is_hook_enabled() {
    return t_hook_enable;
}

void set_hook_enable(bool enable) {
    t_hook_enable = enable;
}

}  // namespace zcoroutine

// 定义原始函数指针
sleep_func sleep_f = nullptr;
usleep_func usleep_f = nullptr;
nanosleep_func nanosleep_f = nullptr;
socket_func socket_f = nullptr;
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

// 宏定义：加载原始函数
#define HOOK_FUN(XX) \
    XX ## _f = (XX ## _func)dlsym(RTLD_NEXT, #XX);

// 初始化Hook
struct HookIniter {
    HookIniter() {
        HOOK_FUN(sleep);
        HOOK_FUN(usleep);
        HOOK_FUN(nanosleep);
        HOOK_FUN(socket);
        HOOK_FUN(connect);
        HOOK_FUN(accept);
        HOOK_FUN(read);
        HOOK_FUN(readv);
        HOOK_FUN(recv);
        HOOK_FUN(recvfrom);
        HOOK_FUN(recvmsg);
        HOOK_FUN(write);
        HOOK_FUN(writev);
        HOOK_FUN(send);
        HOOK_FUN(sendto);
        HOOK_FUN(sendmsg);
        HOOK_FUN(fcntl);
        HOOK_FUN(ioctl);
        HOOK_FUN(close);
        HOOK_FUN(setsockopt);
        HOOK_FUN(getsockopt);
        
        ZCOROUTINE_LOG_DEBUG("Hook initialized");
    }
};

static HookIniter s_hook_initer;

// 定时器信息结构体
struct timer_info {
    int cancelled = 0;
};

/**
 * @brief IO操作Hook模板函数
 * @tparam OriginFun 原始函数类型
 * @tparam Args 参数类型
 * @param fd 文件描述符
 * @param fun 原始函数指针
 * @param hook_fun_name Hook函数名称
 * @param event 事件类型
 * @param timeout_so 超时选项
 * @param args 参数列表
 * @return 操作结果
 */
template<class OriginFun, class... Args>
static ssize_t do_io_hook(int fd, OriginFun fun, const char *hook_fun_name, 
                         FdContext::Event event, int timeout_so, Args &&... args) {
    // 如果hook未启用，直接调用原始函数
    if (!zcoroutine::is_hook_enabled()) {
        return fun(fd, std::forward<Args>(args)...);
    }
    
    // 获取文件描述符上下文
    auto fd_manager = FdManager::GetInstance();
    auto fd_ctx = fd_manager->get(fd);
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
    
    uint64_t timeout = fd_ctx->get_timeout(timeout_so);
    auto tinfo = std::make_shared<timer_info>();
    ssize_t ret = 0;
    
    while (true) {
        ret = fun(fd, std::forward<Args>(args)...);
        
        // 如果被系统中断，继续调用
        while (ret == -1 && errno == EINTR) {
            ret = fun(fd, std::forward<Args>(args)...);
        }
        
        // 如果资源不可用，进行协程调度
        if (ret == -1 && errno == EAGAIN) {
            auto io_scheduler = IoScheduler::GetInstance();
            if (!io_scheduler) {
                return -1;
            }
            
            Timer::ptr timer = nullptr;
            std::weak_ptr<timer_info> winfo(tinfo);
            
            // 如果设置了超时时间，添加定时器
            if (timeout != static_cast<uint64_t>(-1)) {
                timer = io_scheduler->add_timer(timeout, [winfo, fd, io_scheduler, event]() {
                    auto it = winfo.lock();
                    if (!it || it->cancelled) {
                        return;
                    }
                    it->cancelled = ETIMEDOUT;
                    
                    // 取消事件
                    io_scheduler->cancel_event(fd, event);
                });
            }
            
            // 添加事件监听
            int add_event_ret = io_scheduler->add_event(fd, event);
            if (add_event_ret) {
                ZCOROUTINE_LOG_WARN("{} add_event failed, fd={}, event={}, ret={}", 
                                   hook_fun_name, fd, static_cast<int>(event), add_event_ret);
                if (timer) {
                    timer->cancel();
                }
                return -1;
            }
            
            Fiber::yield();
            
            // 协程被唤醒后，检查定时器是否被取消
            if (timer) {
                timer->cancel();
            }
            
            // 如果被定时器取消，设置errno并返回错误
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            
            // 否则，继续执行原始函数
        } else {
            break;
        }
    }
    
    return ret;
}

extern "C" {

// sleep - 转换为定时器
unsigned int sleep(unsigned int seconds) {
    if (!zcoroutine::is_hook_enabled()) {
        return sleep_f(seconds);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return sleep_f(seconds);
    }
    
    // 添加定时器，超时后继续执行
    io_scheduler->add_timer(seconds * 1000, [](){});
    zcoroutine::Fiber::yield();
    
    return 0;
}

// usleep - 转换为定时器
int usleep(useconds_t usec) {
    if (!zcoroutine::is_hook_enabled()) {
        return usleep_f(usec);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return usleep_f(usec);
    }
    
    // 添加定时器
    io_scheduler->add_timer(usec / 1000, [](){});
    zcoroutine::Fiber::yield();
    
    return 0;
}

// nanosleep - 转换为定时器
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!zcoroutine::is_hook_enabled()) {
        return nanosleep_f(req, rem);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return nanosleep_f(req, rem);
    }
    
    // 计算超时时间（毫秒）
    uint64_t timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    
    // 添加定时器
    io_scheduler->add_timer(timeout_ms, [](){});
    zcoroutine::Fiber::yield();
    
    return 0;
}

// socket - 设置为非阻塞
int socket(int domain, int type, int protocol) {
    if (!zcoroutine::is_hook_enabled()) {
        return socket_f(domain, type, protocol);
    }
    
    int fd = socket_f(domain, type, protocol);
    if (fd < 0) {
        return fd;
    }
    
    // 获取文件描述符管理器
    auto fd_manager = zcoroutine::FdManager::GetInstance();
    auto fd_ctx = fd_manager->get(fd, true);
    if (fd_ctx) {
        // 设置为非阻塞
        int flags = fcntl_f(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
            fd_ctx->set_sys_nonblock(true);
        }
        ZCOROUTINE_LOG_DEBUG("hook::socket fd={}", fd);
    }
    
    return fd;
}

// connect - 异步连接
int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms) {
    if (!zcoroutine::is_hook_enabled()) {
        return connect_f(fd, addr, addrlen);
    }
    
    // 获取文件描述符上下文
    auto fd_manager = zcoroutine::FdManager::GetInstance();
    auto fd_ctx = fd_manager->get(fd);
    if (!fd_ctx || fd_ctx->is_closed()) {
        errno = EBADF;
        return -1;
    }
    
    // 不是socket，调用原始connect函数
    if (!fd_ctx->is_socket()) {
        return connect_f(fd, addr, addrlen);
    }
    
    // 用户设置为非阻塞，调用原始connect函数
    if (fd_ctx->get_user_nonblock()) {
        return connect_f(fd, addr, addrlen);
    }
    
    int n = connect_f(fd, addr, addrlen);
    if (n == 0) return 0;
    if (n != -1 || errno != EINPROGRESS) {
        return n;
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return -1;
    }
    
    Timer::ptr timer = nullptr;
    auto tinfo = std::make_shared<timer_info>();
    std::weak_ptr<timer_info> winfo(tinfo);
    
    // 如果设置了超时时间，添加定时器
    if (timeout_ms != static_cast<uint64_t>(-1)) {
        timer = io_scheduler->add_timer(timeout_ms, [winfo, fd, io_scheduler]() {
            auto it = winfo.lock();
            if (!it || it->cancelled) {
                return;
            }
            it->cancelled = ETIMEDOUT;
            
            // 取消写事件
            io_scheduler->cancel_event(fd, zcoroutine::FdContext::kWrite);
        });
    }
    
    // 添加事件监听
    int add_event_ret = io_scheduler->add_event(fd, zcoroutine::FdContext::kWrite);
    if (add_event_ret) {
        ZCOROUTINE_LOG_WARN("connect add_event failed, fd={}, ret={}", fd, add_event_ret);
        if (timer) {
            timer->cancel();
        }
        return -1;
    }
    
    zcoroutine::Fiber::yield();
    
    // 协程被唤醒后，检查定时器是否被取消
    if (timer) {
        timer->cancel();
    }
    
    // 如果被定时器取消，设置errno并返回错误
    if (tinfo->cancelled) {
        errno = tinfo->cancelled;
        return -1;
    }
    
    // 检查连接结果
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt_f(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    
    if (error != 0) {
        errno = error;
        return -1;
    }
    
    return 0;
}

static uint64_t s_connect_timeout = -1;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

// accept - 异步接受连接
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int fd = static_cast<int>(do_io_hook(sockfd, accept_f, "accept",
                                         zcoroutine::FdContext::kRead, SO_RCVTIMEO, addr, addrlen));
    if (fd >= 0) {
        // 获取文件描述符管理器
        auto fd_manager = zcoroutine::FdManager::GetInstance();
        auto fd_ctx = fd_manager->get(fd, true);
        if (fd_ctx) {
            // 设置为非阻塞
            int flags = fcntl_f(fd, F_GETFL, 0);
            if (flags != -1) {
                fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
                fd_ctx->set_sys_nonblock(true);
            }
        }
    }
    return fd;
}

// read - 异步读
ssize_t read(int fd, void *buf, size_t count) {
    return do_io_hook(fd, read_f, "read",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, count);
}

// write - 异步写
ssize_t write(int fd, const void *buf, size_t count) {
    return do_io_hook(fd, write_f, "write",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, count);
}

// close - 删除事件
int close(int fd) {
    if (!zcoroutine::is_hook_enabled()) {
        return close_f(fd);
    }
    
    // 获取文件描述符上下文
    auto fd_manager = zcoroutine::FdManager::GetInstance();
    auto fd_ctx = fd_manager->get(fd);
    if (fd_ctx) {
        auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
        if (io_scheduler) {
            io_scheduler->del_event(fd, zcoroutine::FdContext::kRead);
            io_scheduler->del_event(fd, zcoroutine::FdContext::kWrite);
        }
        
        // 删除文件描述符上下文
        fd_manager->del(fd);
    }
    
    return close_f(fd);
}

// readv - 异步读
ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_io_hook(fd, readv_f, "readv",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, iov, iovcnt);
}

// recv - 异步接收
ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_io_hook(sockfd, recv_f, "recv",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, len, flags);
}

// recvfrom - 异步接收
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return do_io_hook(sockfd, recvfrom_f, "recvfrom",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

// recvmsg - 异步接收
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return do_io_hook(sockfd, recvmsg_f, "recvmsg",
                      zcoroutine::FdContext::kRead, SO_RCVTIMEO, msg, flags);
}

// writev - 异步写
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_io_hook(fd, writev_f, "writev",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, iov, iovcnt);
}

// send - 异步发送
ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return do_io_hook(sockfd, send_f, "send",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, len, flags);
}

// sendto - 异步发送
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return do_io_hook(sockfd, sendto_f, "sendto",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
}

// sendmsg - 异步发送
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    return do_io_hook(sockfd, sendmsg_f, "sendmsg",
                      zcoroutine::FdContext::kWrite, SO_SNDTIMEO, msg, flags);
}

// fcntl - 控制文件描述符
int fcntl(int fd, int cmd, ...) {
    va_list va;
    va_start(va, cmd);
    
    switch (cmd) {
        case F_SETFL: {
            int arg = va_arg(va, int);
            va_end(va);
            
            // 获取文件描述符上下文
            auto fd_manager = zcoroutine::FdManager::GetInstance();
            auto fd_ctx = fd_manager->get(fd);
            
            // 如果上下文不存在、已关闭或不是socket，调用原始fcntl函数
            if (!fd_ctx || fd_ctx->is_closed() || !fd_ctx->is_socket()) {
                return fcntl_f(fd, cmd, arg);
            }
            
            // 用户是否设定了非阻塞
            fd_ctx->set_user_nonblock(!!(arg & O_NONBLOCK));
            
            // 最终是否阻塞根据系统设置决定
            if (fd_ctx->get_sys_nonblock()) {
                arg |= O_NONBLOCK;
            } else {
                arg &= ~O_NONBLOCK;
            }
            
            return fcntl_f(fd, cmd, arg);
        }
        
        case F_GETFL: {
            va_end(va);
            int arg = fcntl_f(fd, cmd);
            
            // 获取文件描述符上下文
            auto fd_manager = zcoroutine::FdManager::GetInstance();
            auto fd_ctx = fd_manager->get(fd);
            
            if (!fd_ctx || fd_ctx->is_closed() || !fd_ctx->is_socket()) {
                return arg;
            }
            
            // 这里是呈现给用户显示的为用户设定的值
            // 但是底层还是根据系统设置决定的
            if (fd_ctx->get_user_nonblock()) {
                return arg | O_NONBLOCK;
            } else {
                return arg & ~O_NONBLOCK;
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
            struct flock *arg = va_arg(va, struct flock*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        
        case F_GETOWN_EX:
        case F_SETOWN_EX: {
            struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock*);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

// ioctl - 控制设备
int ioctl(int fd, unsigned long request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);
    
    // 如果request是FIONBIO，设置用户非阻塞标志
    if (FIONBIO == request) {
        bool user_nonblock = !!*static_cast<int*>(arg);
        
        // 获取文件描述符上下文
        auto fd_manager = zcoroutine::FdManager::GetInstance();
        auto fd_ctx = fd_manager->get(fd);
        
        if (!fd_ctx || fd_ctx->is_closed() || !fd_ctx->is_socket()) {
            return ioctl_f(fd, request, arg);
        }
        
        fd_ctx->set_user_nonblock(user_nonblock);
    }
    
    return ioctl_f(fd, request, arg);
}

// setsockopt - 设置socket选项
int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    if (!zcoroutine::is_hook_enabled()) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            // 获取文件描述符上下文
            auto fd_manager = zcoroutine::FdManager::GetInstance();
            auto fd_ctx = fd_manager->get(sockfd);
            
            if (fd_ctx) {
                const struct timeval *v = static_cast<const struct timeval*>(optval);
                fd_ctx->set_timeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

// getsockopt - 获取socket选项
int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

}  // extern "C"
