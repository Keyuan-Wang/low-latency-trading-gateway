#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

MESSAGES="${MESSAGES:-100000}"
WARMUP="${WARMUP:-1000}"
PORT="${PORT:-9000}"

# Build
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
cmake --build "$BUILD_DIR" --target order_entry_epoll_server order_entry_epoll_bench_client -j"$(nproc)" >/dev/null 2>&1

SERVER="$BUILD_DIR/core/order_entry/order_entry_epoll_server"
CLIENT="$BUILD_DIR/core/order_entry/order_entry_epoll_bench_client"

# Start server
"$SERVER" &
SERVER_PID=$!
sleep 0.3

cleanup() { kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT

# Run benchmark
"$CLIENT" --port "$PORT" --messages "$MESSAGES" --warmup "$WARMUP"
