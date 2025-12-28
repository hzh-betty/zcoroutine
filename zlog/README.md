# Zlog - High Performance C++ Logging Library

Zlog 是一个专为高并发场景设计的高性能 C++ 日志库。它采用了无锁设计思想（自旋锁优化）、双缓冲机制以及零拷贝技术，在多线程环境下展现出远超传统日志库（如 Spdlog, Glog）的吞吐量和低延迟表现。

## 核心特性

- **极致性能**: 在 16 线程高并发场景下，异步日志延迟低至 200ns 级别。
- **双缓冲机制**: 生产缓冲区与消费缓冲区分离，通过指针交换实现无锁（或低锁）数据传递。
- **自旋锁优化**: 采用 Read-Phase + Backoff 策略的自旋锁，大幅减少上下文切换开销。
- **零拷贝**: 核心链路使用指针传递，避免不必要的内存拷贝。
- **严格时序**: 移除线程局部缓存，保证多线程日志的严格写入顺序。

## 系统架构 (Architecture)

### 1. 异步架构 (Async Mode)

Zlog 采用经典的异步生产-消费模型。前端（业务线程）负责极速写入内存缓冲区，后端（后台线程）负责批量落盘。

```mermaid
graph LR
    User[User Thread] -->|Log| AsyncLogger
    AsyncLogger -->|Direct Push| AsyncLooper
    subgraph AsyncLooper [Async Looper Core]
        direction TB
        Spinlock[Optimized Spinlock]
        ProBuf[Production Buffer]
        ConBuf[Consumption Buffer]
        Thread[Background Worker]
    end
    AsyncLogger -.->|Lock & Push| ProBuf
    Thread -->|Swap & Consume| ConBuf
    ConBuf -->|Flush| Sinks
    Sinks -->|Write| Disk[Disk / Console]
```

### 2. 同步架构 (Sync Mode)

同步模式下，用户线程直接完成格式化与落盘操作，适合调试或低频关键日志。

```mermaid
graph LR
    User[User Thread] -->|Log| SyncLogger
    SyncLogger -->|Format| MemoryBuf[Thread Local Buffer]
    SyncLogger -->|Write| Sinks
    Sinks -->|Flush| Disk[Disk / Console]
```

## 核心类图 (Class Diagram)

```mermaid
classDiagram
    %% Core Logger Hierarchy
    class Logger {
        <<abstract>>
        #mutex_ : mutex
        #loggerName_ : const char*
        #limitLevel_ : LogLevel
        #formatter_ : FormatterPtr
        #sinks_ : vector~LogSinkPtr~
        +log(data, len)*
        +logImpl(level, file, line, fmt, args...)
        #serialize(level, file, line, data)
    }

    class SyncLogger {
        +log(data, len)
    }

    class AsyncLogger {
        -looper_ : AsyncLooperPtr
        +log(data, len)
        #reLog(buffer)
    }

    Logger <|-- SyncLogger
    Logger <|-- AsyncLogger

    %% Async Components
    class AsyncLooper {
        -looperType_ : AsyncType
        -stop_ : atomic~bool~
        -proBuf_ : Buffer
        -conBuf_ : Buffer
        -mutex_ : Spinlock
        -condPro_ : condition_variable_any
        -condCon_ : condition_variable_any
        -thread_ : thread
        +push(data, len)
        -threadEntry()
    }

    class Buffer {
        -data_ : char*
        -capacity_ : size_t
        -writerIdx_ : size_t
        -readerIdx_ : size_t
        +push(data, len)
        +readAbleSize()
        +writeAbleSize()
        +swap(buffer)
    }

    class Spinlock {
        -locked_ : atomic~bool~
        +lock()
        +unlock()
    }

    AsyncLogger *-- AsyncLooper
    AsyncLooper *-- Buffer
    AsyncLooper *-- Spinlock

    %% Sinks
    class LogSink {
        <<interface>>
        +log(data, len)*
    }
    class StdOutSink {
        +log(data, len)
    }
    class FileSink {
        -ofs_ : ofstream
        +log(data, len)
    }
    class RollBySizeSink {
        -ofs_ : ofstream
        -maxSize_ : size_t
        -curSize_ : size_t
        +log(data, len)
        -rollOver()
    }

    Logger o-- LogSink
    LogSink <|-- StdOutSink
    LogSink <|-- FileSink
    LogSink <|-- RollBySizeSink

    %% Formatter
    class Formatter {
        -pattern_ : string
        -items_ : vector~FormatItemPtr~
        +format(buffer, msg)
        +init()
    }
    
    Logger o-- Formatter

    %% Management & Builder
    class LoggerManager {
        <<singleton>>
        -rootLogger_ : LoggerPtr
        -loggers_ : map~string, LoggerPtr~
        +getInstance()
        +getLogger(name)
        +addLogger(logger)
    }

    class LoggerBuilder {
        <<abstract>>
        #loggerType_ : LoggerType
        #sinks_ : vector~LogSinkPtr~
        +build()*
        +buildLoggerName(name)
        +buildLoggerSink(args...)
    }
    class LocalLoggerBuilder {
        +build()
    }
    class GlobalLoggerBuilder {
        +build()
    }

    LoggerBuilder <|-- LocalLoggerBuilder
    LoggerBuilder <|-- GlobalLoggerBuilder
    LoggerManager o-- Logger
    LoggerBuilder ..> Logger : creates
```

