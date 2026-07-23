#!/bin/bash
# ============================================================
# Day 20 压测脚本 — v1（模块化） vs multi_reactor（原始版）
# 用法：在 Linux VM 的 week01 目录下执行
#   chmod +x scripts/bench.sh
#   cd build && cmake .. && make v1_http_server multi_reactor_http -j$(nproc)
#   ../scripts/bench.sh
# ============================================================

set -euo pipefail

PORT=8888
TEST_URL="http://localhost:${PORT}"
TEST_FILE="/index.html"
BIG_FILE="/map.html"

# 压测参数
CONCURRENCY_LEVELS="100 500 1000"
SHORT_CONCURRENCY="50 100 200"
KEEPALIVE_REQUESTS=100000
SHORT_REQUESTS=50000

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${CYAN}============================================${NC}"
echo -e "${CYAN}  Day 20 压测 — v1 vs multi_reactor${NC}"
echo -e "${CYAN}============================================${NC}"
echo ""

# ---- 检查工具 ----
for tool in ab curl; do
    if ! command -v $tool &> /dev/null; then
        echo -e "${RED}[错误] $tool 未安装${NC}"
        echo "  Ubuntu/Debian: sudo apt install apache2-utils curl"
        exit 1
    fi
done

# ---- 检查二进制 ----
BIN_V1="./v1_http_server"
BIN_MULTI="./multi_reactor_http"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"

if [ ! -f "${BUILD_DIR}/v1_http_server" ]; then
    echo -e "${RED}[错误] 找不到 ${BUILD_DIR}/v1_http_server${NC}"
    echo "  请先编译: cd build && cmake .. && make v1_http_server multi_reactor_http -j\$(nproc)"
    exit 1
fi

# ---- 启动服务器（阻塞等待就绪）----
start_and_wait() {
    local name=$1
    local binary=$2
    local port=$3

    echo -ne "  ${YELLOW}启动${NC} ${name} (port ${port})..."

    "${binary}" &
    local pid=$!

    # 等待端口就绪（最多等 3 秒）
    for i in $(seq 1 30); do
        if curl -s -o /dev/null --max-time 0.5 "http://localhost:${port}/" 2>/dev/null; then
            echo -e " ${GREEN}PID=${pid} ✓${NC}"
            echo "$pid"
            return 0
        fi
        sleep 0.1
    done

    echo -e " ${RED}启动超时${NC}"
    kill $pid 2>/dev/null || true
    return 1
}

# ---- 安全停止 ----
stop_server() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    sleep 0.3
}

# ---- 运行 ab 并提取指标 ----
run_ab() {
    local label=$1
    local url=$2
    local concurrency=$3
    local total_req=$4
    local keepalive=$5

    local ab_opts="-n $total_req -c $concurrency"
    [ "$keepalive" = "yes" ] && ab_opts="$ab_opts -k"

    local result
    result=$(ab $ab_opts "$url" 2>&1) || true

    local qps      failed      tpr      tpr_sd      transfer
    qps=$(echo "$result"      | grep "Requests per second"     | awk '{print $4}')
    failed=$(echo "$result"   | grep "Failed requests"         | awk '{print $3}')
    tpr=$(echo "$result"      | grep "Time per request.*mean"  | awk '{print $4}')
    tpr_sd=$(echo "$result"   | grep "Time per request.*mean.*across" | awk '{print $4}')
    transfer=$(echo "$result" | grep "Transfer rate"           | awk '{print $3}')

    if [ -z "$qps" ]; then
        echo -e "  ${label} ${RED}✗ ab 失败${NC}"
        return 1
    fi

    local fail_disp="${failed:-0}"
    local color="$GREEN"
    echo -e "  ${label} ${color}${qps} QPS${NC} | 失败: ${fail_disp} | 平均延迟: ${tpr:-?}ms | 吞吐: ${transfer:-?}KB/s"

    # 返回 QPS 数值，用于后续对比
    echo "${qps}"
}

# ============================================================
# 场景 1：Keep-Alive 长连接 — v1 vs multi_reactor
# ============================================================
echo -e "${BOLD}${CYAN}═══ 场景 1：Keep-Alive 长连接 — v1 vs multi_reactor ═══${NC}"
echo ""
printf "%-8s | %18s | %18s | %10s\n" "并发" "v1_http_server" "multi_reactor" "v1 提升"
echo "-------------------------------------------------------------------------"

