#!/bin/bash
# 性能分析脚本 - 支持独立栈和共享栈模式对比测试

set -e

PORT=9000
THREADS=4
DURATION=30
MODE=${1:-"normal"}  # normal/shared/both

run_perf_test() {
    local use_shared=$1
    local prefix=$2
    
    # 创建输出目录
    local output_dir="perf_results/${prefix}_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$output_dir"
    echo "输出目录: $output_dir"
    
    echo ""
    echo "=========================================="
    if [ "$use_shared" = "true" ]; then
        echo "性能分析: 共享栈模式"
    else
        echo "性能分析: 独立栈模式"
    fi
    echo "=========================================="
    
    # 构建启动命令
    local cmd="./tests/perf_server_bench -p $PORT -t $THREADS -d $DURATION"
    if [ "$use_shared" = "true" ]; then
        cmd="$cmd -s"
    fi
    
    # 启动服务器
    echo "[1] 启动服务器: port=$PORT, threads=$THREADS, duration=$DURATION, shared_stack=$use_shared"
    $cmd > ${output_dir}/server.log 2>&1 &
    SERVER_PID=$!
    echo "服务器PID: $SERVER_PID"

    # 等待服务器启动
    sleep 2
    
    # 检查服务器是否运行
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "错误: 服务器启动失败"
        cat ${output_dir}/server.log
        return 1
    fi
    
    echo "[2] 使用perf记录CPU性能数据 (18秒)..."
    perf record -F 99 -g -p $SERVER_PID -o ${output_dir}/perf_cpu.data -- sleep 18 2>${output_dir}/perf_record.err &
    PERF_PID=$!
    
    echo "[3] 使用perf记录L1缓存失效数据 (18秒)..."
    perf record -e L1-dcache-load-misses -c 10000 -g -p $SERVER_PID -o ${output_dir}/perf_l1_miss.data -- sleep 18 2>${output_dir}/perf_l1_record.err &
    L1_PID=$!
    
    echo "[4] 使用perf记录L2缓存失效数据 (18秒)..."
    perf record -e l2_rqsts.miss -c 5000 -g -p $SERVER_PID -o ${output_dir}/perf_l2_miss.data -- sleep 18 2>${output_dir}/perf_l2_record.err &
    L2_PID=$!
    
    echo "[5] 使用perf记录L3缓存失效数据 (18秒)..."
    perf record -e LLC-load-misses -c 1000 -g -p $SERVER_PID -o ${output_dir}/perf_l3_miss.data -- sleep 18 2>${output_dir}/perf_l3_record.err &
    L3_PID=$!
    
    echo "[6] 使用perf stat记录缓存命中率..."
    perf stat -e cache-references,cache-misses,instructions,cycles,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses -p $SERVER_PID sleep 18 2>${output_dir}/perf_stat.txt &
    STAT_PID=$!
    
    # 等待1秒让perf开始记录
    sleep 1
    
    echo "[7] 运行wrk压测..."
    wrk -t4 -c200 -d15s http://localhost:$PORT/ > ${output_dir}/wrk_result.txt 2>&1 || echo "wrk执行完成"
    
    # 等待perf完成
    echo "[8] 等待perf记录完成..."
    wait $PERF_PID 2>/dev/null || true
    wait $L1_PID 2>/dev/null || true
    wait $L2_PID 2>/dev/null || true
    wait $L3_PID 2>/dev/null || true
    wait $STAT_PID 2>/dev/null || true
    
    echo "[9] 等待服务器结束..."
    wait $SERVER_PID 2>/dev/null || true

    echo ""
    echo "=========================================="
    echo "性能分析报告 - $prefix"
    echo "=========================================="
    
    echo ""
    echo "=== WRK压测结果 ==="
    cat ${output_dir}/wrk_result.txt
    
    echo ""
    echo "=== Perf Stat缓存统计 ==="
    cat ${output_dir}/perf_stat.txt 2>/dev/null || echo "perf stat数据收集失败"
    
    echo ""
    echo "=== 生成perf性能数据 ==="
    if [ -f ${output_dir}/perf_cpu.data ]; then
        perf report -i ${output_dir}/perf_cpu.data -n --stdio --sort symbol,dso 2>/dev/null > ${output_dir}/perf_functions.txt || echo "perf report生成失败"
        echo "函数性能数据 (前100行):"
        head -100 ${output_dir}/perf_functions.txt
    else
        echo "perf_cpu.data文件不存在"
    fi
    
    echo ""
    echo "=== 生成L1缓存失效分析 ==="
    if [ -f ${output_dir}/perf_l1_miss.data ]; then
        echo "[L1分析1] L1缓存失效最严重的函数 (top 30):"
        perf report -i ${output_dir}/perf_l1_miss.data -n --stdio --sort symbol,dso --percent-limit 0.5 2>/dev/null | head -80 > ${output_dir}/l1_miss_functions.txt
        cat ${output_dir}/l1_miss_functions.txt
        
        echo ""
        echo "[L1分析2] 生成L1指令级缓存失效注解..."
        top_funcs=$(perf report -i ${output_dir}/perf_l1_miss.data --stdio -n --sort symbol 2>/dev/null | grep -E '^[[:space:]]*[0-9]+\.[0-9]+%' | head -3 | awk '{print $NF}')
        
        > ${output_dir}/l1_miss_annotate.txt
        for func in $top_funcs; do
            echo "" >> ${output_dir}/l1_miss_annotate.txt
            echo "=== 函数: $func 的L1指令级缓存失效 ===" >> ${output_dir}/l1_miss_annotate.txt
            perf annotate -i ${output_dir}/perf_l1_miss.data -s $func --stdio 2>/dev/null | head -100 >> ${output_dir}/l1_miss_annotate.txt || echo "无法注解函数: $func" >> ${output_dir}/l1_miss_annotate.txt
        done
        
        echo "L1缓存失效指令级分析已保存到: ${output_dir}/l1_miss_annotate.txt"
        echo "预览前50行:"
        head -50 ${output_dir}/l1_miss_annotate.txt 2>/dev/null || echo "L1注解文件为空"
    else
        echo "perf_l1_miss.data文件不存在"
    fi
    
    echo ""
    echo "=== 生成L2缓存失效分析 ==="
    if [ -f ${output_dir}/perf_l2_miss.data ]; then
        echo "[L2分析1] L2缓存失效最严重的函数 (top 30):"
        perf report -i ${output_dir}/perf_l2_miss.data -n --stdio --sort symbol,dso --percent-limit 0.5 2>/dev/null | head -80 > ${output_dir}/l2_miss_functions.txt
        cat ${output_dir}/l2_miss_functions.txt
        
        echo ""
        echo "[L2分析2] 生成L2指令级缓存失效注解..."
        top_funcs=$(perf report -i ${output_dir}/perf_l2_miss.data --stdio -n --sort symbol 2>/dev/null | grep -E '^[[:space:]]*[0-9]+\.[0-9]+%' | head -3 | awk '{print $NF}')
        
        > ${output_dir}/l2_miss_annotate.txt
        for func in $top_funcs; do
            echo "" >> ${output_dir}/l2_miss_annotate.txt
            echo "=== 函数: $func 的L2指令级缓存失效 ===" >> ${output_dir}/l2_miss_annotate.txt
            perf annotate -i ${output_dir}/perf_l2_miss.data -s $func --stdio 2>/dev/null | head -100 >> ${output_dir}/l2_miss_annotate.txt || echo "无法注解函数: $func" >> ${output_dir}/l2_miss_annotate.txt
        done
        
        echo "L2缓存失效指令级分析已保存到: ${output_dir}/l2_miss_annotate.txt"
        echo "预览前50行:"
        head -50 ${output_dir}/l2_miss_annotate.txt 2>/dev/null || echo "L2注解文件为空"
    else
        echo "perf_l2_miss.data文件不存在"
    fi
    
    echo ""
    echo "=== 生成L3缓存失效分析 ==="
    if [ -f ${output_dir}/perf_l3_miss.data ]; then
        echo "[L3分析1] L3缓存失效最严重的函数 (top 30):"
        perf report -i ${output_dir}/perf_l3_miss.data -n --stdio --sort symbol,dso --percent-limit 0.5 2>/dev/null | head -80 > ${output_dir}/l3_miss_functions.txt
        cat ${output_dir}/l3_miss_functions.txt
        
        echo ""
        echo "[L3分析2] 生成L3指令级缓存失效注解..."
        top_funcs=$(perf report -i ${output_dir}/perf_l3_miss.data --stdio -n --sort symbol 2>/dev/null | grep -E '^[[:space:]]*[0-9]+\.[0-9]+%' | head -3 | awk '{print $NF}')
        
        > ${output_dir}/l3_miss_annotate.txt
        for func in $top_funcs; do
            echo "" >> ${output_dir}/l3_miss_annotate.txt
            echo "=== 函数: $func 的L3指令级缓存失效 ===" >> ${output_dir}/l3_miss_annotate.txt
            perf annotate -i ${output_dir}/perf_l3_miss.data -s $func --stdio 2>/dev/null | head -100 >> ${output_dir}/l3_miss_annotate.txt || echo "无法注解函数: $func" >> ${output_dir}/l3_miss_annotate.txt
        done
        
        echo "L3缓存失效指令级分析已保存到: ${output_dir}/l3_miss_annotate.txt"
        echo "预览前50行:"
        head -50 ${output_dir}/l3_miss_annotate.txt 2>/dev/null || echo "L3注解文件为空"
    else
        echo "perf_l3_miss.data文件不存在"
    fi
    
    echo ""
    echo "=========================================="
    echo "分析文件已生成到目录: $output_dir"
    echo "=========================================="
    echo "数据文件:"
    echo "  - perf_cpu.data: CPU性能数据"
    echo "  - perf_l1_miss.data: L1缓存失效采样数据"
    echo "  - perf_l2_miss.data: L2缓存失效采样数据"
    echo "  - perf_l3_miss.data: L3缓存失效采样数据"
    echo "  - perf_stat.txt: 缓存统计"
    echo ""
    echo "报告文件:"
    echo "  - perf_functions.txt: 所有函数性能数据"
    echo "  - l1_miss_functions.txt: L1缓存失效函数排名"
    echo "  - l1_miss_annotate.txt: L1指令级缓存失效分析"
    echo "  - l2_miss_functions.txt: L2缓存失效函数排名"
    echo "  - l2_miss_annotate.txt: L2指令级缓存失效分析"
    echo "  - l3_miss_functions.txt: L3缓存失效函数排名"
    echo "  - l3_miss_annotate.txt: L3指令级缓存失效分析"
    echo "  - wrk_result.txt: 压测结果"
    echo "  - server.log: 服务器日志"
    echo "=========================================="
}

