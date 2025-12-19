#include "runtime/shared_stack_pool.h"
#include "runtime/fiber.h"
#include "zcoroutine_logger.h"
#include <cstdlib>
#include <cstring>
#include <memory>

namespace zcoroutine {

SharedStackPool::SharedStackPool(int count, size_t stack_size)
    : count_(count)
    , stack_size_(stack_size)
    , alloc_idx_(0) {
    
    ZCOROUTINE_LOG_INFO("SharedStackPool creating: count={}, stack_size={}", count, stack_size);

    // 预分配所有共享栈
    stack_array_.reserve(count);
    for (int i = 0; i < count; ++i) {
        auto stack_mem = std::make_unique<StackMem>();
        stack_mem->stack_size = stack_size;
        stack_mem->stack_buffer = static_cast<char*>(malloc(stack_size));
        stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;  // 栈顶=栈底+栈大小
        stack_mem->occupy_fiber = nullptr;
        
        if (!stack_mem->stack_buffer) {
            ZCOROUTINE_LOG_FATAL("SharedStackPool malloc failed: index={}, stack_size={}", i, stack_size);
            abort();
        }
        
        stack_array_.push_back(std::move(stack_mem));
        ZCOROUTINE_LOG_DEBUG("SharedStackPool allocated stack: index={}, buffer={}, bp={}", 
                             i, static_cast<void*>(stack_array_.back()->stack_buffer), 
                             static_cast<void*>(stack_array_.back()->stack_bp));
    }
    
    ZCOROUTINE_LOG_INFO("SharedStackPool created: count={}, stack_size={}, total_memory={}KB", 
                        count, stack_size, (count * stack_size) / 1024);
}

SharedStackPool::~SharedStackPool() {
    ZCOROUTINE_LOG_INFO("SharedStackPool destroying: count={}", count_);
    
    size_t freed_count = 0;
    for (const auto& stack_mem : stack_array_) {
        if (stack_mem) {
            if (stack_mem->stack_buffer) {
                free(stack_mem->stack_buffer);
                freed_count++;
            }
        }
    }
    stack_array_.clear();
    
    ZCOROUTINE_LOG_INFO("SharedStackPool destroyed: freed {} stacks", freed_count);
}

StackMem* SharedStackPool::allocate_stack() {
    // 轮询分配策略
    unsigned int idx = alloc_idx_.fetch_add(1, std::memory_order_relaxed) % count_;
    StackMem* stack = stack_array_[idx].get();
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::allocate_stack: allocated index={}, buffer={}", 
                         idx, static_cast<void*>(stack->stack_buffer));
    return stack;
}

void SharedStackPool::save_stack(Fiber* fiber) {
    if (!fiber) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::save_stack failed: null fiber");
        return;
    }
    
    // 获取Fiber的共享栈信息
    StackMem* shared_stack = nullptr;
    char* stack_sp = nullptr;
    size_t* save_size_ref = nullptr;
    char** save_buffer_ref = nullptr;
    
    // 通过指针的指针直接访问Fiber的私有成员，避免增加公共接口
    // 这样可以在不破坏封装性的情况下实现功能
    fiber->get_shared_stack_info(&shared_stack, &stack_sp, &save_size_ref, &save_buffer_ref);
    
    if (!shared_stack || !stack_sp || !save_size_ref) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::save_stack failed: invalid fiber stack info");
        return;
    }
    
    // 检查共享栈是否被当前协程占用
    if (shared_stack->occupy_fiber != fiber) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::save_stack warning: fiber not occupying stack, fiber_id={}", 
                           fiber->id());
    }
    
    // 计算需要保存的栈数据大小
    // stack_bp是栈顶（高地址），stack_sp是当前栈指针（低地址）
    size_t copy_size = shared_stack->stack_bp - stack_sp;
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::save_stack: fiber_id={}, copy_size={}", 
                         fiber->id(), copy_size);
    
    // 如果需要保存的数据大于0，则进行保存
    if (copy_size > 0) {
        // 如果保存缓冲区不够大，重新分配
        if (!*save_buffer_ref || *save_size_ref < copy_size) {
            // 使用realloc重新分配内存
            char* new_buffer = static_cast<char*>(realloc(*save_buffer_ref, copy_size));
            if (!new_buffer) {
                ZCOROUTINE_LOG_FATAL("SharedStackPool::save_stack realloc failed: size={}", copy_size);
                abort();
            }
            *save_buffer_ref = new_buffer;
            *save_size_ref = copy_size;
        }
        
        // 拷贝栈数据到保存缓冲区
        if (*save_buffer_ref) {
            memcpy(*save_buffer_ref, stack_sp, copy_size);
        }
    }
    
    // 清除占用标记
    shared_stack->occupy_fiber = nullptr;
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::save_stack success: fiber_id={}, copy_size={}", 
                         fiber->id(), copy_size);
}

void SharedStackPool::restore_stack(Fiber* fiber) {
    if (!fiber) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::restore_stack failed: null fiber");
        return;
    }
    
    // 获取Fiber的共享栈信息
    StackMem* shared_stack = nullptr;
    char** stack_sp_ref = nullptr;
    size_t* save_size_ref = nullptr;
    char** save_buffer_ref = nullptr;
    
    // 通过指针的指针直接访问Fiber的私有成员
    fiber->get_restore_stack_info(&shared_stack, &stack_sp_ref, &save_size_ref, &save_buffer_ref);
    
    if (!shared_stack || !stack_sp_ref || !save_size_ref) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::restore_stack failed: invalid fiber stack info");
        return;
    }
    
    // 检查共享栈是否已被其他协程占用
    if (shared_stack->occupy_fiber != nullptr && shared_stack->occupy_fiber != fiber) {
        ZCOROUTINE_LOG_WARN("SharedStackPool::restore_stack conflict: stack occupied by fiber_id={}, "
                           "current fiber_id={}", 
                           shared_stack->occupy_fiber ? shared_stack->occupy_fiber->id() : 0, 
                           fiber->id());
    }
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::restore_stack: fiber_id={}, restore_size={}", 
                         fiber->id(), *save_size_ref);
    
    // 设置占用标记
    shared_stack->occupy_fiber = fiber;
    
    // 恢复栈指针位置
    if (*save_size_ref > 0) {
        // 确保不会越界
        if (shared_stack->stack_bp >= *save_size_ref) {
            *stack_sp_ref = shared_stack->stack_bp - *save_size_ref;
            
            // 恢复栈数据
            if (*save_buffer_ref) {
                memcpy(*stack_sp_ref, *save_buffer_ref, *save_size_ref);
            }
        } else {
            ZCOROUTINE_LOG_WARN("SharedStackPool::restore_stack failed: invalid stack size, bp={}, size={}", 
                               shared_stack->stack_bp, *save_size_ref);
            *stack_sp_ref = shared_stack->stack_bp;
        }
    } else {
        // 如果没有保存的数据，将栈指针设置为栈顶
        *stack_sp_ref = shared_stack->stack_bp;
    }
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::restore_stack success: fiber_id={}, restore_size={}", 
                         fiber->id(), *save_size_ref);
}

} // namespace zcoroutine
