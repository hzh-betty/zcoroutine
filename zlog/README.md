# ZLog

ZLog 是一个高性能、轻量级的 C++ 日志库，支持同步和异步两种日志模式。采用双缓冲区技术和生产者-消费者模式实现高效的异步日志，适用于对性能有较高要求的应用场景。

## 特性

- 同步/异步双模式支持
- 双缓冲区异步实现，减少锁竞争
- 多种日志落地方式（控制台、文件、滚动文件）
- 灵活的日志格式化
- 线程安全
- 建造者模式简化配置

## 编译依赖

| 依赖项 | 版本要求 | 说明 |
|--------|----------|------|
| CMake | >= 3.18 | 构建系统 |
| C++ 编译器 | C++11 | GCC 5+ / Clang 3.4+ |
| fmt | 任意版本 | 格式化库 |

### 编译方法

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 系统架构

```mermaid
graph TB
    subgraph User Interface
        API[ZLog API]
        Macros[日志宏<br/>DEBUG/INFO/WARN/ERROR/FATAL]
    end

    subgraph Core
        Logger[Logger 基类]
        SyncLogger[SyncLogger<br/>同步日志器]
        AsyncLogger[AsyncLogger<br/>异步日志器]
    end

    subgraph Async Engine
        AsyncLooper[AsyncLooper<br/>异步循环器]
        ProBuffer[生产缓冲区]
        ConBuffer[消费缓冲区]
        WorkerThread[工作线程]
    end

    subgraph Format
        Formatter[Formatter<br/>格式化器]
        FormatItems[FormatItem<br/>格式化项]
        LogMessage[LogMessage<br/>日志消息]
    end

    subgraph Sink
        LogSink[LogSink 基类]
        StdOutSink[StdOutSink<br/>控制台输出]
        FileSink[FileSink<br/>文件输出]
        RollBySizeSink[RollBySizeSink<br/>滚动文件]
    end

    subgraph Management
        LoggerManager[LoggerManager<br/>日志器管理器]
        LoggerBuilder[LoggerBuilder<br/>建造者]
    end

    API --> Logger
    Macros --> API
    Logger --> SyncLogger
    Logger --> AsyncLogger

    SyncLogger --> Formatter
    SyncLogger --> LogSink

    AsyncLogger --> AsyncLooper
    AsyncLooper --> ProBuffer
    AsyncLooper --> ConBuffer
    ProBuffer --> WorkerThread
    ConBuffer --> WorkerThread
    WorkerThread --> LogSink

    Formatter --> FormatItems
    Formatter --> LogMessage

    LogSink --> StdOutSink
    LogSink --> FileSink
    LogSink --> RollBySizeSink

    LoggerManager --> Logger
    LoggerBuilder --> Logger
```

## 核心类图