# 主流程
case $MODE in
    "normal")
        run_perf_test "false" "normal"
        ;;
    "shared")
        run_perf_test "true" "shared"
        ;;
    "both")
        run_perf_test "false" "normal"
        sleep 3
        run_perf_test "true" "shared"
        
        echo ""
        echo "=========================================="
        echo "性能对比总结"
        echo "=========================================="
        echo ""
        echo "--- 独立栈模式 ---"
        find perf_results -name "normal_*" -type d | sort | tail -1 | xargs -I {} sh -c 'grep -E "Requests/sec|Transfer/sec" {}/wrk_result.txt || true'
        find perf_results -name "normal_*" -type d | sort | tail -1 | xargs -I {} sh -c 'grep -E "cache-misses|L1-dcache-load-misses" {}/perf_stat.txt | head -2 || true'
        echo ""
        echo "--- 共享栈模式 ---"
        find perf_results -name "shared_*" -type d | sort | tail -1 | xargs -I {} sh -c 'grep -E "Requests/sec|Transfer/sec" {}/wrk_result.txt || true'
        find perf_results -name "shared_*" -type d | sort | tail -1 | xargs -I {} sh -c 'grep -E "cache-misses|L1-dcache-load-misses" {}/perf_stat.txt | head -2 || true'
        ;;
    *)
        echo "用法: $0 [normal|shared|both]"
        echo "  normal: 仅测试独立栈模式"
        echo "  shared: 仅测试共享栈模式"
        echo "  both:   对比测试两种模式"
        exit 1
        ;;
esac
