#!/usr/bin/env bash
# bench/run.sh — WebZero benchmark script
# Requires: wrk, httpd (or any server on a different port for comparison)
#
# Usage:
#   ./bench/run.sh <bundle.web> [port]

set -euo pipefail

BUNDLE="${1:-examples/landing-page.web}"
PORT="${2:-8080}"
DURATION=30
THREADS=4
CONNECTIONS=50

echo "========================================"
echo "  WebZero Benchmark"
echo "  Bundle     : $BUNDLE"
echo "  Port       : $PORT"
echo "  Duration   : ${DURATION}s"
echo "  Threads    : $THREADS  Connections: $CONNECTIONS"
echo "========================================"

# Build the example bundle if it doesn't exist
if [ ! -f "$BUNDLE" ]; then
  echo "[bench] Building bundle..."
  node tools/wz.js build examples/landing-page
fi

# Ensure webzero binary exists
if [ ! -f "./webzero" ]; then
  echo "[bench] Building webzero..."
  make
fi

# Start webzero in background
echo "[bench] Starting webzero..."
./webzero "$BUNDLE" "$PORT" &
WZ_PID=$!
trap 'kill "$WZ_PID" 2>/dev/null || true' EXIT
sleep 0.5

# Warm-up pass
echo "[bench] Warm-up (5s)..."
wrk -t2 -c20 -d5s "http://localhost:$PORT/" >/dev/null 2>&1 || true

# Main benchmark
echo ""
echo "[bench] Main run (${DURATION}s, ${THREADS}T/${CONNECTIONS}C)..."
wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
    --latency \
    "http://localhost:$PORT/"

# Cleanup
kill "$WZ_PID" 2>/dev/null || true

echo ""
echo "[bench] Done."