```mermaid
classDiagram
    class Logger {
        <<abstract>>
        #mutex_ : mutex
        #loggerName_ : const char*
        #limitLevel_ : LogLevel::value
        #formatter_ : Formatter::ptr
        #sinks_ : vector~LogSink::ptr~
        +Logger(loggerName, limitLevel, formatter, sinks)
        +~Logger()
        +getName() string
        +logImpl(level, file, line, fmt, args...)
        #logImplHelper(level, file, line, fmt, args...)
        #serialize(level, file, line, data)
        #log(data, len)* void
    }

    class SyncLogger {
        +SyncLogger(loggerName, limitLevel, formatter, sinks)
        #log(data, len) void
    }

    class AsyncLogger {
        #looper_ : AsyncLooper::ptr
        +AsyncLogger(loggerName, limitLevel, formatter, sinks, looperType, milliseco)
        #log(data, len) void
        #reLog(buffer) void
    }

    class LoggerBuilder {
        <<abstract>>
        #loggerType_ : LoggerType
        #loggerName_ : const char*
        #limitLevel_ : LogLevel::value
        #formatter_ : Formatter::ptr
        #sinks_ : vector~LogSink::ptr~
        #looperType_ : AsyncType
        #milliseco_ : milliseconds
        +LoggerBuilder()
        +buildLoggerType(loggerType)
        +buildEnalleUnSafe()
        +buildLoggerName(loggerName)
        +buildLoggerLevel(limitLevel)
        +buildWaitTime(milliseco)
        +buildLoggerFormatter(pattern)
        +buildLoggerSink(args...)
        +build()* Logger::ptr
    }

    class LocalLoggerBuilder {
        +LocalLoggerBuilder()
        +build() Logger::ptr
    }

    class GlobalLoggerBuilder {
        +GlobalLoggerBuilder()
        +build() Logger::ptr
    }

    class LoggerManager {
        -mutex_ : mutex
        -rootLogger_ : Logger::ptr
        -loggers_ : unordered_map~string, Logger::ptr~
        +getInstance()$ LoggerManager&
        +addLogger(logger)
        +hasLogger(name) bool
        +getLogger(name) Logger::ptr
        +rootLogger() Logger::ptr
        -LoggerManager()
    }

    class AsyncLooper {
        -looperType_ : AsyncType
        -stop_ : atomic~bool~
        -proBuf_ : Buffer
        -conBuf_ : Buffer
        -mutex_ : Spinlock
        -condPro_ : condition_variable_any
        -condCon_ : condition_variable_any
        -thread_ : thread
        -callBack_ : Functor
        -milliseco_ : milliseconds
        +AsyncLooper(func, looperType, milliseco)
        +~AsyncLooper()
        +push(data, len)
        +stop()
        -threadEntry()
    }

    class Buffer {
        -data_ : char*
        -writerIdx_ : size_t
        -capacity_ : size_t
        -readerIdx_ : size_t
        +Buffer()
        +~Buffer()
        +push(data, len)
        +begin() const char*
        +writeAbleSize() size_t
        +readAbleSize() size_t
        +moveReader(len)
        +reset()
        +swap(buffer)
        +empty() bool
        +canAccommodate(len) bool
        +capacity() size_t
        -ensureEnoughSize(len)
        -calculateNewSize(len) size_t
        -moveWriter(len)
    }

    class LogSink {
        <<abstract>>
        +LogSink()
        +~LogSink()
        +log(data, len)* void
    }

    class StdOutSink {
        +log(data, len) void
    }

    class FileSink {
        #pathname_ : string
        #ofs_ : ofstream
        #autoFlush_ : bool
        +FileSink(pathname, autoFlush)
        +log(data, len) void
    }

    class RollBySizeSink {
        #basename_ : string
        #ofs_ : ofstream
        #maxSize_ : size_t
        #curSize_ : size_t
        #nameCount_ : size_t
        #autoFlush_ : bool
        +RollBySizeSink(basename, maxSize, autoFlush)
        +log(data, len) void
        #createNewFile() string
        #rollOver()
    }

    class Formatter {
        #pattern_ : string
        #items_ : vector~FormatItem::prt~
        +Formatter(pattern)
        +format(buffer, msg)
        #parsePattern() bool
        #createItem(key, val)$ FormatItem::prt
    }

    class FormatItem {
        <<abstract>>
        +~FormatItem()
        +format(buffer, msg)* void
    }

    class LogMessage {
        +curtime_ : time_t
        +level_ : LogLevel::value
        +file_ : const char*
        +line_ : size_t
        +tid_ : threadId
        +payload_ : const char*
        +loggerName_ : const char*
        +LogMessage(level, file, line, payload, loggerName)
    }

    class LogLevel {
        <<enumeration>>
        UNKNOWN
        DEBUG
        INFO
        WARNING
        ERROR
        FATAL
        OFF
        +toString(level)$ string
    }

    Logger <|-- SyncLogger
    Logger <|-- AsyncLogger
    LoggerBuilder <|-- LocalLoggerBuilder
    LoggerBuilder <|-- GlobalLoggerBuilder
    LogSink <|-- StdOutSink
    LogSink <|-- FileSink
    LogSink <|-- RollBySizeSink

    Logger o-- Formatter
    Logger o-- LogSink
    AsyncLogger o-- AsyncLooper
    AsyncLooper o-- Buffer
    Formatter o-- FormatItem
    LoggerManager o-- Logger
    LoggerBuilder ..> Logger : creates
```

## 时序图

### 同步日志时序图

```mermaid
sequenceDiagram
    participant User as 用户代码
    participant Macro as 日志宏
    participant Logger as SyncLogger
    participant Formatter as Formatter
    participant Message as LogMessage
    participant Sink as LogSink

    User->>Macro: INFO("message", args...)
    Macro->>Logger: logImpl(level, file, line, fmt, args)
    Logger->>Logger: logImplHelper()
    Logger->>Logger: fmt::vformat_to() 格式化参数
    Logger->>Logger: serialize(level, file, line, data)
    Logger->>Message: 创建 LogMessage
    Logger->>Formatter: format(buffer, msg)
    Formatter->>Formatter: 遍历 FormatItems
    Formatter-->>Logger: 返回格式化结果
    Logger->>Logger: log(data, len)
    loop 遍历所有 Sink
        Logger->>Sink: log(data, len)
        Sink->>Sink: 写入目标
    end
    Logger-->>User: 返回
```

### 异步日志时序图

```mermaid
sequenceDiagram
    participant User as 用户代码
    participant Logger as AsyncLogger
    participant Looper as AsyncLooper
    participant ProBuf as 生产缓冲区
    participant ConBuf as 消费缓冲区
    participant Worker as 工作线程
    participant Sink as LogSink

    User->>Logger: logImpl(level, file, line, fmt, args)
    Logger->>Logger: serialize() 序列化消息
    Logger->>Looper: push(data, len)
    
    alt ASYNC_SAFE 模式
        Looper->>Looper: 检查缓冲区空间
        alt 空间不足
            Looper->>Looper: 等待消费者
        end
    end
    
    Looper->>ProBuf: push(data, len)
    Looper-->>User: 返回（非阻塞）

    Note over Worker: 后台工作线程
    loop 循环处理
        Worker->>Looper: 等待条件/超时
        Worker->>ProBuf: swap(ConBuf)
        Worker->>ConBuf: 获取数据
        Worker->>Logger: reLog(buffer)
        loop 遍历所有 Sink
            Logger->>Sink: log(data, len)
        end
        Worker->>ConBuf: reset()
    end
```