for c in $CONCURRENCY_LEVELS; do
    V1_QPS=""
    MULTI_QPS=""

    # ① 测 v1_http_server
    PID=$(start_and_wait "v1"        "${BUILD_DIR}/v1_http_server"       $PORT)
    if [ -n "$PID" ]; then
        V1_QPS=$(run_ab "  v1       " "${TEST_URL}${TEST_FILE}" $c $KEEPALIVE_REQUESTS "yes")
        stop_server "$PID"
    fi

    # ② 测 multi_reactor_http（端口相同，先后测）
    PID=$(start_and_wait "multi_rx" "${BUILD_DIR}/multi_reactor_http"   $PORT)
    if [ -n "$PID" ]; then
        MULTI_QPS=$(run_ab "  multi_rx" "${TEST_URL}${TEST_FILE}" $c $KEEPALIVE_REQUESTS "yes")
        stop_server "$PID"
    fi

    # 计算提升
    if [ -n "$V1_QPS" ] && [ -n "$MULTI_QPS" ] && [ "$MULTI_QPS" != "0" ]; then
        RATIO=$(echo "scale=2; $V1_QPS / $MULTI_QPS" | bc 2>/dev/null || echo "N/A")
    else
        RATIO="N/A"
    fi

    printf "%-8s | %18s | %18s | %8sx\n" "$c" "${V1_QPS:-N/A}" "${MULTI_QPS:-N/A}" "${RATIO}"
done

echo ""

# ============================================================
# 场景 2：短连接 — v1 vs multi_reactor
# ============================================================
echo -e "${BOLD}${CYAN}═══ 场景 2：短连接（无 Keep-Alive）— v1 vs multi_reactor ═══${NC}"
echo ""
printf "%-8s | %18s | %18s | %10s\n" "并发" "v1_http_server" "multi_reactor" "v1 提升"
echo "-------------------------------------------------------------------------"

for c in $SHORT_CONCURRENCY; do
    V1_QPS=""
    MULTI_QPS=""

    PID=$(start_and_wait "v1"        "${BUILD_DIR}/v1_http_server"       $PORT)
    if [ -n "$PID" ]; then
        V1_QPS=$(run_ab "  v1       " "${TEST_URL}${TEST_FILE}" $c $SHORT_REQUESTS "no")
        stop_server "$PID"
    fi

    PID=$(start_and_wait "multi_rx" "${BUILD_DIR}/multi_reactor_http"   $PORT)
    if [ -n "$PID" ]; then
        MULTI_QPS=$(run_ab "  multi_rx" "${TEST_URL}${TEST_FILE}" $c $SHORT_REQUESTS "no")
        stop_server "$PID"
    fi

    if [ -n "$V1_QPS" ] && [ -n "$MULTI_QPS" ] && [ "$MULTI_QPS" != "0" ]; then
        RATIO=$(echo "scale=2; $V1_QPS / $MULTI_QPS" | bc 2>/dev/null || echo "N/A")
    else
        RATIO="N/A"
    fi

    printf "%-8s | %18s | %18s | %8sx\n" "$c" "${V1_QPS:-N/A}" "${MULTI_QPS:-N/A}" "${RATIO}"
done

echo ""

# ============================================================
# 场景 3：大文件压测（multi_reactor 单跑）
# ============================================================
echo -e "${BOLD}${CYAN}═══ 场景 3：大文件（map.html）— multi_reactor 单跑 ═══${NC}"
echo ""

PID=$(start_and_wait "multi_rx" "${BUILD_DIR}/multi_reactor_http" $PORT)
if [ -n "$PID" ]; then
    run_ab "  Keep-Alive c=500" "${TEST_URL}${BIG_FILE}" 500 $KEEPALIVE_REQUESTS "yes"
    run_ab "  短连接   c=200" "${TEST_URL}${BIG_FILE}" 200 $SHORT_REQUESTS "no"
    stop_server "$PID"
fi

echo ""

# ============================================================
# 场景 4：极限并发（v1_http_server）
# ============================================================
echo -e "${BOLD}${CYAN}═══ 场景 4：v1_http_server 极限并发 ═══${NC}"
echo ""

PID=$(start_and_wait "v1" "${BUILD_DIR}/v1_http_server" $PORT)
if [ -n "$PID" ]; then
    for c in 500 1000 2000; do
        run_ab "  Keep-Alive c=$c" "${TEST_URL}${TEST_FILE}" $c $KEEPALIVE_REQUESTS "yes"
    done
    stop_server "$PID"
fi

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Day 20 压测完成！${NC}"
echo -e "${GREEN}  将结果记录到 DESIGN.md  §二十${NC}"
echo -e "${GREEN}============================================${NC}"
