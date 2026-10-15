#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "fusefs.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image_file> [size_in_mb] [total_inodes]\n", argv[0]);
        fprintf(stderr, "Example: %s fs.img 64 2048\n", argv[0]);
        return 1;
    }

    const char *img_path = argv[1];
    uint64_t size_mb = 64; /* Default 64 MB */
    uint64_t inode_count = 2048; /* Default 2048 inodes */

    if (argc >= 3) {
        size_mb = strtoull(argv[2], NULL, 10);
        if (size_mb < 4) size_mb = 4;
    }
    if (argc >= 4) {
        inode_count = strtoull(argv[3], NULL, 10);
        if (inode_count < 64) inode_count = 64;
    }

    uint64_t total_bytes = size_mb * 1024ULL * 1024ULL;
    uint64_t total_blocks = total_bytes / FUSEFS_BLOCK_SIZE;

    int fd = open(img_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("Failed to create image file");
        return 1;
    }

    if (ftruncate(fd, total_bytes) != 0) {
        perror("ftruncate failed");
        close(fd);
        return 1;
    }

    /* Layout calculations */
    uint64_t sb_blocks = 1;
    uint64_t inode_bitmap_blocks = (inode_count + (FUSEFS_BLOCK_SIZE * 8) - 1) / (FUSEFS_BLOCK_SIZE * 8);
    uint64_t inode_table_blocks = (inode_count * FUSEFS_INODE_SIZE + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint64_t block_bitmap_blocks = (total_blocks + (FUSEFS_BLOCK_SIZE * 8) - 1) / (FUSEFS_BLOCK_SIZE * 8);

    uint64_t metadata_blocks = sb_blocks + inode_bitmap_blocks + block_bitmap_blocks + inode_table_blocks;
    if (metadata_blocks >= total_blocks) {
        fprintf(stderr, "Error: Image size is too small for metadata structures.\n");
        close(fd);
        return 1;
    }

    uint64_t data_blocks_start = metadata_blocks;
    uint64_t data_blocks_count = total_blocks - data_blocks_start;

    /* Populate Superblock */
    struct fusefs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FUSEFS_MAGIC;
    sb.block_size = FUSEFS_BLOCK_SIZE;
    sb.inode_size = FUSEFS_INODE_SIZE;
    sb.total_blocks = total_blocks;
    sb.total_inodes = inode_count;
    sb.free_blocks = data_blocks_count - 1; /* 1 data block used for root directory */
    sb.free_inodes = inode_count - 2;      /* Inode 0 reserved, Inode 1 root dir */

    sb.inode_bitmap_start = sb_blocks;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;

    sb.block_bitmap_start = sb.inode_bitmap_start + inode_bitmap_blocks;
    sb.block_bitmap_blocks = block_bitmap_blocks;

    sb.inode_table_start = sb.block_bitmap_start + block_bitmap_blocks;
    sb.inode_table_blocks = inode_table_blocks;

    sb.data_blocks_start = data_blocks_start;
    sb.data_blocks_count = data_blocks_count;

    sb.root_inode = FUSEFS_ROOT_INO;
    sb.state = FUSEFS_STATE_CLEAN;
    sb.mount_count = 0;

    /* Write Superblock */
    if (pwrite(fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Failed to write superblock");
        close(fd);
        return 1;
    }

    /* Zero all metadata blocks on disk */
    uint8_t zero_block[FUSEFS_BLOCK_SIZE];
    memset(zero_block, 0, FUSEFS_BLOCK_SIZE);
    for (uint64_t i = 1; i < data_blocks_start; i++) {
        if (pwrite(fd, zero_block, FUSEFS_BLOCK_SIZE, i * FUSEFS_BLOCK_SIZE) != FUSEFS_BLOCK_SIZE) {
            perror("zero block write failed");
        }
    }

    /* Setup Inode Bitmap: Inode 0 (reserved) and Inode 1 (root) allocated */
    uint8_t ib_buf[FUSEFS_BLOCK_SIZE];
    memset(ib_buf, 0, FUSEFS_BLOCK_SIZE);
    ib_buf[0] = 0x03; /* bits 0 and 1 set: 00000011 */
    if (pwrite(fd, ib_buf, FUSEFS_BLOCK_SIZE, sb.inode_bitmap_start * FUSEFS_BLOCK_SIZE) != FUSEFS_BLOCK_SIZE) {
        perror("inode bitmap write failed");
    }

    /* Setup Block Bitmap: Data Block 0 allocated for root directory */
    uint8_t bb_buf[FUSEFS_BLOCK_SIZE];
    memset(bb_buf, 0, FUSEFS_BLOCK_SIZE);
    bb_buf[0] = 0x01; /* bit 0 set */
    if (pwrite(fd, bb_buf, FUSEFS_BLOCK_SIZE, sb.block_bitmap_start * FUSEFS_BLOCK_SIZE) != FUSEFS_BLOCK_SIZE) {
        perror("block bitmap write failed");
    }

    /* Setup Root Inode (Inode 1) */
    struct fusefs_inode root_in;
    memset(&root_in, 0, sizeof(root_in));
    root_in.mode = S_IFDIR | 0755;
    root_in.nlink = 2; /* '.' and '..' */
    root_in.uid = getuid();
    root_in.gid = getgid();
    root_in.size = 2 * sizeof(struct fusefs_dirent);
    time_t now = time(NULL);
    root_in.atime = now;
    root_in.mtime = now;
    root_in.ctime = now;
    root_in.direct[0] = data_blocks_start; /* Data block 0 */

    off_t root_inode_offset = (sb.inode_table_start * FUSEFS_BLOCK_SIZE) + (FUSEFS_ROOT_INO * FUSEFS_INODE_SIZE);
    if (pwrite(fd, &root_in, sizeof(root_in), root_inode_offset) != sizeof(root_in)) {
        perror("Failed to write root inode");
        close(fd);
        return 1;
    }

    /* Initialize Root Directory Block */
    uint8_t root_block[FUSEFS_BLOCK_SIZE];
    memset(root_block, 0, FUSEFS_BLOCK_SIZE);
    struct fusefs_dirent *entries = (struct fusefs_dirent *)root_block;

    /* Entry 0: "." */
    entries[0].inode = FUSEFS_ROOT_INO;
    entries[0].file_type = DT_DIR;
    entries[0].name_len = 1;
    strncpy(entries[0].name, ".", sizeof(entries[0].name));

    /* Entry 1: ".." */
    entries[1].inode = FUSEFS_ROOT_INO;
    entries[1].file_type = DT_DIR;
    entries[1].name_len = 2;
    strncpy(entries[1].name, "..", sizeof(entries[1].name));

    off_t root_data_offset = data_blocks_start * FUSEFS_BLOCK_SIZE;
    if (pwrite(fd, root_block, FUSEFS_BLOCK_SIZE, root_data_offset) != FUSEFS_BLOCK_SIZE) {
        perror("Failed to write root directory block");
        close(fd);
        return 1;
    }

    fsync(fd);
    close(fd);

    printf("===================================================\n");
    printf(" FuseFS Filesystem Formatted Successfully!\n");
    printf("===================================================\n");
    printf("Image File          : %s\n", img_path);
    printf("Total Size          : %lu MB (%lu blocks)\n", (unsigned long)size_mb, (unsigned long)total_blocks);
    printf("Block Size          : %u bytes\n", FUSEFS_BLOCK_SIZE);
    printf("Total Inodes        : %lu\n", (unsigned long)inode_count);
    printf("Free Inodes         : %lu\n", (unsigned long)sb.free_inodes);
    printf("Free Data Blocks    : %lu\n", (unsigned long)sb.free_blocks);
    printf("Superblock Block    : 0\n");
    printf("Inode Bitmap Blocks : %lu .. %lu\n", (unsigned long)sb.inode_bitmap_start, (unsigned long)(sb.inode_bitmap_start + inode_bitmap_blocks - 1));
    printf("Block Bitmap Blocks : %lu .. %lu\n", (unsigned long)sb.block_bitmap_start, (unsigned long)(sb.block_bitmap_start + block_bitmap_blocks - 1));
    printf("Inode Table Blocks  : %lu .. %lu\n", (unsigned long)sb.inode_table_start, (unsigned long)(sb.inode_table_start + inode_table_blocks - 1));
    printf("Data Blocks Region  : %lu .. %lu\n", (unsigned long)data_blocks_start, (unsigned long)(total_blocks - 1));
    printf("===================================================\n");

    return 0;
}