## 性能对比

基准测试环境说明：
- 测试环境：WSL2 Ubuntu24.04 12th Gen Intel(R) Core(TM) i5-12500H 8G Release
- 测试框架：Google Benchmark
- 消息大小：8B / 512B / 4096B
- 线程数：1 / 2 / 4 / 8 / 16
- 吞吐量计算：消息大小 / 操作时间

### 8 字节消息吞吐量对比 (MB/s)

| 线程数 | ZLog Sync | ZLog Async | ZLog Async Unsafe | Spdlog Sync | Spdlog Async | Glog |
|--------|-----------|------------|-------------------|-------------|--------------|------|
| 1 | 12.61 | 64.66 | 58.21 | 61.04 | 22.62 | 4.06 |
| 2 | 22.97 | 58.67 | 54.89 | 116.27 | 1.22 | 2.96 |
| 4 | 42.40 | 47.10 | 37.77 | 247.65 | 0.68 | 2.18 |
| 8 | 64.66 | 26.97 | 35.14 | 384.62 | 0.84 | 1.08 |
| 16 | 82.93 | 36.33 | 26.67 | 600.63 | 0.75 | 0.86 |

### 512 字节消息吞吐量对比 (MB/s)

| 线程数 | ZLog Sync | ZLog Async | ZLog Async Unsafe | Spdlog Sync | Spdlog Async | Glog |
|--------|-----------|------------|-------------------|-------------|--------------|------|
| 1 | 540.89 | 669.27 | 1387.49 | 1207.92 | 476.66 | 203.87 |
| 2 | 1087.76 | 872.05 | 1043.60 | 2478.57 | 27.43 | 135.82 |
| 4 | 2142.06 | 828.47 | 2357.31 | 4479.49 | 34.99 | 123.30 |
| 8 | 3110.35 | 830.17 | 1755.73 | 6748.57 | 44.07 | 67.22 |
| 16 | 4173.72 | 655.29 | 1018.84 | 9530.03 | 46.97 | 55.20 |

### 4096 字节消息吞吐量对比 (MB/s)

| 线程数 | ZLog Sync | ZLog Async | ZLog Async Unsafe | Spdlog Sync | Spdlog Async | Glog |
|--------|-----------|------------|-------------------|-------------|--------------|------|
| 1 | 1330.31 | 1003.52 | 901.38 | 1482.66 | 701.64 | 846.81 |
| 2 | 2540.18 | 951.72 | 835.53 | 2591.06 | 192.56 | 377.71 |
| 4 | 4699.89 | 942.97 | 1375.24 | 5177.59 | 212.81 | 366.13 |
| 8 | 6974.86 | 986.29 | 1136.44 | 10977.53 | 279.37 | 360.34 |
| 16 | 11489.01 | 865.80 | 1715.22 | 13377.19 | 305.73 | 326.49 |

### 性能分析

1. **同步模式**：ZLog 同步模式在多线程场景下表现出良好的扩展性，随线程数增加吞吐量显著提升。

2. **异步模式**：ZLog 异步模式在单线程/低竞争场景下延迟最低（~118ns），适合对延迟敏感的场景。

3. **对比 Spdlog**：
   - Spdlog 同步模式吞吐量更高
   - 但 Spdlog 异步模式在多线程下性能下降严重
   - ZLog 异步模式在多线程下保持稳定的低延迟

4. **对比 Glog**：ZLog 在所有场景下均显著优于 Glog。

## 快速开始

```cpp
#include "zlog.h"

int main() {
    // 使用默认 root 日志器
    INFO("Hello, {}!", "ZLog");
    
    // 创建自定义异步日志器
    zlog::GlobalLoggerBuilder builder;
    builder.buildLoggerName("async_logger");
    builder.buildLoggerType(zlog::LoggerType::LOGGER_ASYNC);
    builder.buildLoggerLevel(zlog::LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("[%d{%H:%M:%S}][%p]%T%m%n");
    builder.buildLoggerSink<zlog::FileSink>("./logs/app.log");
    builder.build();
    
    auto logger = zlog::getLogger("async_logger");
    logger->ZLOG_INFO("Async logging: {}", 42);
    
    return 0;
}
```

## 格式化字符串

| 占位符 | 说明 |
|--------|------|
| `%d{format}` | 时间，format 为 strftime 格式 |
| `%t` | 线程 ID |
| `%c` | 日志器名称 |
| `%f` | 源文件名 |
| `%l` | 行号 |
| `%p` | 日志级别 |
| `%T` | 制表符 |
| `%m` | 日志消息 |
| `%n` | 换行符 |

## License

MIT License
