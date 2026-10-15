#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include "fs_ops.h"
#include "storage.h"
#include "inode.h"
#include "dir.h"
#include "bitmap.h"

static void *fusefs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino = 1;
    cfg->entry_timeout = 0.0;
    cfg->attr_timeout = 0.0;
    cfg->negative_timeout = 0.0;
    cfg->kernel_cache = 0;
    cfg->direct_io = 1;
    return NULL;
}

static void fusefs_destroy(void *private_data) {
    (void)private_data;
    fs_lock();
    storage_sync();
    fs_unlock();
}

static int fusefs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    fs_lock();
    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    stbuf->st_ino = (ino_t)ino;
    stbuf->st_mode = inode.mode;
    stbuf->st_nlink = inode.nlink;
    stbuf->st_uid = inode.uid;
    stbuf->st_gid = inode.gid;
    stbuf->st_size = inode.size;
    stbuf->st_blksize = FUSEFS_BLOCK_SIZE;
    stbuf->st_blocks = (inode.size + 511) / 512;
    stbuf->st_atime = inode.atime;
    stbuf->st_mtime = inode.mtime;
    stbuf->st_ctime = inode.ctime;

    fs_unlock();
    return 0;
}

struct readdir_context {
    void *buf;
    fuse_fill_dir_t filler;
};

static int readdir_cb(const struct fusefs_dirent *de, void *user_data) {
    struct readdir_context *ctx = (struct readdir_context *)user_data;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->inode;

    struct fusefs_inode inode;
    if (inode_read(de->inode, &inode) == 0) {
        st.st_mode = inode.mode;
    }

    if (ctx->filler(ctx->buf, de->name, &st, 0, 0) != 0) {
        return 1; /* Buffer full */
    }
    return 0;
}

static int fusefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    fs_lock();
    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct readdir_context ctx = { .buf = buf, .filler = filler };
    ret = dir_iterate(ino, readdir_cb, &ctx);

    fs_unlock();
    return (ret > 0) ? 0 : ret;
}

static int fusefs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    if (fi) {
        fi->direct_io = 1;
    }
    fs_lock();

    uint64_t parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    int ret = dir_lookup_parent(path, &parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fuse_context *ctx = fuse_get_context();
    uint64_t new_ino = 0;
    ret = inode_create(S_IFREG | (mode & 0777), ctx->uid, ctx->gid, &new_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    ret = dir_add_entry(parent_ino, basename, new_ino, DT_REG);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_open(const char *path, struct fuse_file_info *fi) {
    if (fi) {
        fi->direct_io = 1;
    }
    fs_lock();
    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    fs_unlock();
    return ret;
}

static int fusefs_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
    (void)fi;
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    size_t bytes_read = 0;
    ret = inode_read_data(ino, buf, size, offset, &bytes_read);
    fs_unlock();

    if (ret < 0) return ret;
    return (int)bytes_read;
}

static int fusefs_write(const char *path, const char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (fi && (fi->flags & O_APPEND)) {
        struct fusefs_inode in;
        if (inode_read(ino, &in) == 0) {
            offset = (off_t)in.size;
        }
    }

    size_t bytes_written = 0;
    ret = inode_write_data(ino, buf, size, offset, &bytes_written);
    if (ret == 0) {
        storage_sync();
    }

    fs_unlock();
    if (ret < 0) return ret;
    return (int)bytes_written;
}

static int fusefs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    ret = inode_truncate(ino, size);
    if (ret == 0) {
        storage_sync();
    }

    fs_unlock();
    return ret;
}

