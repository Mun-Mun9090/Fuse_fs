#ifndef FUSEFS_BITMAP_H
#define FUSEFS_BITMAP_H

#include <stdint.h>
#include <stdbool.h>

/* Inode bitmap helpers */
int  bitmap_alloc_inode(uint64_t *out_ino);
int  bitmap_free_inode(uint64_t ino);
bool bitmap_is_inode_allocated(uint64_t ino);

/* Data block bitmap helpers */
int  bitmap_alloc_block(uint64_t *out_block);
int  bitmap_free_block(uint64_t block_no);
bool bitmap_is_block_allocated(uint64_t block_no);

#endif /* FUSEFS_BITMAP_H */
