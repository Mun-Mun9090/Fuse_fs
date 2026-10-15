#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include "fusefs.h"

struct fsck_context {
    int fd;
    const char *img_path;
    bool repair;
    int error_count;
    int warning_count;
    struct fusefs_superblock sb;
    uint32_t *block_refs;
    uint32_t *inode_refs;
    bool *inode_reachable;
};

static int read_block(struct fsck_context *ctx, uint64_t blk, void *buf) {
    off_t off = (off_t)blk * FUSEFS_BLOCK_SIZE;
    if (pread(ctx->fd, buf, FUSEFS_BLOCK_SIZE, off) != (ssize_t)FUSEFS_BLOCK_SIZE) {
        return -EIO;
    }
    return 0;
}

static int write_block(struct fsck_context *ctx, uint64_t blk, const void *buf) {
    off_t off = (off_t)blk * FUSEFS_BLOCK_SIZE;
    if (pwrite(ctx->fd, buf, FUSEFS_BLOCK_SIZE, off) != (ssize_t)FUSEFS_BLOCK_SIZE) {
        return -EIO;
    }
    return 0;
}

static int read_inode(struct fsck_context *ctx, uint64_t ino, struct fusefs_inode *in) {
    off_t off = (ctx->sb.inode_table_start * FUSEFS_BLOCK_SIZE) + (ino * FUSEFS_INODE_SIZE);
    if (pread(ctx->fd, in, FUSEFS_INODE_SIZE, off) != (ssize_t)FUSEFS_INODE_SIZE) {
        return -EIO;
    }
    return 0;
}

static int write_inode(struct fsck_context *ctx, uint64_t ino, const struct fusefs_inode *in) {
    off_t off = (ctx->sb.inode_table_start * FUSEFS_BLOCK_SIZE) + (ino * FUSEFS_INODE_SIZE);
    if (pwrite(ctx->fd, in, FUSEFS_INODE_SIZE, off) != (ssize_t)FUSEFS_INODE_SIZE) {
        return -EIO;
    }
    return 0;
}

static bool is_inode_allocated_disk(struct fsck_context *ctx, uint64_t ino) {
    uint64_t bit_idx = ino;
    uint64_t blk_off = bit_idx / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t bit_rem = bit_idx % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = bit_rem / 8;
    uint32_t bit = bit_rem % 8;

    uint8_t buf[FUSEFS_BLOCK_SIZE];
    read_block(ctx, ctx->sb.inode_bitmap_start + blk_off, buf);
    return (buf[byte_idx] & (1U << bit)) != 0;
}

static bool is_block_allocated_disk(struct fsck_context *ctx, uint64_t phys_blk) {
    if (phys_blk < ctx->sb.data_blocks_start ||
        phys_blk >= ctx->sb.data_blocks_start + ctx->sb.data_blocks_count) {
        return false;
    }
    uint64_t rel_idx = phys_blk - ctx->sb.data_blocks_start;
    uint64_t blk_off = rel_idx / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t bit_rem = rel_idx % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = bit_rem / 8;
    uint32_t bit = bit_rem % 8;

    uint8_t buf[FUSEFS_BLOCK_SIZE];
    read_block(ctx, ctx->sb.block_bitmap_start + blk_off, buf);
    return (buf[byte_idx] & (1U << bit)) != 0;
}

static void set_block_allocated_disk(struct fsck_context *ctx, uint64_t phys_blk, bool val) {
    uint64_t rel_idx = phys_blk - ctx->sb.data_blocks_start;
    uint64_t blk_off = rel_idx / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t bit_rem = rel_idx % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = bit_rem / 8;
    uint32_t bit = bit_rem % 8;

    uint8_t buf[FUSEFS_BLOCK_SIZE];
    read_block(ctx, ctx->sb.block_bitmap_start + blk_off, buf);
    if (val) {
        buf[byte_idx] |= (1U << bit);
    } else {
        buf[byte_idx] &= ~(1U << bit);
    }
    write_block(ctx, ctx->sb.block_bitmap_start + blk_off, buf);
}

