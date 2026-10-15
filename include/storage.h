#ifndef FUSEFS_STORAGE_H
#define FUSEFS_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "fusefs.h"
#include "cache.h"

struct fusefs_context {
    int image_fd;
    char image_path[512];
    struct fusefs_superblock sb;
    struct block_cache cache;
    pthread_mutex_t lock;
    bool is_mounted;
};

extern struct fusefs_context g_fs;

int  storage_open(const char *path, bool cache_enabled, int cache_size);
void storage_close(void);
int  storage_read_block(uint64_t block_no, void *buf);
int  storage_write_block(uint64_t block_no, const void *buf);
int  storage_flush_sb(void);
int  storage_sync(void);

void fs_lock(void);
void fs_unlock(void);

#endif /* FUSEFS_STORAGE_H */
