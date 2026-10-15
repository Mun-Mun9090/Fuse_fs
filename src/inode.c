#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "inode.h"
#include "bitmap.h"
#include "storage.h"

int inode_read(uint64_t ino, struct fusefs_inode *out_inode) {
    if (ino == 0 || ino >= g_fs.sb.total_inodes) {
        return -EINVAL;
    }

    uint64_t byte_pos = ino * FUSEFS_INODE_SIZE;
    uint64_t block_offset = byte_pos / FUSEFS_BLOCK_SIZE;
    uint32_t in_block_offset = byte_pos % FUSEFS_BLOCK_SIZE;

    uint64_t phys_block = g_fs.sb.inode_table_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return -EIO;
    }

    memcpy(out_inode, buf + in_block_offset, FUSEFS_INODE_SIZE);
    return 0;
}

int inode_write(uint64_t ino, const struct fusefs_inode *inode) {
    if (ino == 0 || ino >= g_fs.sb.total_inodes) {
        return -EINVAL;
    }

    uint64_t byte_pos = ino * FUSEFS_INODE_SIZE;
    uint64_t block_offset = byte_pos / FUSEFS_BLOCK_SIZE;
    uint32_t in_block_offset = byte_pos % FUSEFS_BLOCK_SIZE;

    uint64_t phys_block = g_fs.sb.inode_table_start + block_offset;
    uint8_t buf[FUSEFS_BLOCK_SIZE];
    if (storage_read_block(phys_block, buf) < 0) {
        return -EIO;
    }

    memcpy(buf + in_block_offset, inode, FUSEFS_INODE_SIZE);
    if (storage_write_block(phys_block, buf) < 0) {
        return -EIO;
    }
    return 0;
}

int inode_create(uint16_t mode, uint32_t uid, uint32_t gid, uint64_t *out_ino) {
    uint64_t ino = 0;
    int ret = bitmap_alloc_inode(&ino);
    if (ret < 0) return ret;

    struct fusefs_inode in;
    memset(&in, 0, sizeof(in));
    in.mode = mode;
    in.nlink = S_ISDIR(mode) ? 2 : 1;
    in.uid = uid;
    in.gid = gid;
    in.size = 0;
    time_t now = time(NULL);
    in.atime = now;
    in.mtime = now;
    in.ctime = now;

    ret = inode_write(ino, &in);
    if (ret < 0) {
        bitmap_free_inode(ino);
        return ret;
    }

    *out_ino = ino;
    return 0;
}

int inode_bmap(uint64_t ino, struct fusefs_inode *inode, uint64_t file_block_idx, bool allocate, uint64_t *out_phys_block) {
    *out_phys_block = 0;

    /* 1. Direct blocks (0 .. 11) */
    if (file_block_idx < FUSEFS_DIRECT_BLOCKS) {
        if (inode->direct[file_block_idx] == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            inode->direct[file_block_idx] = blk;
            inode_write(ino, inode);
        }
        *out_phys_block = inode->direct[file_block_idx];
        return 0;
    }

    /* 2. Single indirect blocks (12 .. 12 + 511 = 523) */
    uint64_t single_idx = file_block_idx - FUSEFS_DIRECT_BLOCKS;
    if (single_idx < FUSEFS_PTRS_PER_BLOCK) {
        if (inode->single_indirect == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            inode->single_indirect = blk;
            inode_write(ino, inode);
        }

        uint64_t ptrs[FUSEFS_PTRS_PER_BLOCK];
        if (storage_read_block(inode->single_indirect, ptrs) < 0) return -EIO;

        if (ptrs[single_idx] == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            ptrs[single_idx] = blk;
            if (storage_write_block(inode->single_indirect, ptrs) < 0) return -EIO;
        }

        *out_phys_block = ptrs[single_idx];
        return 0;
    }

    /* 3. Double indirect blocks (524 .. 524 + 512*512 - 1 = 262667) */
    uint64_t double_idx = single_idx - FUSEFS_PTRS_PER_BLOCK;
    uint64_t max_double_blocks = FUSEFS_PTRS_PER_BLOCK * FUSEFS_PTRS_PER_BLOCK;
    if (double_idx < max_double_blocks) {
        uint64_t l1_idx = double_idx / FUSEFS_PTRS_PER_BLOCK;
        uint64_t l2_idx = double_idx % FUSEFS_PTRS_PER_BLOCK;

        if (inode->double_indirect == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            inode->double_indirect = blk;
            inode_write(ino, inode);
        }

        uint64_t l1_ptrs[FUSEFS_PTRS_PER_BLOCK];
        if (storage_read_block(inode->double_indirect, l1_ptrs) < 0) return -EIO;

        if (l1_ptrs[l1_idx] == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            l1_ptrs[l1_idx] = blk;
            if (storage_write_block(inode->double_indirect, l1_ptrs) < 0) return -EIO;
        }

        uint64_t l2_ptrs[FUSEFS_PTRS_PER_BLOCK];
        if (storage_read_block(l1_ptrs[l1_idx], l2_ptrs) < 0) return -EIO;

        if (l2_ptrs[l2_idx] == 0) {
            if (!allocate) return 0;
            uint64_t blk = 0;
            int ret = bitmap_alloc_block(&blk);
            if (ret < 0) return ret;
            l2_ptrs[l2_idx] = blk;
            if (storage_write_block(l1_ptrs[l1_idx], l2_ptrs) < 0) return -EIO;
        }

        *out_phys_block = l2_ptrs[l2_idx];
        return 0;
    }

    return -EFBIG;
}

