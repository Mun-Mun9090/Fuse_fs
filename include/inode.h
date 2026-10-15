#ifndef FUSEFS_INODE_H
#define FUSEFS_INODE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "fusefs.h"

int inode_read(uint64_t ino, struct fusefs_inode *out_inode);
int inode_write(uint64_t ino, const struct fusefs_inode *inode);

int inode_create(uint16_t mode, uint32_t uid, uint32_t gid, uint64_t *out_ino);
int inode_free(uint64_t ino);

int inode_bmap(uint64_t ino, struct fusefs_inode *inode, uint64_t file_block_idx, bool allocate, uint64_t *out_phys_block);
int inode_truncate(uint64_t ino, uint64_t new_size);

int inode_read_data(uint64_t ino, void *buf, size_t size, off_t offset, size_t *bytes_read);
int inode_write_data(uint64_t ino, const void *buf, size_t size, off_t offset, size_t *bytes_written);

#endif /* FUSEFS_INODE_H */
