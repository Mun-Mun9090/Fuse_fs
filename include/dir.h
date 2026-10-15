#ifndef FUSEFS_DIR_H
#define FUSEFS_DIR_H

#include <stdint.h>
#include <stdbool.h>
#include "fusefs.h"

int dir_lookup_path(const char *path, uint64_t *out_ino);
int dir_lookup_parent(const char *path, uint64_t *out_parent_ino, char *out_basename);

int dir_find_entry(uint64_t dir_ino, const char *name, uint64_t *out_ino, uint8_t *out_file_type);
int dir_add_entry(uint64_t dir_ino, const char *name, uint64_t target_ino, uint8_t file_type);
int dir_remove_entry(uint64_t dir_ino, const char *name, uint64_t *out_removed_ino);
int dir_is_empty(uint64_t dir_ino);

typedef int (*dir_iter_cb)(const struct fusefs_dirent *de, void *user_data);
int dir_iterate(uint64_t dir_ino, dir_iter_cb cb, void *user_data);

#endif /* FUSEFS_DIR_H */
