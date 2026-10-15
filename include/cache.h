#ifndef FUSEFS_CACHE_H
#define FUSEFS_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "fusefs.h"

struct cache_entry {
    uint64_t block_no;
    bool dirty;
    bool valid;
    uint8_t data[FUSEFS_BLOCK_SIZE];
    struct cache_entry *prev;
    struct cache_entry *next;
    struct cache_entry *hash_next;
};

struct block_cache {
    bool enabled;
    int capacity;
    int count;
    struct cache_entry *head; /* Most recently used */
    struct cache_entry *tail; /* Least recently used */
    struct cache_entry **hash_table;
    int hash_size;
    uint64_t hits;
    uint64_t misses;
};

int cache_init(struct block_cache *cache, int capacity, bool enabled);
int cache_read(struct block_cache *cache, int fd, uint64_t block_no, void *buf);
int cache_write(struct block_cache *cache, int fd, uint64_t block_no, const void *buf);
int cache_flush(struct block_cache *cache, int fd);
void cache_destroy(struct block_cache *cache, int fd);
void cache_get_stats(struct block_cache *cache, uint64_t *hits, uint64_t *misses);

#endif /* FUSEFS_CACHE_H */