static int fusefs_unlink(const char *path) {
    fs_lock();

    uint64_t parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    int ret = dir_lookup_parent(path, &parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    uint64_t target_ino = 0;
    ret = dir_remove_entry(parent_ino, basename, &target_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(target_ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (inode.nlink > 1) {
        inode.nlink--;
        time_t now = time(NULL);
        inode.ctime = now;
        inode_write(target_ino, &inode);
    } else {
        inode_free(target_ino);
    }

    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_mkdir(const char *path, mode_t mode) {
    fs_lock();

    uint64_t parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    int ret = dir_lookup_parent(path, &parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fuse_context *ctx = fuse_get_context();
    uint64_t new_ino = 0;
    ret = inode_create(S_IFDIR | (mode & 0777), ctx->uid, ctx->gid, &new_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    /* Initialize '.' and '..' entries */
    ret = dir_add_entry(new_ino, ".", new_ino, DT_DIR);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    ret = dir_add_entry(new_ino, "..", parent_ino, DT_DIR);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    /* Add new directory to parent */
    ret = dir_add_entry(parent_ino, basename, new_ino, DT_DIR);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    /* Increment parent directory nlink */
    struct fusefs_inode parent_inode;
    if (inode_read(parent_ino, &parent_inode) == 0) {
        parent_inode.nlink++;
        inode_write(parent_ino, &parent_inode);
    }

    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_rmdir(const char *path) {
    fs_lock();

    uint64_t target_ino = 0;
    int ret = dir_lookup_path(path, &target_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (target_ino == FUSEFS_ROOT_INO) {
        fs_unlock();
        return -EBUSY;
    }

    if (!dir_is_empty(target_ino)) {
        fs_unlock();
        return -ENOTEMPTY;
    }

    uint64_t parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    ret = dir_lookup_parent(path, &parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    uint64_t removed_ino = 0;
    ret = dir_remove_entry(parent_ino, basename, &removed_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    /* Decrement parent nlink for '..' removal */
    struct fusefs_inode parent_inode;
    if (inode_read(parent_ino, &parent_inode) == 0 && parent_inode.nlink > 2) {
        parent_inode.nlink--;
        inode_write(parent_ino, &parent_inode);
    }

    inode_free(target_ino);
    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    inode.mode = (inode.mode & S_IFMT) | (mode & 07777);
    inode.ctime = time(NULL);
    ret = inode_write(ino, &inode);
    if (ret == 0) storage_sync();

    fs_unlock();
    return ret;
}

static int fusefs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (uid != (uid_t)-1) inode.uid = uid;
    if (gid != (gid_t)-1) inode.gid = gid;
    inode.ctime = time(NULL);
    ret = inode_write(ino, &inode);
    if (ret == 0) storage_sync();

    fs_unlock();
    return ret;
}

static int fusefs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void)fi;
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    inode.atime = tv[0].tv_sec;
    inode.mtime = tv[1].tv_sec;
    inode.ctime = time(NULL);
    ret = inode_write(ino, &inode);
    if (ret == 0) storage_sync();

    fs_unlock();
    return ret;
}

static int fusefs_link(const char *from, const char *to) {
    fs_lock();

    uint64_t source_ino = 0;
    int ret = dir_lookup_path(from, &source_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(source_ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (S_ISDIR(inode.mode)) {
        fs_unlock();
        return -EPERM;
    }

    uint64_t to_parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    ret = dir_lookup_parent(to, &to_parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    ret = dir_add_entry(to_parent_ino, basename, source_ino, DT_REG);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    inode.nlink++;
    inode.ctime = time(NULL);
    inode_write(source_ino, &inode);
    storage_sync();

    fs_unlock();
    return 0;
}

static int fusefs_symlink(const char *to, const char *from) {
    fs_lock();

    uint64_t parent_ino = 0;
    char basename[FUSEFS_NAME_MAX];
    int ret = dir_lookup_parent(from, &parent_ino, basename);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fuse_context *ctx = fuse_get_context();
    uint64_t new_ino = 0;
    ret = inode_create(S_IFLNK | 0777, ctx->uid, ctx->gid, &new_ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    size_t target_len = strlen(to);
    struct fusefs_inode inode;
    ret = inode_read(new_ino, &inode);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    if (target_len < sizeof(inode.symlink_inline)) {
        strncpy(inode.symlink_inline, to, sizeof(inode.symlink_inline) - 1);
        inode.size = target_len;
        inode_write(new_ino, &inode);
    } else {
        size_t written = 0;
        inode_write_data(new_ino, to, target_len, 0, &written);
    }

    ret = dir_add_entry(parent_ino, basename, new_ino, DT_LNK);
    if (ret < 0) {
        inode_free(new_ino);
        fs_unlock();
        return ret;
    }

    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_readlink(const char *path, char *buf, size_t size) {
    fs_lock();

    uint64_t ino = 0;
    int ret = dir_lookup_path(path, &ino);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    struct fusefs_inode inode;
    ret = inode_read(ino, &inode);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    if (!S_ISLNK(inode.mode)) {
        fs_unlock();
        return -EINVAL;
    }

    if (inode.size < sizeof(inode.symlink_inline)) {
        size_t copy_len = (inode.size < size - 1) ? inode.size : size - 1;
        memcpy(buf, inode.symlink_inline, copy_len);
        buf[copy_len] = '\0';
    } else {
        size_t bytes_read = 0;
        ret = inode_read_data(ino, buf, size - 1, 0, &bytes_read);
        if (ret < 0) {
            fs_unlock();
            return ret;
        }
        buf[bytes_read] = '\0';
    }

    fs_unlock();
    return 0;
}

static int fusefs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    fs_lock();

    uint64_t from_parent = 0;
    char from_base[FUSEFS_NAME_MAX];
    int ret = dir_lookup_parent(from, &from_parent, from_base);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    uint64_t to_parent = 0;
    char to_base[FUSEFS_NAME_MAX];
    ret = dir_lookup_parent(to, &to_parent, to_base);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    uint64_t target_ino = 0;
    uint8_t file_type = 0;
    ret = dir_find_entry(from_parent, from_base, &target_ino, &file_type);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    /* Check if destination exists; if so, remove it */
    uint64_t existing_to_ino = 0;
    if (dir_find_entry(to_parent, to_base, &existing_to_ino, NULL) == 0) {
        dir_remove_entry(to_parent, to_base, NULL);
        struct fusefs_inode ex_inode;
        if (inode_read(existing_to_ino, &ex_inode) == 0) {
            if (ex_inode.nlink > 1) {
                ex_inode.nlink--;
                inode_write(existing_to_ino, &ex_inode);
            } else {
                inode_free(existing_to_ino);
            }
        }
    }

    /* Add entry to destination parent */
    ret = dir_add_entry(to_parent, to_base, target_ino, file_type);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    /* Remove entry from source parent */
    ret = dir_remove_entry(from_parent, from_base, NULL);
    if (ret < 0) {
        fs_unlock();
        return ret;
    }

    storage_sync();
    fs_unlock();
    return 0;
}

static int fusefs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    fs_lock();

    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize = FUSEFS_BLOCK_SIZE;
    stbuf->f_frsize = FUSEFS_BLOCK_SIZE;
    stbuf->f_blocks = g_fs.sb.data_blocks_count;
    stbuf->f_bfree = g_fs.sb.free_blocks;
    stbuf->f_bavail = g_fs.sb.free_blocks;
    stbuf->f_files = g_fs.sb.total_inodes;
    stbuf->f_ffree = g_fs.sb.free_inodes;
    stbuf->f_favail = g_fs.sb.free_inodes;
    stbuf->f_namemax = FUSEFS_NAME_MAX;

    fs_unlock();
    return 0;
}

const struct fuse_operations fusefs_oper = {
    .init       = fusefs_init,
    .destroy    = fusefs_destroy,
    .getattr    = fusefs_getattr,
    .readdir    = fusefs_readdir,
    .create     = fusefs_create,
    .open       = fusefs_open,
    .read       = fusefs_read,
    .write      = fusefs_write,
    .truncate   = fusefs_truncate,
    .unlink     = fusefs_unlink,
    .mkdir      = fusefs_mkdir,
    .rmdir      = fusefs_rmdir,
    .chmod      = fusefs_chmod,
    .chown      = fusefs_chown,
    .utimens    = fusefs_utimens,
    .link       = fusefs_link,
    .symlink    = fusefs_symlink,
    .readlink   = fusefs_readlink,
    .rename     = fusefs_rename,
    .statfs     = fusefs_statfs,
};
