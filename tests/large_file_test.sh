#!/usr/bin/env bash
# ==============================================================================
# FuseFS Indirect and Double-Indirect Large File Test Battery
# Validates: Direct limit bypass (48 KB), Single Indirect (up to 2 MB),
#            Double Indirect (> 2 MB), and Complete Leak-Free Deallocation.
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BIN_DIR="$ROOT_DIR/bin"
FUSEFS="$BIN_DIR/fusefs"
MKFS="$BIN_DIR/mkfs.fusefs"
FSCK="$BIN_DIR/fsck.fusefs"

IMG="$ROOT_DIR/large_test_fs.img"
MNT="$ROOT_DIR/mnt_large"

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
    rm -rf "$MNT" "$IMG" /tmp/expected_*.bin
}

trap cleanup EXIT

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE} FuseFS Large-File (Indirect & Double-Indirect) Test  ${NC}"
echo -e "${BLUE}======================================================${NC}"

# Format 64 MB filesystem
echo -e "\n${BLUE}--> [1] Formatting 64 MB image...${NC}"
rm -f "$IMG"
"$MKFS" "$IMG" 64 2048

mkdir -p "$MNT"
echo -e "\n${BLUE}--> [2] Mounting FuseFS...${NC}"
"$FUSEFS" "$IMG" "$MNT"
sleep 1

# Test 1: Single indirect (512 KB = 128 blocks > 12 direct blocks)
echo -e "\n${BLUE}--> [3] Testing Single Indirect Block Allocation (512 KB file)...${NC}"
dd if=/dev/urandom of=/tmp/expected_single.bin bs=1K count=512 status=none
cp /tmp/expected_single.bin "$MNT/single_indirect.bin"

ORIG_SHA=$(sha256sum /tmp/expected_single.bin | awk '{print $1}')
READ_SHA=$(sha256sum "$MNT/single_indirect.bin" | awk '{print $1}')

if [ "$ORIG_SHA" != "$READ_SHA" ]; then
    echo -e "${RED}[FAILED] Single indirect data corruption!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Single indirect block file read/write checksum matches: $READ_SHA${NC}"

# Test 2: Double indirect (4 MB = 1024 blocks > 524 direct+single limit)
echo -e "\n${BLUE}--> [4] Testing Double Indirect Block Allocation (4 MB file)...${NC}"
dd if=/dev/urandom of=/tmp/expected_double.bin bs=1M count=4 status=none
cp /tmp/expected_double.bin "$MNT/double_indirect.bin"

DOUBLE_ORIG_SHA=$(sha256sum /tmp/expected_double.bin | awk '{print $1}')
DOUBLE_READ_SHA=$(sha256sum "$MNT/double_indirect.bin" | awk '{print $1}')

if [ "$DOUBLE_ORIG_SHA" != "$DOUBLE_READ_SHA" ]; then
    echo -e "${RED}[FAILED] Double indirect data corruption!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Double indirect block file (4 MB) read/write checksum matches: $DOUBLE_READ_SHA${NC}"

# Test 3: Unmount and Remount Persistence Check for Large Files
echo -e "\n${BLUE}--> [5] Unmounting & Remounting to verify large file persistence...${NC}"
unmount_and_wait

"$FSCK" "$IMG"

"$FUSEFS" "$IMG" "$MNT"
sleep 1

REMOUNT_DOUBLE_SHA=$(sha256sum "$MNT/double_indirect.bin" | awk '{print $1}')
if [ "$DOUBLE_ORIG_SHA" != "$REMOUNT_DOUBLE_SHA" ]; then
    echo -e "${RED}[FAILED] Checksum mismatch across remount for double indirect file!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] 4 MB Double-indirect file perfectly preserved across remount!${NC}"

# Test 4: Truncation & Complete Block Deallocation
echo -e "\n${BLUE}--> [6] Truncating 4 MB file to 100 KB and verifying no leaks...${NC}"
truncate -s 102400 "$MNT/double_indirect.bin"
TRUNC_SIZE=$(stat -c "%s" "$MNT/double_indirect.bin")
if [ "$TRUNC_SIZE" -ne 102400 ]; then
    echo -e "${RED}[FAILED] Truncate size mismatch: $TRUNC_SIZE${NC}"
    exit 1
fi

echo -e "\n${BLUE}--> [7] Removing large files and running final FSCK leak test...${NC}"
rm -f "$MNT/single_indirect.bin" "$MNT/double_indirect.bin"

unmount_and_wait

"$FSCK" "$IMG"

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN} LARGE-FILE & DOUBLE-INDIRECT TESTS PASSED (100% OK)  ${NC}"
echo -e "${GREEN}======================================================${NC}"