static void check_data_block(struct fsck_context *ctx, uint64_t blk, const char *desc, uint64_t ino) {
    if (blk == 0) return;
    if (blk < ctx->sb.data_blocks_start || blk >= ctx->sb.total_blocks) {
        printf("[ERROR] Inode %lu references out-of-range block %lu (%s)\n",
               (unsigned long)ino, (unsigned long)blk, desc);
        ctx->error_count++;
        return;
    }
    if (ctx->block_refs[blk] > 0) {
        printf("[ERROR] Duplicate block reference: Block %lu already used, also referenced by inode %lu (%s)\n",
               (unsigned long)blk, (unsigned long)ino, desc);
        ctx->error_count++;
    }
    ctx->block_refs[blk]++;
}

/* Recursive directory walker */
static void scan_directory(struct fsck_context *ctx, uint64_t dir_ino, uint64_t parent_ino) {
    ctx->inode_reachable[dir_ino] = true;

    struct fusefs_inode in;
    if (read_inode(ctx, dir_ino, &in) < 0) return;

    uint64_t num_blocks = (in.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < num_blocks && b < FUSEFS_DIRECT_BLOCKS; b++) {
        uint64_t phys_blk = in.direct[b];
        if (phys_blk == 0) continue;
        if (read_block(ctx, phys_blk, blk_buf) < 0) continue;

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode == 0) continue;

            uint64_t child_ino = entries[i].inode;
            if (child_ino >= ctx->sb.total_inodes) {
                printf("[ERROR] Directory inode %lu contains invalid inode reference %lu ('%s')\n",
                       (unsigned long)dir_ino, (unsigned long)child_ino, entries[i].name);
                ctx->error_count++;
                continue;
            }

            ctx->inode_refs[child_ino]++;

            if (strcmp(entries[i].name, ".") == 0) {
                if (child_ino != dir_ino) {
                    printf("[ERROR] Inode %lu: '.' points to %lu (expected %lu)\n",
                           (unsigned long)dir_ino, (unsigned long)child_ino, (unsigned long)dir_ino);
                    ctx->error_count++;
                }
            } else if (strcmp(entries[i].name, "..") == 0) {
                if (child_ino != parent_ino) {
                    printf("[ERROR] Inode %lu: '..' points to %lu (expected %lu)\n",
                           (unsigned long)dir_ino, (unsigned long)child_ino, (unsigned long)parent_ino);
                    ctx->error_count++;
                }
            } else {
                /* Regular entry or subdirectory */
                struct fusefs_inode child_in;
                if (read_inode(ctx, child_ino, &child_in) == 0) {
                    if (S_ISDIR(child_in.mode)) {
                        if (!ctx->inode_reachable[child_ino]) {
                            scan_directory(ctx, child_ino, dir_ino);
                        } else {
                            printf("[ERROR] Directory cycle detected at inode %lu\n", (unsigned long)child_ino);
                            ctx->error_count++;
                        }
                    } else {
                        ctx->inode_reachable[child_ino] = true;
                    }
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [--repair] <image_file>\n", argv[0]);
        return 1;
    }

    struct fsck_context ctx;
    memset(&ctx, 0, sizeof(ctx));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0 || strcmp(argv[i], "-r") == 0) {
            ctx.repair = true;
        } else {
            ctx.img_path = argv[i];
        }
    }

    if (!ctx.img_path) {
        fprintf(stderr, "Error: Missing image file.\n");
        return 1;
    }

    ctx.fd = open(ctx.img_path, ctx.repair ? O_RDWR : O_RDONLY);
    if (ctx.fd < 0) {
        perror("open image failed");
        return 1;
    }

    printf("=====================================================\n");
    printf(" FuseFS Consistency Checker (fsck.fusefs)\n");
    printf(" Target Image: %s (Mode: %s)\n", ctx.img_path, ctx.repair ? "CHECK & REPAIR" : "CHECK ONLY");
    printf("=====================================================\n");

    /* Pass 1: Superblock Sanity */
    printf("[Pass 1] Checking superblock...\n");
    if (pread(ctx.fd, &ctx.sb, sizeof(ctx.sb), 0) != sizeof(ctx.sb)) {
        fprintf(stderr, "[FATAL] Failed to read superblock!\n");
        close(ctx.fd);
        return 1;
    }

    if (ctx.sb.magic != FUSEFS_MAGIC) {
        printf("[FATAL] Bad magic number: 0x%lx (expected 0x%lx)\n",
               (unsigned long)ctx.sb.magic, (unsigned long)FUSEFS_MAGIC);
        close(ctx.fd);
        return 1;
    }

    if (ctx.sb.block_size != FUSEFS_BLOCK_SIZE) {
        printf("[FATAL] Invalid block size: %u\n", ctx.sb.block_size);
        close(ctx.fd);
        return 1;
    }

    if (ctx.sb.state == FUSEFS_STATE_DIRTY) {
        printf("[WARN] Filesystem was marked DIRTY (unclean unmount or active mount)\n");
        ctx.warning_count++;
    }

    ctx.block_refs = calloc(ctx.sb.total_blocks, sizeof(uint32_t));
    ctx.inode_refs = calloc(ctx.sb.total_inodes, sizeof(uint32_t));
    ctx.inode_reachable = calloc(ctx.sb.total_inodes, sizeof(bool));

    /* Pass 2: Inode Scan & Block Reference Collection */
    printf("[Pass 2] Scanning inode table and block mapping tree...\n");
    for (uint64_t ino = 1; ino < ctx.sb.total_inodes; ino++) {
        if (!is_inode_allocated_disk(&ctx, ino)) {
            continue;
        }

        struct fusefs_inode in;
        if (read_inode(&ctx, ino, &in) < 0) {
            printf("[ERROR] Failed to read allocated inode %lu\n", (unsigned long)ino);
            ctx.error_count++;
            continue;
        }

        /* Direct blocks */
        for (int i = 0; i < FUSEFS_DIRECT_BLOCKS; i++) {
            check_data_block(&ctx, in.direct[i], "direct block", ino);
        }

        /* Single indirect */
        if (in.single_indirect != 0) {
            check_data_block(&ctx, in.single_indirect, "single indirect ptr block", ino);
            uint64_t ptrs[FUSEFS_PTRS_PER_BLOCK];
            if (read_block(&ctx, in.single_indirect, ptrs) == 0) {
                for (size_t p = 0; p < FUSEFS_PTRS_PER_BLOCK; p++) {
                    check_data_block(&ctx, ptrs[p], "single indirect data block", ino);
                }
            }
        }

        /* Double indirect */
        if (in.double_indirect != 0) {
            check_data_block(&ctx, in.double_indirect, "double indirect l1 block", ino);
            uint64_t l1[FUSEFS_PTRS_PER_BLOCK];
            if (read_block(&ctx, in.double_indirect, l1) == 0) {
                for (size_t i = 0; i < FUSEFS_PTRS_PER_BLOCK; i++) {
                    if (l1[i] != 0) {
                        check_data_block(&ctx, l1[i], "double indirect l2 block", ino);
                        uint64_t l2[FUSEFS_PTRS_PER_BLOCK];
                        if (read_block(&ctx, l1[i], l2) == 0) {
                            for (size_t j = 0; j < FUSEFS_PTRS_PER_BLOCK; j++) {
                                check_data_block(&ctx, l2[j], "double indirect data block", ino);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Pass 3: Directory Tree Traversal & Link Verification */
    printf("[Pass 3] Traversing directory hierarchy and validating link counts...\n");
    scan_directory(&ctx, FUSEFS_ROOT_INO, FUSEFS_ROOT_INO);

    for (uint64_t ino = 1; ino < ctx.sb.total_inodes; ino++) {
        if (is_inode_allocated_disk(&ctx, ino)) {
            if (!ctx.inode_reachable[ino]) {
                printf("[ERROR] Orphaned inode %lu: allocated in bitmap but unreachable from root!\n", (unsigned long)ino);
                ctx.error_count++;
            } else {
                struct fusefs_inode in;
                if (read_inode(&ctx, ino, &in) == 0) {
                    if (in.nlink != ctx.inode_refs[ino]) {
                        printf("[ERROR] Inode %lu link count mismatch: on-disk nlink=%u, actual refs=%u\n",
                               (unsigned long)ino, in.nlink, ctx.inode_refs[ino]);
                        ctx.error_count++;
                        if (ctx.repair) {
                            in.nlink = ctx.inode_refs[ino];
                            write_inode(&ctx, ino, &in);
                            printf("  -> [REPAIR] Fixed nlink for inode %lu to %u\n", (unsigned long)ino, in.nlink);
                        }
                    }
                }
            }
        }
    }

    /* Pass 4: Bitmap Consistency Verification */
    printf("[Pass 4] Cross-checking block bitmap against referenced blocks...\n");
    uint64_t leaked_blocks = 0;
    uint64_t phantom_blocks = 0;
    uint64_t actual_free_blocks = 0;

    for (uint64_t blk = ctx.sb.data_blocks_start; blk < ctx.sb.total_blocks; blk++) {
        bool bit_set = is_block_allocated_disk(&ctx, blk);
        uint32_t refs = ctx.block_refs[blk];

        if (bit_set && refs == 0) {
            printf("[ERROR] Block leak detected: Block %lu is marked allocated in bitmap but unreferenced by any inode!\n",
                   (unsigned long)blk);
            ctx.error_count++;
            leaked_blocks++;
            if (ctx.repair) {
                set_block_allocated_disk(&ctx, blk, false);
                printf("  -> [REPAIR] Cleared bitmap bit for leaked block %lu\n", (unsigned long)blk);
            }
        } else if (!bit_set && refs > 0) {
            printf("[ERROR] Phantom block detected: Block %lu is referenced by %u inode(s) but marked FREE in bitmap!\n",
                   (unsigned long)blk, refs);
            ctx.error_count++;
            phantom_blocks++;
            if (ctx.repair) {
                set_block_allocated_disk(&ctx, blk, true);
                printf("  -> [REPAIR] Set bitmap bit for referenced block %lu\n", (unsigned long)blk);
            }
        }

        if (refs == 0 && (!bit_set || (ctx.repair && leaked_blocks > 0))) {
            actual_free_blocks++;
        }
    }

    if (ctx.repair && (ctx.error_count > 0 || ctx.sb.state == FUSEFS_STATE_DIRTY)) {
        ctx.sb.state = FUSEFS_STATE_CLEAN;
        ctx.sb.free_blocks = actual_free_blocks;
        if (pwrite(ctx.fd, &ctx.sb, sizeof(ctx.sb), 0) != sizeof(ctx.sb)) {
            perror("repaired superblock write failed");
        }
        fsync(ctx.fd);
        printf("[REPAIR] Superblock updated and marked CLEAN.\n");
    }

    printf("=====================================================\n");
    printf(" FSCK SUMMARY\n");
    printf(" Total Errors Found   : %d\n", ctx.error_count);
    printf(" Total Warnings Found : %d\n", ctx.warning_count);
    printf(" Leaked Blocks        : %lu\n", (unsigned long)leaked_blocks);
    printf(" Phantom Blocks       : %lu\n", (unsigned long)phantom_blocks);

    int exit_code = 0;
    if (ctx.error_count == 0) {
        printf(" STATUS: [CLEAN] All filesystem structures are 100%% sound!\n");
        exit_code = 0;
    } else if (ctx.repair) {
        printf(" STATUS: [REPAIRED] Discrepancies have been corrected.\n");
        exit_code = 0;
    } else {
        printf(" STATUS: [ERRORS DETECTED] Run with --repair to fix.\n");
        exit_code = 1;
    }
    printf("=====================================================\n");

    free(ctx.block_refs);
    free(ctx.inode_refs);
    free(ctx.inode_reachable);
    close(ctx.fd);
    return exit_code;
}
