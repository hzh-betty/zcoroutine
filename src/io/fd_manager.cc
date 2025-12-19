#include "io/fd_manager.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "util/zcoroutine_logger.h"



extern "C" {
    extern int (*fcntl_f)(int, int, ...);
}
namespace zcoroutine {
    FdCtx::FdCtx(const int fd)
            : fd_(fd)
    {
        init();
    }

    FdCtx::~FdCtx()
    = default;

    bool FdCtx::init()
    {
        if (is_init_) return true; // 已初始化


        // 判断文件描述符是否有效
        struct stat st{};
        if (fstat(fd_, &st) == -1)
        {
            is_init_ = false;
            is_socket_ = false;
            return false;
        }
        is_init_ = true;
        is_socket_ = S_ISSOCK(st.st_mode); // 判断是否是套接字

        // 如果是套接字
        if (is_socket_)
        {
            int flags = 0;
            if (fcntl_f) {
                flags = fcntl_f(fd_, F_GETFL, 0);
            } else {
                flags = ::fcntl(fd_, F_GETFL, 0);
            }
            if (!(flags & O_NONBLOCK)) {
                const int newf = flags | O_NONBLOCK;
                if (fcntl_f) {
                    fcntl_f(fd_, F_SETFL, newf);
                } else {
                    ::fcntl(fd_, F_SETFL, newf);
                }
                sys_nonblock_ = true;
            } else {
                sys_nonblock_ = true;
            }
        }
        else
        {
            sys_nonblock_ = false;
        }

        return is_init_;
    }

    void FdCtx::set_timeout(const int type, const uint64_t ms)
    {
        if (type == SO_RCVTIMEO)
        {
            recv_timeout_ = ms;
        }
        else
        {
            send_timeout_ = ms;
        }
    }

    uint64_t FdCtx::get_timeout(const int type) const
    {
        if (type == SO_RCVTIMEO) return recv_timeout_;
        return send_timeout_;
    }


FdManager::FdManager() {
    // 预分配一定数量的空间
    fd_contexts_.resize(64);
    ZCOROUTINE_LOG_DEBUG("FdManager initialized with capacity={}", fd_contexts_.size());
}

FdContext::ptr FdManager::get(int fd, bool auto_create) {
    if (fd < 0) {
        ZCOROUTINE_LOG_WARN("FdManager::get invalid fd={}", fd);
        return nullptr;
    }
    
    // 读锁
    {
        RWMutex::ReadLock lock(mutex_);
        if (static_cast<size_t>(fd) < fd_contexts_.size()) {
            if (fd_contexts_[fd] || !auto_create) {
                return fd_contexts_[fd];
            }
        } else if (!auto_create) {
            return nullptr;
        }
    }
    
    // 写锁
    RWMutex::WriteLock lock(mutex_);
    
    // 扩容
    if (static_cast<size_t>(fd) >= fd_contexts_.size()) {
        size_t old_size = fd_contexts_.size();
        size_t new_size = static_cast<size_t>(fd * 1.5);
        fd_contexts_.resize(new_size);
        ZCOROUTINE_LOG_INFO("FdManager::get resize fd_contexts: old_size={}, new_size={}", old_size, new_size);
    }
    
    // 创建新的上下文
    if (!fd_contexts_[fd]) {
        fd_contexts_[fd] = std::make_shared<FdContext>(fd);
        ZCOROUTINE_LOG_DEBUG("FdManager::get created new FdContext for fd={}", fd);
    }
    
    return fd_contexts_[fd];
}

void FdManager::del(int fd) {
    if (fd < 0) {
        ZCOROUTINE_LOG_WARN("FdManager::del invalid fd={}", fd);
        return;
    }
    
    RWMutex::WriteLock lock(mutex_);
    
    if (static_cast<size_t>(fd) < fd_contexts_.size()) {
        if (fd_contexts_[fd]) {
            fd_contexts_[fd].reset();
            ZCOROUTINE_LOG_DEBUG("FdManager::del removed FdContext for fd={}", fd);
        }
    }
}

FdManager::ptr FdManager::GetInstance() {
    static FdManager::ptr instance = std::make_shared<FdManager>();
    return instance;
}

}  // namespace zcoroutine
