#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "bitmap.h"
#include "storage.h"

int bitmap_alloc_inode(uint64_t *out_ino) {
    if (g_fs.sb.free_inodes == 0) {
        return -ENOSPC;
    }

    uint8_t buf[FUSEFS_BLOCK_SIZE];
    uint64_t total = g_fs.sb.total_inodes;

    for (uint64_t b = 0; b < g_fs.sb.inode_bitmap_blocks; b++) {
        uint64_t phys_block = g_fs.sb.inode_bitmap_start + b;
        if (storage_read_block(phys_block, buf) < 0) {
            return -EIO;
        }

        for (uint32_t byte_idx = 0; byte_idx < FUSEFS_BLOCK_SIZE; byte_idx++) {
            if (buf[byte_idx] == 0xFF) continue;

            for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
                uint64_t ino = (b * FUSEFS_BLOCK_SIZE + byte_idx) * 8 + bit_idx;
                if (ino >= total) {
                    return -ENOSPC;
                }
                if (ino == 0) continue; /* Inode 0 reserved */

                if (!(buf[byte_idx] & (1U << bit_idx))) {
                    buf[byte_idx] |= (1U << bit_idx);
                    if (storage_write_block(phys_block, buf) < 0) {
                        return -EIO;
                    }
                    g_fs.sb.free_inodes--;
                    storage_flush_sb();
                    *out_ino = ino;
                    return 0;
                }
            }
        }
    }
    return -ENOSPC;
}

int bitmap_free_inode(uint64_t ino) {
    if (ino == 0 || ino >= g_fs.sb.total_inodes) {
        return -EINVAL;
    }

    uint64_t bit_index = ino;
    uint64_t block_offset = bit_index / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t in_block_bit = bit_index % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = in_block_bit / 8;
    uint32_t bit_idx = in_block_bit % 8;

    uint64_t phys_block = g_fs.sb.inode_bitmap_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return -EIO;
    }

    if (buf[byte_idx] & (1U << bit_idx)) {
        buf[byte_idx] &= ~(1U << bit_idx);
        if (storage_write_block(phys_block, buf) < 0) {
            return -EIO;
        }
        g_fs.sb.free_inodes++;
        storage_flush_sb();
    }
    return 0;
}

bool bitmap_is_inode_allocated(uint64_t ino) {
    if (ino == 0 || ino >= g_fs.sb.total_inodes) {
        return false;
    }

    uint64_t bit_index = ino;
    uint64_t block_offset = bit_index / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t in_block_bit = bit_index % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = in_block_bit / 8;
    uint32_t bit_idx = in_block_bit % 8;

    uint64_t phys_block = g_fs.sb.inode_bitmap_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return false;
    }
    return (buf[byte_idx] & (1U << bit_idx)) != 0;
}

int bitmap_alloc_block(uint64_t *out_block) {
    if (g_fs.sb.free_blocks == 0) {
        return -ENOSPC;
    }

    uint8_t buf[FUSEFS_BLOCK_SIZE];
    uint64_t count = g_fs.sb.data_blocks_count;

    for (uint64_t b = 0; b < g_fs.sb.block_bitmap_blocks; b++) {
        uint64_t phys_block = g_fs.sb.block_bitmap_start + b;
        if (storage_read_block(phys_block, buf) < 0) {
            return -EIO;
        }

        for (uint32_t byte_idx = 0; byte_idx < FUSEFS_BLOCK_SIZE; byte_idx++) {
            if (buf[byte_idx] == 0xFF) continue;

            for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
                uint64_t rel_idx = (b * FUSEFS_BLOCK_SIZE + byte_idx) * 8 + bit_idx;
                if (rel_idx >= count) {
                    return -ENOSPC;
                }

                if (!(buf[byte_idx] & (1U << bit_idx))) {
                    buf[byte_idx] |= (1U << bit_idx);
                    if (storage_write_block(phys_block, buf) < 0) {
                        return -EIO;
                    }
                    g_fs.sb.free_blocks--;
                    storage_flush_sb();

                    uint64_t target_phys_block = g_fs.sb.data_blocks_start + rel_idx;
                    /* Zero out newly allocated block to prevent garbage */
                    uint8_t zero[FUSEFS_BLOCK_SIZE];
                    memset(zero, 0, sizeof(zero));
                    storage_write_block(target_phys_block, zero);

                    *out_block = target_phys_block;
                    return 0;
                }
            }
        }
    }
    return -ENOSPC;
}

int bitmap_free_block(uint64_t block_no) {
    if (block_no < g_fs.sb.data_blocks_start ||
        block_no >= g_fs.sb.data_blocks_start + g_fs.sb.data_blocks_count) {
        return -EINVAL;
    }

    uint64_t rel_idx = block_no - g_fs.sb.data_blocks_start;
    uint64_t block_offset = rel_idx / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t in_block_bit = rel_idx % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = in_block_bit / 8;
    uint32_t bit_idx = in_block_bit % 8;

    uint64_t phys_block = g_fs.sb.block_bitmap_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return -EIO;
    }

    if (buf[byte_idx] & (1U << bit_idx)) {
        buf[byte_idx] &= ~(1U << bit_idx);
        if (storage_write_block(phys_block, buf) < 0) {
            return -EIO;
        }
        g_fs.sb.free_blocks++;
        storage_flush_sb();
    }
    return 0;
}

bool bitmap_is_block_allocated(uint64_t block_no) {
    if (block_no < g_fs.sb.data_blocks_start ||
        block_no >= g_fs.sb.data_blocks_start + g_fs.sb.data_blocks_count) {
        return false;
    }

    uint64_t rel_idx = block_no - g_fs.sb.data_blocks_start;
    uint64_t block_offset = rel_idx / (FUSEFS_BLOCK_SIZE * 8);
    uint32_t in_block_bit = rel_idx % (FUSEFS_BLOCK_SIZE * 8);
    uint32_t byte_idx = in_block_bit / 8;
    uint32_t bit_idx = in_block_bit % 8;

    uint64_t phys_block = g_fs.sb.block_bitmap_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return false;
    }
    return (buf[byte_idx] & (1U << bit_idx)) != 0;
}
