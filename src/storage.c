#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "storage.h"

struct fusefs_context g_fs;

void fs_lock(void) {
    pthread_mutex_lock(&g_fs.lock);
}

void fs_unlock(void) {
    pthread_mutex_unlock(&g_fs.lock);
}

int storage_open(const char *path, bool cache_enabled, int cache_size) {
    memset(&g_fs, 0, sizeof(g_fs));
    strncpy(g_fs.image_path, path, sizeof(g_fs.image_path) - 1);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_fs.lock, &attr);
    pthread_mutexattr_destroy(&attr);

    g_fs.image_fd = open(path, O_RDWR);
    if (g_fs.image_fd < 0) {
        perror("open disk image");
        return -errno;
    }

    if (cache_init(&g_fs.cache, cache_size, cache_enabled) < 0) {
        close(g_fs.image_fd);
        return -ENOMEM;
    }

    /* Read superblock directly */
    ssize_t ret = pread(g_fs.image_fd, &g_fs.sb, sizeof(struct fusefs_superblock), 0);
    if (ret != sizeof(struct fusefs_superblock)) {
        fprintf(stderr, "Failed to read superblock\n");
        close(g_fs.image_fd);
        return -EIO;
    }

    if (g_fs.sb.magic != FUSEFS_MAGIC) {
        fprintf(stderr, "Invalid filesystem magic: 0x%lx (expected 0x%lx)\n",
                (unsigned long)g_fs.sb.magic, (unsigned long)FUSEFS_MAGIC);
        close(g_fs.image_fd);
        return -EINVAL;
    }

    if (g_fs.sb.block_size != FUSEFS_BLOCK_SIZE) {
        fprintf(stderr, "Unsupported block size: %u\n", g_fs.sb.block_size);
        close(g_fs.image_fd);
        return -EINVAL;
    }

    g_fs.sb.state = FUSEFS_STATE_DIRTY;
    g_fs.sb.mount_count++;
    storage_flush_sb();
    g_fs.is_mounted = true;

    return 0;
}

int storage_flush_sb(void) {
    ssize_t ret = pwrite(g_fs.image_fd, &g_fs.sb, sizeof(struct fusefs_superblock), 0);
    if (ret != sizeof(struct fusefs_superblock)) {
        return -errno;
    }
    return 0;
}

int storage_read_block(uint64_t block_no, void *buf) {
    if (block_no >= g_fs.sb.total_blocks) {
        return -EINVAL;
    }
    return cache_read(&g_fs.cache, g_fs.image_fd, block_no, buf);
}

int storage_write_block(uint64_t block_no, const void *buf) {
    if (block_no >= g_fs.sb.total_blocks) {
        return -EINVAL;
    }
    return cache_write(&g_fs.cache, g_fs.image_fd, block_no, buf);
}

int storage_sync(void) {
    int ret = cache_flush(&g_fs.cache, g_fs.image_fd);
    if (ret < 0) return ret;
    storage_flush_sb();
    return fsync(g_fs.image_fd);
}

void storage_close(void) {
    if (!g_fs.is_mounted) return;

    g_fs.sb.state = FUSEFS_STATE_CLEAN;
    storage_flush_sb();
    cache_destroy(&g_fs.cache, g_fs.image_fd);
    close(g_fs.image_fd);
    pthread_mutex_destroy(&g_fs.lock);
    g_fs.is_mounted = false;
}