## 核心流程时序图 (Sequence Diagram)

### 1. 异步写入流程 (Async Flow)

以下展示了异步日志从用户调用到最终落盘的完整流程：

```mermaid
sequenceDiagram
    participant User as User Thread
    participant Logger as AsyncLogger
    participant Looper as AsyncLooper
    participant ProBuf as Buffer (Pro)
    participant Thread as BgThread
    participant ConBuf as Buffer (Con)
    participant Sink as FileSink

    Note over User, Logger: 前端写入阶段 (极速)
    User->>Logger: LOG(INFO) << "msg"
    Logger->>Looper: push(data, len)
    activate Looper
    Looper->>Looper: Spinlock.lock() (CAS + Backoff)
    Looper->>ProBuf: append(data)
    Looper->>Looper: Spinlock.unlock()
    
    opt Buffer Full / Timeout
        Looper->>Thread: notify_one() (Condition Variable)
    end
    deactivate Looper
    
    Note over Thread, Sink: 后端处理阶段 (批量)
    activate Thread
    Thread->>Thread: Wait for Signal
    Thread->>Looper: Spinlock.lock()
    Thread->>Looper: Swap(ProBuf, ConBuf)
    Thread->>Looper: Spinlock.unlock()
    
    loop Process ConBuf
        Thread->>Sink: log(batch_data)
        Sink->>Sink: fwrite / flush
    end
    Thread->>ConBuf: reset()
    deactivate Thread
```

### 2. 同步写入流程 (Sync Flow)

同步模式下，调用链直接且简单，保证数据即时落盘。

```mermaid
sequenceDiagram
    participant User as User Thread
    participant Logger as SyncLogger
    participant Sink as FileSink

    User->>Logger: LOG(INFO) << "msg"
    activate Logger
    Logger->>Logger: Format Message (Thread Local Buffer)
    Logger->>Sink: log(data, len)
    activate Sink
    Sink->>Sink: fwrite
    Sink->>Sink: flush
    deactivate Sink
    deactivate Logger
```

## 性能表现

在 16 线程并发写入 8 字节小包的极端场景下：
- **Zlog Async**: ~203 ns
- **Spdlog Async**: ~12,294 ns
- **Glog**: ~11,442 ns

Zlog 实现了 **60倍** 于竞品的性能提升。

## 编译与运行

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
./tests/zlog_benchmark
```
