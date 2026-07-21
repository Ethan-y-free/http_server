#!/bin/bash
# ============================================================
# Day 20 压测脚本 — 单 Reactor vs 主从 Reactor HTTP Server
# 用法：在 Linux VM 上执行
#   chmod +x scripts/bench.sh
#   ./scripts/bench.sh
# ============================================================

PORT_SINGLE=8888
PORT_MULTI=8889
TEST_URL="http://localhost"
TEST_FILE="/index.html"   # 3KB，模拟典型请求

# 压测参数
CONCURRENCY_LEVELS="100 500 1000"
SHORT_CONCURRENCY="50 100 200"
REQUESTS=50000
KEEPALIVE_REQUESTS=100000
TIMELIMIT=30

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  HTTP Server 压测 — Day 20${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# ---- 检查 ab 是否安装 ----
if ! command -v ab &> /dev/null; then
    echo -e "${RED}[错误] ab (ApacheBench) 未安装${NC}"
    echo "  Ubuntu/Debian: sudo apt install apache2-utils"
    echo "  CentOS/RHEL:   sudo yum install httpd-tools"
    exit 1
fi

# ---- 启动服务器函数 ----
start_server() {
    local name=$1
    local binary=$2
    local port=$3

    echo -e "${YELLOW}[启动]${NC} $name (port $port)..."

    # 修改端口号后启动（用 sed 临时改，更简单的方式是传参，但当前代码硬编码了端口）
    # 直接运行，用 timeout 限制
    timeout $((TIMELIMIT + 10)) "$binary" &
    local pid=$!
    sleep 1

    # 检查是否启动成功
    if ! kill -0 $pid 2>/dev/null; then
        echo -e "${RED}[失败]${NC} $name 启动失败"
        return 1
    fi

    echo "  PID=$pid"
    echo "$pid"
}

# ---- 单次压测 ----
run_ab() {
    local label=$1
    local url=$2
    local concurrency=$3
    local total_req=$4
    local keepalive=$5

    local ab_opts="-n $total_req -c $concurrency"
    if [ "$keepalive" = "yes" ]; then
        ab_opts="$ab_opts -k"
    fi

    echo -ne "  ${label} (c=$concurrency, n=$total_req)... "

    # 运行 ab，提取 QPS 和失败数
    local result=$(ab $ab_opts "$url" 2>&1)
    local qps=$(echo "$result" | grep "Requests per second" | awk '{print $4}')
    local failed=$(echo "$result" | grep "Failed requests" | awk '{print $3}')
    local time_per_req=$(echo "$result" | grep "Time per request.*mean" | awk '{print $4}')
    local transfer=$(echo "$result" | grep "Transfer rate" | awk '{print $3}')

    if [ -z "$qps" ]; then
        echo -e "${RED}失败${NC}"
        echo "  ---"
        return
    fi

    echo -e "${GREEN}${qps} QPS${NC} (失败: ${failed:-0}, 平均: ${time_per_req}ms, 吞吐: ${transfer}KB/s)"
}

# ---- 主流程 ----
echo -e "${CYAN}>>> 场景 1：Keep-Alive 长连接压测${NC}"
echo "=========================================="
printf "%-10s | %12s | %12s | %12s\n" "并发数" "单Reactor QPS" "主从Reactor QPS" "提升倍数"
echo "----------------------------------------------------------"

for c in $CONCURRENCY_LEVELS; do
    # 单 Reactor
    SINGLE_QPS=""
    MULTI_QPS=""

    # 启动单 Reactor
    SPID=$(start_server "SingleReactor" "./build/single_reactor_http" $PORT_SINGLE)
    if [ -n "$SPID" ]; then
        RESULT=$(ab -n $KEEPALIVE_REQUESTS -c $c -k "http://localhost:$PORT_SINGLE$TEST_FILE" 2>&1)
        SINGLE_QPS=$(echo "$RESULT" | grep "Requests per second" | awk '{print $4}')
        kill $SPID 2>/dev/null
        wait $SPID 2>/dev/null
        sleep 0.5
    fi

    # 启动主从 Reactor
    MPID=$(start_server "MultiReactor" "./build/multi_reactor_http" $PORT_MULTI)
    if [ -n "$MPID" ]; then
        RESULT=$(ab -n $KEEPALIVE_REQUESTS -c $c -k "http://localhost:$PORT_MULTI$TEST_FILE" 2>&1)
        MULTI_QPS=$(echo "$RESULT" | grep "Requests per second" | awk '{print $4}')
        kill $MPID 2>/dev/null
        wait $MPID 2>/dev/null
        sleep 0.5
    fi

    # 计算提升倍数
    if [ -n "$SINGLE_QPS" ] && [ -n "$MULTI_QPS" ]; then
        RATIO=$(echo "scale=2; $MULTI_QPS / $SINGLE_QPS" | bc 2>/dev/null || echo "N/A")
    else
        RATIO="N/A"
    fi

    printf "%-10s | %12s | %12s | %12s\n" "$c" "${SINGLE_QPS:-N/A}" "${MULTI_QPS:-N/A}" "${RATIO}x"
done

echo ""
echo -e "${CYAN}>>> 场景 2：短连接压测 (no Keep-Alive)${NC}"
echo "=========================================="
printf "%-10s | %12s | %12s | %12s\n" "并发数" "单Reactor QPS" "主从Reactor QPS" "提升倍数"
echo "----------------------------------------------------------"

for c in $SHORT_CONCURRENCY; do
    SINGLE_QPS=""
    MULTI_QPS=""

    SPID=$(start_server "SingleReactor" "./build/single_reactor_http" $PORT_SINGLE)
    if [ -n "$SPID" ]; then
        RESULT=$(ab -n $REQUESTS -c $c "http://localhost:$PORT_SINGLE$TEST_FILE" 2>&1)
        SINGLE_QPS=$(echo "$RESULT" | grep "Requests per second" | awk '{print $4}')
        kill $SPID 2>/dev/null
        wait $SPID 2>/dev/null
        sleep 0.5
    fi

    MPID=$(start_server "MultiReactor" "./build/multi_reactor_http" $PORT_MULTI)
    if [ -n "$MPID" ]; then
        RESULT=$(ab -n $REQUESTS -c $c "http://localhost:$PORT_MULTI$TEST_FILE" 2>&1)
        MULTI_QPS=$(echo "$RESULT" | grep "Requests per second" | awk '{print $4}')
        kill $MPID 2>/dev/null
        wait $MPID 2>/dev/null
        sleep 0.5
    fi

    if [ -n "$SINGLE_QPS" ] && [ -n "$MULTI_QPS" ]; then
        RATIO=$(echo "scale=2; $MULTI_QPS / $SINGLE_QPS" | bc 2>/dev/null || echo "N/A")
    else
        RATIO="N/A"
    fi

    printf "%-10s | %12s | %12s | %12s\n" "$c" "${SINGLE_QPS:-N/A}" "${MULTI_QPS:-N/A}" "${RATIO}x"
done

echo ""
echo -e "${CYAN}>>> 场景 3：大文件压测 (map.html ~32KB)${NC}"
echo "=========================================="

MPID=$(start_server "MultiReactor" "./build/multi_reactor_http" $PORT_MULTI)
if [ -n "$MPID" ]; then
    echo "  并发=500, Keep-Alive:"
    run_ab "  " "http://localhost:$PORT_MULTI/map.html" 500 $KEEPALIVE_REQUESTS "yes"
    echo "  并发=200, 短连接:"
    run_ab "  " "http://localhost:$PORT_MULTI/map.html" 200 $REQUESTS "no"
    kill $MPID 2>/dev/null
    wait $MPID 2>/dev/null
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  压测完成！${NC}"
echo -e "${GREEN}========================================${NC}"
