#!/usr/bin/env bash
# ==============================================================================
# FuseFS Comprehensive Automated Test Battery
# Covers: Core operations, Persistence, Hard links, Symlinks, and FSCK leak check
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BIN_DIR="$ROOT_DIR/bin"
FUSEFS="$BIN_DIR/fusefs"
MKFS="$BIN_DIR/mkfs.fusefs"
FSCK="$BIN_DIR/fsck.fusefs"

IMG="$ROOT_DIR/test_fs.img"
MNT="$ROOT_DIR/mnt"

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
echo -e "${BLUE} FuseFS Automated Functional & Persistence Test Battery${NC}"
echo -e "${BLUE}======================================================${NC}"

# Check binaries
for b in "$FUSEFS" "$MKFS" "$FSCK"; do
    if [ ! -f "$b" ]; then
        echo -e "${RED}[ERROR] Binary $b not found. Run 'make' first.${NC}"
        exit 1
    fi
done

# Step 1: Format backing image
echo -e "\n${BLUE}--> [Step 1] Formatting 32 MB image...${NC}"
rm -f "$IMG"
"$MKFS" "$IMG" 32 1024

echo -e "\n${BLUE}--> [Step 2] Initial FSCK consistency check...${NC}"
"$FSCK" "$IMG"

# Step 3: Mount
mkdir -p "$MNT"
echo -e "\n${BLUE}--> [Step 3] Mounting FuseFS to $MNT...${NC}"
"$FUSEFS" "$IMG" "$MNT"
sleep 1

if ! mountpoint -q "$MNT"; then
    echo -e "${RED}[FAILED] Filesystem failed to mount!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Mounted successfully.${NC}"

# Step 4: Core operations
echo -e "\n${BLUE}--> [Step 4] Testing Basic File Creation & Content...${NC}"
echo "Hello, FuseFS Userspace World!" > "$MNT/hello.txt"
CONTENT=$(cat "$MNT/hello.txt")
if [ "$CONTENT" != "Hello, FuseFS Userspace World!" ]; then
    echo -e "${RED}[FAILED] File content mismatch! Read: $CONTENT${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] File create and read verified.${NC}"

echo -e "\n${BLUE}--> [Step 5] Testing Append (>>) Redirection...${NC}"
echo "Line 2 Appended" >> "$MNT/hello.txt"
LINE_COUNT=$(wc -l < "$MNT/hello.txt")
if [ "$LINE_COUNT" -ne 2 ]; then
    echo -e "${RED}[FAILED] Line count mismatch on append.${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Append redirection verified.${NC}"

echo -e "\n${BLUE}--> [Step 6] Testing Subdirectory Creation and Tree Walking...${NC}"
mkdir -p "$MNT/dir1/subdir2"
echo "Deep content" > "$MNT/dir1/subdir2/nested.txt"
if [ ! -f "$MNT/dir1/subdir2/nested.txt" ]; then
    echo -e "${RED}[FAILED] Nested file not found!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Subdirectory creation and nested file verified.${NC}"

echo -e "\n${BLUE}--> [Step 7] Testing Hard Links...${NC}"
ln "$MNT/hello.txt" "$MNT/hello_link.txt"
LINK_COUNT=$(stat -c "%h" "$MNT/hello.txt")
if [ "$LINK_COUNT" -ne 2 ]; then
    echo -e "${RED}[FAILED] Link count expected 2, got $LINK_COUNT${NC}"
    exit 1
fi
echo "Additional line via link" >> "$MNT/hello_link.txt"
ORIGINAL_CONTENT=$(cat "$MNT/hello.txt")
if [[ "$ORIGINAL_CONTENT" != *"Additional line via link"* ]]; then
    echo -e "${RED}[FAILED] Hard link content synchronization failed!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Hard links verified (nlink=$LINK_COUNT).${NC}"

echo -e "\n${BLUE}--> [Step 8] Testing Symbolic Links...${NC}"
ln -s "hello.txt" "$MNT/symlink_to_hello"
SYM_TARGET=$(readlink "$MNT/symlink_to_hello")
if [ "$SYM_TARGET" != "hello.txt" ]; then
    echo -e "${RED}[FAILED] Symlink target mismatch: $SYM_TARGET${NC}"
    exit 1
fi
SYM_READ=$(cat "$MNT/symlink_to_hello")
if [[ "$SYM_READ" != *"Hello, FuseFS Userspace World!"* ]]; then
    echo -e "${RED}[FAILED] Reading through symlink failed!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Symbolic links verified.${NC}"

echo -e "\n${BLUE}--> [Step 9] Testing Truncate...${NC}"
echo "1234567890" > "$MNT/trunc_test.txt"
truncate -s 5 "$MNT/trunc_test.txt"
TRUNC_SIZE=$(stat -c "%s" "$MNT/trunc_test.txt")
if [ "$TRUNC_SIZE" -ne 5 ]; then
    echo -e "${RED}[FAILED] Truncate size mismatch: $TRUNC_SIZE${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Truncate operation verified.${NC}"

# Prepare persistence canary data
echo -e "\n${BLUE}--> [Step 10] Writing Persistence Test Canaries...${NC}"
mkdir -p "$MNT/persist_dir"
echo "Persistence test block data 1" > "$MNT/persist_dir/file1.txt"
dd if=/dev/urandom of="$MNT/persist_dir/rand.bin" bs=4096 count=10 status=none
ORIG_MD5=$(md5sum "$MNT/persist_dir/rand.bin" | awk '{print $1}')
echo "Random canary MD5: $ORIG_MD5"

# Step 11: Unmount and Remount (The Persistence Test)
echo -e "\n${BLUE}--> [Step 11] Unmounting filesystem (Persistence checkpoint)...${NC}"
unmount_and_wait

echo -e "\n${BLUE}--> [Step 12] FSCK Check post-unmount...${NC}"
"$FSCK" "$IMG"

echo -e "\n${BLUE}--> [Step 13] Remounting filesystem from disk image...${NC}"
"$FUSEFS" "$IMG" "$MNT"
sleep 1

# Step 14: Verify persisted structures
echo -e "\n${BLUE}--> [Step 14] Validating Persisted Data Integrity...${NC}"
if [ ! -f "$MNT/persist_dir/file1.txt" ]; then
    echo -e "${RED}[FAILED] Persisted file missing after remount!${NC}"
    exit 1
fi

REMOUNT_MD5=$(md5sum "$MNT/persist_dir/rand.bin" | awk '{print $1}')
if [ "$ORIG_MD5" != "$REMOUNT_MD5" ]; then
    echo -e "${RED}[FAILED] Checksum mismatch across remount! $ORIG_MD5 != $REMOUNT_MD5${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] File contents perfectly preserved across unmount/remount!${NC}"

# Step 15: Deletion & Leak Verification
echo -e "\n${BLUE}--> [Step 15] Testing Unlink and Rmdir (Leak Check)...${NC}"
rm -rf "$MNT/persist_dir"
rm -f "$MNT/hello_link.txt"
rm -f "$MNT/symlink_to_hello"
rm -f "$MNT/trunc_test.txt"
rm -rf "$MNT/dir1"
rm -f "$MNT/hello.txt"

unmount_and_wait

echo -e "\n${BLUE}--> [Step 16] Final FSCK Check (Validating zero leaks after deletion)...${NC}"
"$FSCK" "$IMG"

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN} ALL FUNCTIONAL & PERSISTENCE TESTS PASSED (100% OK) ${NC}"
echo -e "${GREEN}======================================================${NC}"
