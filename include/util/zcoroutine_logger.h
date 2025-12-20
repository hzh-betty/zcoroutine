#ifndef ZCOROUTINE_LOGGER_H_
#define ZCOROUTINE_LOGGER_H_

#include "zlog.h"

namespace zcoroutine {

/**
 * @brief 初始化zcoroutine专属日志器
 * @param level 日志级别，默认为DEBUG
 */
 void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG);

} // namespace zcoroutine

// 便利的日志宏定义
#define ZCOROUTINE_LOG_DEBUG(fmt, ...) zlog::getLogger("zcoroutine_logger")->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_INFO(fmt, ...) zlog::getLogger("zcoroutine_logger")->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_WARN(fmt, ...)  zlog::getLogger("zcoroutine_logger")->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_ERROR(fmt, ...) zlog::getLogger("zcoroutine_logger")->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_FATAL(fmt, ...) zlog::getLogger("zcoroutine_logger")->ZLOG_FATAL(fmt, ##__VA_ARGS__)

#endif // ZCOROUTINE_LOGGER_H_
