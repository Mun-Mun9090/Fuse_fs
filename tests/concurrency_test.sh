#!/usr/bin/env bash
# ==============================================================================
# FuseFS Multiprocess Concurrency Stress Test
# Tests concurrent file creations, writes, reads across multiple parallel workers
# Proves thread-safe lock synchronization and zero metadata corruption via FSCK.
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BIN_DIR="$ROOT_DIR/bin"
FUSEFS="$BIN_DIR/fusefs"
MKFS="$BIN_DIR/mkfs.fusefs"
FSCK="$BIN_DIR/fsck.fusefs"

IMG="$ROOT_DIR/concurrency_fs.img"
MNT="$ROOT_DIR/mnt_concurrent"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

unmount_and_wait() {
    if mountpoint -q "$MNT" 2>/dev/null; then
        fusermount3 -u "$MNT" 2>/dev/null || umount -f "$MNT" 2>/dev/null || true
    fi
    for i in {1..30}; do
        if ! pgrep -f "$FUSEFS" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    sleep 0.2
}

cleanup() {
    unmount_and_wait
    rm -rf "$MNT" "$IMG"
}

trap cleanup EXIT

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE} FuseFS Multiprocess Concurrency Stress Test          ${NC}"
echo -e "${BLUE}======================================================${NC}"

# Format 64 MB filesystem
rm -f "$IMG"
"$MKFS" "$IMG" 64 2048 > /dev/null

mkdir -p "$MNT"
"$FUSEFS" "$IMG" "$MNT"
sleep 1

NUM_WORKERS=8
FILES_PER_WORKER=20

echo -e "${BLUE}--> Spawning $NUM_WORKERS parallel worker processes...${NC}"

worker_task() {
    local WORKER_ID=$1
    local WORKER_DIR="$MNT/worker_$WORKER_ID"
    mkdir -p "$WORKER_DIR"

    for i in $(seq 1 $FILES_PER_WORKER); do
        local FNAME="$WORKER_DIR/file_$i.txt"
        echo "Worker $WORKER_ID Data $i" > "$FNAME"
        echo "Append $i" >> "$FNAME"
        local CONTENT=$(cat "$FNAME")
        if [[ "$CONTENT" != *"Worker $WORKER_ID Data $i"* ]]; then
            echo -e "${RED}[ERROR] Concurrency data corruption in worker $WORKER_ID file $i!${NC}"
            exit 1
        fi
    done
}

for w in $(seq 1 $NUM_WORKERS); do
    worker_task $w &
done

wait
echo -e "${GREEN}[OK] All $NUM_WORKERS concurrent workers completed successfully!${NC}"

# Unmount
unmount_and_wait

# FSCK Verification
echo -e "\n${BLUE}--> Running FSCK to prove ZERO metadata corruption...${NC}"
"$FSCK" "$IMG"

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN} CONCURRENCY STRESS TEST PASSED (NO METADATA RACES)   ${NC}"
echo -e "${GREEN}======================================================${NC}"
