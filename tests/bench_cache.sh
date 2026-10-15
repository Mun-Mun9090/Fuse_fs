#!/usr/bin/env bash
# ==============================================================================
# FuseFS Cache Performance & Throughput Benchmark
# Compares I/O throughput with Block Cache ENABLED vs Block Cache DISABLED
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BIN_DIR="$ROOT_DIR/bin"
FUSEFS="$BIN_DIR/fusefs"
MKFS="$BIN_DIR/mkfs.fusefs"

IMG="$ROOT_DIR/bench_fs.img"
MNT="$ROOT_DIR/mnt_bench"

unmount_and_wait() {
    if mountpoint -q "$MNT" 2>/dev/null; then
        fusermount3 -u "$MNT" 2>/dev/null || umount -f "$MNT" 2>/dev/null || true
    fi
    for i in {1..30}; do
        if ! pgrep -f "bin/fusefs" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    sleep 0.2
}

cleanup() {
    unmount_and_wait
    rm -rf "$MNT" "$IMG" /tmp/bench_payload.bin
}

trap cleanup EXIT

echo "=========================================================="
echo " FuseFS Buffer Cache Throughput Benchmark"
echo "=========================================================="

# Create 1 MB payload file
dd if=/dev/urandom of=/tmp/bench_payload.bin bs=1K count=1024 status=none

run_benchmark() {
    local CACHE_FLAG="$1"
    local MODE_NAME="$2"

    echo -e "\n--> Running Benchmark: $MODE_NAME..."
    rm -f "$IMG"
    "$MKFS" "$IMG" 64 2048 > /dev/null
    mkdir -p "$MNT"

    "$FUSEFS" $CACHE_FLAG "$IMG" "$MNT"
    sleep 1

    # Write benchmark: write 10 files of 1MB each
    local START_WRITE=$(date +%s%N)
    for i in {1..10}; do
        cp /tmp/bench_payload.bin "$MNT/test_$i.bin"
    done
    sync
    local END_WRITE=$(date +%s%N)
    local DURATION_WRITE_NS=$((END_WRITE - START_WRITE))
    local DURATION_WRITE_SEC=$(awk "BEGIN {print $DURATION_WRITE_NS / 1000000000}")
    local WRITE_MBPS=$(awk "BEGIN {print 10.0 / $DURATION_WRITE_SEC}")

    # Read benchmark: read files repeatedly
    local START_READ=$(date +%s%N)
    for i in {1..10}; do
        cat "$MNT/test_$i.bin" > /dev/null
    done
    local END_READ=$(date +%s%N)
    local DURATION_READ_NS=$((END_READ - START_READ))
    local DURATION_READ_SEC=$(awk "BEGIN {print $DURATION_READ_NS / 1000000000}")
    local READ_MBPS=$(awk "BEGIN {print 10.0 / $DURATION_READ_SEC}")

    unmount_and_wait

    echo "    Write Time : ${DURATION_WRITE_SEC}s (${WRITE_MBPS} MB/s)"
    echo "    Read Time  : ${DURATION_READ_SEC}s (${READ_MBPS} MB/s)"

    eval "$3=$WRITE_MBPS"
    eval "$4=$READ_MBPS"
}

# 1. Without cache
run_benchmark "--no-cache" "Cache DISABLED (--no-cache)" NO_CACHE_WRITE NO_CACHE_READ

# 2. With write-back buffer cache
run_benchmark "--cache-size=2048" "Cache ENABLED (LRU Write-Back)" CACHE_WRITE CACHE_READ

echo -e "\n=========================================================="
echo " BENCHMARK RESULTS & CACHE COMPARISON"
echo "=========================================================="
printf "%-25s | %-15s | %-15s\n" "Mode" "Write (MB/s)" "Read (MB/s)"
echo "----------------------------------------------------------"
printf "%-25s | %-15.2f | %-15.2f\n" "Direct I/O (No Cache)" "$NO_CACHE_WRITE" "$NO_CACHE_READ"
printf "%-25s | %-15.2f | %-15.2f\n" "LRU Write-Back Cache" "$CACHE_WRITE" "$CACHE_READ"
echo "=========================================================="
SPEEDUP_READ=$(awk "BEGIN {printf \"%.1fx\", $CACHE_READ / $NO_CACHE_READ}")
SPEEDUP_WRITE=$(awk "BEGIN {printf \"%.1fx\", $CACHE_WRITE / $NO_CACHE_WRITE}")
echo " Cache Read Speedup  : $SPEEDUP_READ"
echo " Cache Write Speedup : $SPEEDUP_WRITE"
echo "=========================================================="
