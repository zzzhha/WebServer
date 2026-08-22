#!/usr/bin/env bash
# HTTP 压测脚本：对比 reactor / proactor 两种模式在 /index.html 上的 QPS、P95、P99。
# 用法: bench_http.sh <reactor|proactor> <port>
set -u

MODE="$1"
PORT="${2:-8080}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
SERVER_BIN="$BUILD_DIR/cppBackend/httpserver"
WORKDIR="$BUILD_DIR"   # 服务器在 build/ 下运行，./html 解析为 build/html（含 index.html）

CONCURRENCIES=(1 10 50 100 200 400)
REQUESTS=30000
URL="http://127.0.0.1:${PORT}/index.html"
RESULT_DIR="$ROOT/test/results/${MODE}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"

start_server() {
  ( cd "$WORKDIR" && WEBSERVER_NET_MODE="$MODE" WEBSERVER_LOG_LEVEL=error \
      "$SERVER_BIN" 127.0.0.1 "$PORT" >"$RESULT_DIR/server.log" 2>&1 &
    echo $! >"$RESULT_DIR/server.pid" )
  for _ in $(seq 1 50); do
    if curl -s -o /dev/null --max-time 1 "$URL"; then
      echo "server up (mode=$MODE port=$PORT pid=$(cat "$RESULT_DIR/server.pid"))"
      return 0
    fi
    sleep 0.2
  done
  echo "SERVER FAILED TO START"; cat "$RESULT_DIR/server.log"; exit 1
}

stop_server() {
  local pid
  [ -f "$RESULT_DIR/server.pid" ] && pid="$(cat "$RESULT_DIR/server.pid")" || return 0
  kill "$pid" 2>/dev/null
  sleep 0.5
  kill -9 "$pid" 2>/dev/null
  echo "server stopped"
}

percentiles() {
  # $1: ab -g 输出文件；打印 P50/P95/P99/mean/max
  python3 - "$1" <<'EOF'
import csv, sys
totals = []
col = 5  # 新版 ab: starttime,seconds,ctime,dttime,twait,total
with open(sys.argv[1]) as f:
    r = csv.reader(f, delimiter='\t')
    header = next(r, None)
    if header and 'ttime' in header and 'total' not in header:
        col = 4  # 旧版 ab: starttime,seconds,ctime,dtime,ttime,wait
    for row in r:
        if len(row) > col:
            try:
                totals.append(float(row[col]))  # 单位 ms
            except ValueError:
                pass
if not totals:
    print("  (no data)"); sys.exit(0)
totals.sort()
n = len(totals)
def pct(p):
    k = max(0, min(n - 1, int(p / 100.0 * n)))
    return totals[k]
print(f"  n={n} mean={sum(totals)/n:.3f}ms p50={pct(50):.3f}ms p95={pct(95):.3f}ms p99={pct(99):.3f}ms max={totals[-1]:.3f}ms")
EOF
}

start_server

SUMMARY="$RESULT_DIR/summary.txt"
: >"$SUMMARY"

for C in "${CONCURRENCIES[@]}"; do
  echo "=== concurrency=$C ===" | tee -a "$SUMMARY"
  DAT="$RESULT_DIR/c${C}.dat"
  OUT="$RESULT_DIR/c${C}.txt"
  ab -k -n "$REQUESTS" -c "$C" -s 60 -g "$DAT" "$URL" >"$OUT" 2>&1
  QPS=$(grep "Requests per second" "$OUT" | awk '{print $4}')
  FAILED=$(grep "Failed requests" "$OUT" | awk '{print $3}')
  echo "  QPS=$QPS failed=$FAILED" | tee -a "$SUMMARY"
  echo "  latency(total):" | tee -a "$SUMMARY"
  percentiles "$DAT" | tee -a "$SUMMARY"
  grep -E "Non-2xx|Connection Errors" "$OUT" | sed 's/^/  /' | tee -a "$SUMMARY"
done

stop_server
echo "DONE: results in $RESULT_DIR"
