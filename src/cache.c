#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "cache.h"
#include "fusefs.h"

#define HASH_MASK(size, key) ((key) % (size))

static struct cache_entry *find_in_hash(struct block_cache *cache, uint64_t block_no) {
    if (!cache->hash_table) return NULL;
    int idx = HASH_MASK(cache->hash_size, block_no);
    struct cache_entry *curr = cache->hash_table[idx];
    while (curr) {
        if (curr->valid && curr->block_no == block_no) {
            return curr;
        }
        curr = curr->hash_next;
    }
    return NULL;
}

static void insert_into_hash(struct block_cache *cache, struct cache_entry *entry) {
    int idx = HASH_MASK(cache->hash_size, entry->block_no);
    entry->hash_next = cache->hash_table[idx];
    cache->hash_table[idx] = entry;
}

static void remove_from_hash(struct block_cache *cache, struct cache_entry *entry) {
    int idx = HASH_MASK(cache->hash_size, entry->block_no);
    struct cache_entry **curr = &cache->hash_table[idx];
    while (*curr) {
        if (*curr == entry) {
            *curr = entry->hash_next;
            entry->hash_next = NULL;
            return;
        }
        curr = &((*curr)->hash_next);
    }
}

static void detach_lru(struct block_cache *cache, struct cache_entry *entry) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->head = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->tail = entry->prev;
    }
    entry->prev = NULL;
    entry->next = NULL;
}

static void attach_lru_head(struct block_cache *cache, struct cache_entry *entry) {
    entry->prev = NULL;
    entry->next = cache->head;
    if (cache->head) {
        cache->head->prev = entry;
    }
    cache->head = entry;
    if (!cache->tail) {
        cache->tail = entry;
    }
}

int cache_init(struct block_cache *cache, int capacity, bool enabled) {
    memset(cache, 0, sizeof(*cache));
    cache->enabled = enabled;
    cache->capacity = (capacity > 0) ? capacity : 1024;
    cache->hash_size = cache->capacity * 2;
    cache->hash_table = calloc(cache->hash_size, sizeof(struct cache_entry *));
    if (!cache->hash_table) {
        return -ENOMEM;
    }
    return 0;
}

int cache_flush(struct block_cache *cache, int fd) {
    if (!cache || !cache->enabled) return 0;
    struct cache_entry *curr = cache->head;
    while (curr) {
        if (curr->valid && curr->dirty) {
            off_t offset = (off_t)curr->block_no * FUSEFS_BLOCK_SIZE;
            ssize_t ret = pwrite(fd, curr->data, FUSEFS_BLOCK_SIZE, offset);
            if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
                return (ret < 0) ? -errno : -EIO;
            }
            curr->dirty = false;
        }
        curr = curr->next;
    }
    fsync(fd);
    return 0;
}

void cache_destroy(struct block_cache *cache, int fd) {
    if (!cache) return;
    if (cache->enabled) {
        cache_flush(cache, fd);
    }
    struct cache_entry *curr = cache->head;
    while (curr) {
        struct cache_entry *next = curr->next;
        free(curr);
        curr = next;
    }
    free(cache->hash_table);
    memset(cache, 0, sizeof(*cache));
}

int cache_read(struct block_cache *cache, int fd, uint64_t block_no, void *buf) {
    if (!cache->enabled) {
        off_t offset = (off_t)block_no * FUSEFS_BLOCK_SIZE;
        ssize_t ret = pread(fd, buf, FUSEFS_BLOCK_SIZE, offset);
        if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
            return (ret < 0) ? -errno : -EIO;
        }
        return 0;
    }

    struct cache_entry *entry = find_in_hash(cache, block_no);
    if (entry) {
        cache->hits++;
        detach_lru(cache, entry);
        attach_lru_head(cache, entry);
        memcpy(buf, entry->data, FUSEFS_BLOCK_SIZE);
        return 0;
    }

    cache->misses++;
    if (cache->count >= cache->capacity) {
        /* Evict LRU from tail */
        entry = cache->tail;
        if (entry->dirty) {
            off_t offset = (off_t)entry->block_no * FUSEFS_BLOCK_SIZE;
            ssize_t ret = pwrite(fd, entry->data, FUSEFS_BLOCK_SIZE, offset);
            if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
                return (ret < 0) ? -errno : -EIO;
            }
            entry->dirty = false;
        }
        remove_from_hash(cache, entry);
        detach_lru(cache, entry);
    } else {
        entry = malloc(sizeof(*entry));
        if (!entry) return -ENOMEM;
        cache->count++;
    }

    off_t offset = (off_t)block_no * FUSEFS_BLOCK_SIZE;
    ssize_t ret = pread(fd, entry->data, FUSEFS_BLOCK_SIZE, offset);
    if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
        free(entry);
        cache->count--;
        return (ret < 0) ? -errno : -EIO;
    }

    entry->block_no = block_no;
    entry->dirty = false;
    entry->valid = true;
    insert_into_hash(cache, entry);
    attach_lru_head(cache, entry);

    memcpy(buf, entry->data, FUSEFS_BLOCK_SIZE);
    return 0;
}

int cache_write(struct block_cache *cache, int fd, uint64_t block_no, const void *buf) {
    if (!cache->enabled) {
        off_t offset = (off_t)block_no * FUSEFS_BLOCK_SIZE;
        ssize_t ret = pwrite(fd, buf, FUSEFS_BLOCK_SIZE, offset);
        if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
            return (ret < 0) ? -errno : -EIO;
        }
        return 0;
    }

    struct cache_entry *entry = find_in_hash(cache, block_no);
    if (entry) {
        memcpy(entry->data, buf, FUSEFS_BLOCK_SIZE);
        entry->dirty = true;
        detach_lru(cache, entry);
        attach_lru_head(cache, entry);
        return 0;
    }

    if (cache->count >= cache->capacity) {
        entry = cache->tail;
        if (entry->dirty) {
            off_t offset = (off_t)entry->block_no * FUSEFS_BLOCK_SIZE;
            ssize_t ret = pwrite(fd, entry->data, FUSEFS_BLOCK_SIZE, offset);
            if (ret != (ssize_t)FUSEFS_BLOCK_SIZE) {
                return (ret < 0) ? -errno : -EIO;
            }
            entry->dirty = false;
        }
        remove_from_hash(cache, entry);
        detach_lru(cache, entry);
    } else {
        entry = malloc(sizeof(*entry));
        if (!entry) return -ENOMEM;
        cache->count++;
    }

    entry->block_no = block_no;
    memcpy(entry->data, buf, FUSEFS_BLOCK_SIZE);
    entry->dirty = true;
    entry->valid = true;
    insert_into_hash(cache, entry);
    attach_lru_head(cache, entry);
    return 0;
}

void cache_get_stats(struct block_cache *cache, uint64_t *hits, uint64_t *misses) {
    if (hits) *hits = cache ? cache->hits : 0;
    if (misses) *misses = cache ? cache->misses : 0;
}
