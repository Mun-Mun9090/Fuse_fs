#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include "dir.h"
#include "inode.h"
#include "storage.h"

int dir_find_entry(uint64_t dir_ino, const char *name, uint64_t *out_ino, uint8_t *out_file_type) {
    struct fusefs_inode dir_inode;
    int ret = inode_read(dir_ino, &dir_inode);
    if (ret < 0) return ret;

    if (!S_ISDIR(dir_inode.mode)) {
        return -ENOTDIR;
    }

    uint64_t num_blocks = (dir_inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t phys_block = 0;
        ret = inode_bmap(dir_ino, &dir_inode, b, false, &phys_block);
        if (ret < 0) return ret;
        if (phys_block == 0) continue;

        if (storage_read_block(phys_block, blk_buf) < 0) {
            return -EIO;
        }

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode != 0) {
                if (strcmp(entries[i].name, name) == 0) {
                    if (out_ino) *out_ino = entries[i].inode;
                    if (out_file_type) *out_file_type = entries[i].file_type;
                    return 0;
                }
            }
        }
    }
    return -ENOENT;
}

int dir_add_entry(uint64_t dir_ino, const char *name, uint64_t target_ino, uint8_t file_type) {
    size_t len = strlen(name);
    if (len == 0 || len >= FUSEFS_NAME_MAX) {
        return -EINVAL;
    }

    uint64_t dummy;
    if (dir_find_entry(dir_ino, name, &dummy, NULL) == 0) {
        return -EEXIST;
    }

    struct fusefs_inode dir_inode;
    int ret = inode_read(dir_ino, &dir_inode);
    if (ret < 0) return ret;

    if (!S_ISDIR(dir_inode.mode)) {
        return -ENOTDIR;
    }

    uint64_t num_blocks = (dir_inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    /* Search for empty slot in existing blocks */
    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t phys_block = 0;
        ret = inode_bmap(dir_ino, &dir_inode, b, false, &phys_block);
        if (ret < 0) return ret;
        if (phys_block == 0) continue;

        if (storage_read_block(phys_block, blk_buf) < 0) {
            return -EIO;
        }

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode == 0) {
                entries[i].inode = target_ino;
                entries[i].file_type = file_type;
                entries[i].name_len = (uint8_t)len;
                strncpy(entries[i].name, name, FUSEFS_NAME_MAX - 1);
                entries[i].name[FUSEFS_NAME_MAX - 1] = '\0';

                if (storage_write_block(phys_block, blk_buf) < 0) {
                    return -EIO;
                }

                uint64_t entry_end = b * FUSEFS_BLOCK_SIZE + (i + 1) * sizeof(struct fusefs_dirent);
                if (entry_end > dir_inode.size) {
                    dir_inode.size = entry_end;
                }
                time_t now = time(NULL);
                dir_inode.mtime = now;
                dir_inode.ctime = now;
                return inode_write(dir_ino, &dir_inode);
            }
        }
    }

    /* Allocate new directory block */
    uint64_t new_phys_block = 0;
    ret = inode_bmap(dir_ino, &dir_inode, num_blocks, true, &new_phys_block);
    if (ret < 0) return ret;

    memset(blk_buf, 0, FUSEFS_BLOCK_SIZE);
    struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
    entries[0].inode = target_ino;
    entries[0].file_type = file_type;
    entries[0].name_len = (uint8_t)len;
    strncpy(entries[0].name, name, FUSEFS_NAME_MAX - 1);
    entries[0].name[FUSEFS_NAME_MAX - 1] = '\0';

    if (storage_write_block(new_phys_block, blk_buf) < 0) {
        return -EIO;
    }

    dir_inode.size = (num_blocks * FUSEFS_BLOCK_SIZE) + sizeof(struct fusefs_dirent);
    time_t now = time(NULL);
    dir_inode.mtime = now;
    dir_inode.ctime = now;
    return inode_write(dir_ino, &dir_inode);
}