int inode_truncate(uint64_t ino, uint64_t new_size) {
    struct fusefs_inode inode;
    int ret = inode_read(ino, &inode);
    if (ret < 0) return ret;

    uint64_t old_blocks = (inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint64_t new_blocks = (new_size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;

    if (new_blocks < old_blocks) {
        /* Free direct blocks */
        for (uint64_t i = new_blocks; i < old_blocks && i < FUSEFS_DIRECT_BLOCKS; i++) {
            if (inode.direct[i] != 0) {
                bitmap_free_block(inode.direct[i]);
                inode.direct[i] = 0;
            }
        }

        /* Free single indirect blocks */
        if (inode.single_indirect != 0) {
            uint64_t ptrs[FUSEFS_PTRS_PER_BLOCK];
            if (storage_read_block(inode.single_indirect, ptrs) == 0) {
                bool modified = false;
                for (uint64_t p = 0; p < FUSEFS_PTRS_PER_BLOCK; p++) {
                    uint64_t f_blk = FUSEFS_DIRECT_BLOCKS + p;
                    if (f_blk >= new_blocks && f_blk < old_blocks) {
                        if (ptrs[p] != 0) {
                            bitmap_free_block(ptrs[p]);
                            ptrs[p] = 0;
                            modified = true;
                        }
                    }
                }
                if (new_blocks <= FUSEFS_DIRECT_BLOCKS) {
                    bitmap_free_block(inode.single_indirect);
                    inode.single_indirect = 0;
                } else if (modified) {
                    storage_write_block(inode.single_indirect, ptrs);
                }
            }
        }

        /* Free double indirect blocks */
        if (inode.double_indirect != 0) {
            uint64_t l1_ptrs[FUSEFS_PTRS_PER_BLOCK];
            if (storage_read_block(inode.double_indirect, l1_ptrs) == 0) {
                bool l1_modified = false;
                uint64_t base_fblk = FUSEFS_DIRECT_BLOCKS + FUSEFS_PTRS_PER_BLOCK;

                for (uint64_t l1 = 0; l1 < FUSEFS_PTRS_PER_BLOCK; l1++) {
                    if (l1_ptrs[l1] != 0) {
                        uint64_t l2_ptrs[FUSEFS_PTRS_PER_BLOCK];
                        if (storage_read_block(l1_ptrs[l1], l2_ptrs) == 0) {
                            bool l2_modified = false;
                            for (uint64_t l2 = 0; l2 < FUSEFS_PTRS_PER_BLOCK; l2++) {
                                uint64_t f_blk = base_fblk + l1 * FUSEFS_PTRS_PER_BLOCK + l2;
                                if (f_blk >= new_blocks && f_blk < old_blocks) {
                                    if (l2_ptrs[l2] != 0) {
                                        bitmap_free_block(l2_ptrs[l2]);
                                        l2_ptrs[l2] = 0;
                                        l2_modified = true;
                                    }
                                }
                            }

                            uint64_t l1_first_fblk = base_fblk + l1 * FUSEFS_PTRS_PER_BLOCK;
                            if (new_blocks <= l1_first_fblk) {
                                bitmap_free_block(l1_ptrs[l1]);
                                l1_ptrs[l1] = 0;
                                l1_modified = true;
                            } else if (l2_modified) {
                                storage_write_block(l1_ptrs[l1], l2_ptrs);
                            }
                        }
                    }
                }

                if (new_blocks <= base_fblk) {
                    bitmap_free_block(inode.double_indirect);
                    inode.double_indirect = 0;
                } else if (l1_modified) {
                    storage_write_block(inode.double_indirect, l1_ptrs);
                }
            }
        }
    }

    inode.size = new_size;
    time_t now = time(NULL);
    inode.mtime = now;
    inode.ctime = now;
    return inode_write(ino, &inode);
}

int inode_free(uint64_t ino) {
    if (ino == 0 || ino >= g_fs.sb.total_inodes) {
        return -EINVAL;
    }
    inode_truncate(ino, 0);
    return bitmap_free_inode(ino);
}

int inode_read_data(uint64_t ino, void *buf, size_t size, off_t offset, size_t *bytes_read) {
    *bytes_read = 0;
    if (size == 0) return 0;

    struct fusefs_inode inode;
    int ret = inode_read(ino, &inode);
    if (ret < 0) return ret;

    if (offset >= (off_t)inode.size) {
        return 0;
    }

    if (offset + size > inode.size) {
        size = inode.size - offset;
    }

    size_t total = 0;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    while (total < size) {
        uint64_t curr_off = offset + total;
        uint64_t file_block_idx = curr_off / FUSEFS_BLOCK_SIZE;
        uint32_t in_blk_off = curr_off % FUSEFS_BLOCK_SIZE;
        size_t to_read = FUSEFS_BLOCK_SIZE - in_blk_off;
        if (to_read > size - total) {
            to_read = size - total;
        }

        uint64_t phys_block = 0;
        ret = inode_bmap(ino, &inode, file_block_idx, false, &phys_block);
        if (ret < 0) return ret;

        if (phys_block == 0) {
            /* Sparse hole */
            memset((uint8_t *)buf + total, 0, to_read);
        } else {
            if (storage_read_block(phys_block, blk_buf) < 0) {
                return -EIO;
            }
            memcpy((uint8_t *)buf + total, blk_buf + in_blk_off, to_read);
        }

        total += to_read;
    }

    *bytes_read = total;
    return 0;
}

int inode_write_data(uint64_t ino, const void *buf, size_t size, off_t offset, size_t *bytes_written) {
    *bytes_written = 0;
    if (size == 0) return 0;

    struct fusefs_inode inode;
    int ret = inode_read(ino, &inode);
    if (ret < 0) return ret;

    size_t total = 0;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    while (total < size) {
        uint64_t curr_off = offset + total;
        uint64_t file_block_idx = curr_off / FUSEFS_BLOCK_SIZE;
        uint32_t in_blk_off = curr_off % FUSEFS_BLOCK_SIZE;
        size_t to_write = FUSEFS_BLOCK_SIZE - in_blk_off;
        if (to_write > size - total) {
            to_write = size - total;
        }

        uint64_t phys_block = 0;
        ret = inode_bmap(ino, &inode, file_block_idx, true, &phys_block);
        if (ret < 0) return ret;

        if (in_blk_off == 0 && to_write == FUSEFS_BLOCK_SIZE) {
            /* Full block write */
            if (storage_write_block(phys_block, (const uint8_t *)buf + total) < 0) {
                return -EIO;
            }
        } else {
            /* Partial block write: read-modify-write */
            if (storage_read_block(phys_block, blk_buf) < 0) {
                return -EIO;
            }
            memcpy(blk_buf + in_blk_off, (const uint8_t *)buf + total, to_write);
            if (storage_write_block(phys_block, blk_buf) < 0) {
                return -EIO;
            }
        }

        total += to_write;
    }

    if (offset + total > inode.size) {
        inode.size = offset + total;
    }
    time_t now = time(NULL);
    inode.mtime = now;
    inode.ctime = now;
    ret = inode_write(ino, &inode);
    if (ret < 0) return ret;

    *bytes_written = total;
    return 0;
}
