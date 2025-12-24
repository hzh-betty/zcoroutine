#ifndef ZCOROUTINE_FD_MANAGER_H_
#define ZCOROUTINE_FD_MANAGER_H_

#include <memory>
#include <vector>
#include "sync/rw_mutex.h"

namespace zcoroutine {

    /**
     * @brief 文件描述符上下文
     * 管理单个文件描述符的状态和属性，负责用于hook
     */
    class FdCtx
    {
    public:
        using ptr = std::shared_ptr<FdCtx>;

        explicit FdCtx(int fd);
        ~FdCtx();

        bool init();
        bool is_init() const { return is_init_; }
        bool is_socket() const { return is_socket_; }
        bool is_closed()const {return is_closed_; }

        void set_sys_nonblock(const bool v) { sys_nonblock_ = v; }
        bool get_sys_nonblock() const { return sys_nonblock_; }

        void set_user_nonblock(const bool v) { user_nonblock_ = v; }
        bool get_user_nonblock() const { return user_nonblock_; }

        /*
         * @brief 设置与获取超时时间
         * @param[in] type 超时类型，SO_RCVTIMEO表示读超时，否则表示写超时
         * @param[in] ms 超时时间，单位毫秒
         */
        void set_timeout(int type, uint64_t ms);
        uint64_t get_timeout(int type) const;
    private:
        bool is_init_{false}; // 是否初始化
        bool is_socket_{false}; // 是否是socket
        bool sys_nonblock_{false}; // 系统设置的非阻塞标志
        bool user_nonblock_{false}; // 用户设置的非阻塞标志
        bool is_closed_{false}; // 是否已关闭
        int fd_{-1}; // 文件描述符

        uint64_t recv_timeout_{0}; // 超时时间，单位毫秒
        uint64_t send_timeout_{0}; // 超时时间，单位毫秒
    };


/**
 * @brief 文件描述符管理器
 *
 * 管理 fd -> FdCtx(状态/超时/非阻塞) 的映射。
 */
class FdManager {
public:
    using ptr = std::shared_ptr<FdManager>;

    /**
     * @brief 获取文件描述符上下文
     * @param fd 文件描述符
     * @param auto_create 如果不存在是否自动创建
     */
    FdCtx::ptr get_ctx(int fd, bool auto_create = false);

    /**
     * @brief 删除文件描述符上下文
     */
    void delete_ctx(int fd);

    /**
     * @brief 获取单例
     */
    static ptr GetInstance();

    FdManager();
    ~FdManager() = default;

private:
    std::vector<FdCtx::ptr> fd_datas_;
    RWMutex mutex_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_FD_MANAGER_H_