int dir_remove_entry(uint64_t dir_ino, const char *name, uint64_t *out_removed_ino) {
    struct fusefs_inode dir_inode;
    int ret = inode_read(dir_ino, &dir_inode);
    if (ret < 0) return ret;

    if (!S_ISDIR(dir_inode.mode)) {
        return -ENOTDIR;
    }

    uint64_t num_blocks = (dir_inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t phys_block = 0;
        ret = inode_bmap(dir_ino, &dir_inode, b, false, &phys_block);
        if (ret < 0) return ret;
        if (phys_block == 0) continue;

        if (storage_read_block(phys_block, blk_buf) < 0) {
            return -EIO;
        }

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode != 0 && strcmp(entries[i].name, name) == 0) {
                if (out_removed_ino) {
                    *out_removed_ino = entries[i].inode;
                }
                memset(&entries[i], 0, sizeof(struct fusefs_dirent));
                if (storage_write_block(phys_block, blk_buf) < 0) {
                    return -EIO;
                }
                time_t now = time(NULL);
                dir_inode.mtime = now;
                dir_inode.ctime = now;
                return inode_write(dir_ino, &dir_inode);
            }
        }
    }
    return -ENOENT;
}

int dir_is_empty(uint64_t dir_ino) {
    struct fusefs_inode dir_inode;
    int ret = inode_read(dir_ino, &dir_inode);
    if (ret < 0) return ret;

    uint64_t num_blocks = (dir_inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t phys_block = 0;
        ret = inode_bmap(dir_ino, &dir_inode, b, false, &phys_block);
        if (ret < 0) return ret;
        if (phys_block == 0) continue;

        if (storage_read_block(phys_block, blk_buf) < 0) {
            return -EIO;
        }

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode != 0) {
                if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0) {
                    return 0; /* Not empty */
                }
            }
        }
    }
    return 1; /* Empty */
}

int dir_iterate(uint64_t dir_ino, dir_iter_cb cb, void *user_data) {
    struct fusefs_inode dir_inode;
    int ret = inode_read(dir_ino, &dir_inode);
    if (ret < 0) return ret;

    if (!S_ISDIR(dir_inode.mode)) {
        return -ENOTDIR;
    }

    uint64_t num_blocks = (dir_inode.size + FUSEFS_BLOCK_SIZE - 1) / FUSEFS_BLOCK_SIZE;
    uint8_t blk_buf[FUSEFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t phys_block = 0;
        ret = inode_bmap(dir_ino, &dir_inode, b, false, &phys_block);
        if (ret < 0) return ret;
        if (phys_block == 0) continue;

        if (storage_read_block(phys_block, blk_buf) < 0) {
            return -EIO;
        }

        struct fusefs_dirent *entries = (struct fusefs_dirent *)blk_buf;
        for (uint32_t i = 0; i < FUSEFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].inode != 0) {
                int cb_ret = cb(&entries[i], user_data);
                if (cb_ret != 0) {
                    return cb_ret;
                }
            }
        }
    }
    return 0;
}

int dir_lookup_path(const char *path, uint64_t *out_ino) {
    if (!path || path[0] != '/') {
        return -EINVAL;
    }

    if (strcmp(path, "/") == 0) {
        *out_ino = FUSEFS_ROOT_INO;
        return 0;
    }

    char *path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;

    uint64_t curr_ino = FUSEFS_ROOT_INO;
    char *token = strtok(path_copy, "/");

    while (token != NULL) {
        uint64_t next_ino = 0;
        int ret = dir_find_entry(curr_ino, token, &next_ino, NULL);
        if (ret < 0) {
            free(path_copy);
            return ret;
        }
        curr_ino = next_ino;
        token = strtok(NULL, "/");
    }

    free(path_copy);
    *out_ino = curr_ino;
    return 0;
}

int dir_lookup_parent(const char *path, uint64_t *out_parent_ino, char *out_basename) {
    if (!path || path[0] != '/' || strcmp(path, "/") == 0) {
        return -EINVAL;
    }

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        /* Direct child of root, e.g. "/foo" */
        *out_parent_ino = FUSEFS_ROOT_INO;
        strncpy(out_basename, last_slash + 1, FUSEFS_NAME_MAX - 1);
        out_basename[FUSEFS_NAME_MAX - 1] = '\0';
        return 0;
    }

    size_t parent_len = last_slash - path;
    char parent_path[512];
    if (parent_len >= sizeof(parent_path)) return -ENAMETOOLONG;

    strncpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    strncpy(out_basename, last_slash + 1, FUSEFS_NAME_MAX - 1);
    out_basename[FUSEFS_NAME_MAX - 1] = '\0';

    return dir_lookup_path(parent_path, out_parent_ino);
}
