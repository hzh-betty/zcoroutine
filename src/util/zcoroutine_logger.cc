#include "util/zcoroutine_logger.h"

namespace zcoroutine {
    void init_logger(const zlog::LogLevel::value level) {
        auto builder = std::make_unique<zlog::GlobalLoggerBuilder>();
        builder->buildLoggerName("zcoroutine_logger");
        builder->buildLoggerLevel(level);
        // 日志格式：[文件:行号] [时间戳] 日志内容
        builder->buildLoggerFormatter("[%f:%l] [%d{%Y-%m-%d %H:%M:%S}] %m%n");
        builder->buildLoggerType(zlog::LoggerType::LOGGER_ASYNC);
        builder->buildLoggerSink<zlog::FileSink>("./logfile/zcoroutine.log");
        builder->buildLoggerSink<zlog::StdOutSink>();
        zcoroutine_logger = builder->build();
    }
}