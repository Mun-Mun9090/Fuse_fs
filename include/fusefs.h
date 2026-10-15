#ifndef FUSEFS_H
#define FUSEFS_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#ifndef DT_REG
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#endif

/*
 * ============================================================================
 * FuseFS On-Disk Structures and Constants
 * ============================================================================
 *
 * Disk Layout Overview:
 * +--------------------------------------------------------------------------+
 * | Block 0       : Superblock                                               |
 * | Block 1..IB   : Inode Bitmap (tracks allocated inodes)                   |
 * | Block IB+1..BB: Data Block Bitmap (tracks allocated data blocks)         |
 * | Block BB+1..IT: Inode Table (array of fusefs_inode structures)           |
 * | Block IT+1..EB: Data Blocks (directory entries, file data, indirect ptrs)|
 * +--------------------------------------------------------------------------+
 */

#define FUSEFS_MAGIC            0x46534653ULL /* "FSFS" in ASCII */
#define FUSEFS_BLOCK_SIZE       4096U         /* 4 KB per block */
#define FUSEFS_INODE_SIZE       256U          /* 256 bytes per inode (16 inodes/block) */
#define FUSEFS_INODES_PER_BLOCK (FUSEFS_BLOCK_SIZE / FUSEFS_INODE_SIZE)

#define FUSEFS_DIRECT_BLOCKS    12            /* 12 direct blocks: 48 KB */
#define FUSEFS_PTRS_PER_BLOCK   (FUSEFS_BLOCK_SIZE / sizeof(uint64_t)) /* 512 pointers per 4KB block */
/* Single indirect capacity: 512 * 4KB = 2 MB */
/* Double indirect capacity: 512 * 512 * 4KB = 1 GB */

#define FUSEFS_ROOT_INO         1ULL          /* Inode 1 is Root directory; Inode 0 is NULL/reserved */
#define FUSEFS_NAME_MAX         240           /* Max file name length */

/* Superblock state */
#define FUSEFS_STATE_CLEAN      0
#define FUSEFS_STATE_DIRTY      1

/*
 * Superblock (fits in Block 0)
 */
struct fusefs_superblock {
    uint64_t magic;                 /* Magic identifier: FUSEFS_MAGIC */
    uint32_t block_size;            /* Size of each filesystem block (4096) */
    uint32_t inode_size;            /* Size of each on-disk inode (256) */
    
    uint64_t total_blocks;          /* Total block count in image file */
    uint64_t total_inodes;          /* Total inode count in filesystem */
    
    uint64_t free_blocks;           /* Count of unallocated data blocks */
    uint64_t free_inodes;           /* Count of unallocated inodes */
    
    uint64_t inode_bitmap_start;    /* Starting block index of inode bitmap */
    uint64_t inode_bitmap_blocks;   /* Number of blocks in inode bitmap */
    
    uint64_t block_bitmap_start;    /* Starting block index of data block bitmap */
    uint64_t block_bitmap_blocks;   /* Number of blocks in data block bitmap */
    
    uint64_t inode_table_start;     /* Starting block index of inode table */
    uint64_t inode_table_blocks;    /* Number of blocks in inode table */
    
    uint64_t data_blocks_start;     /* Starting block index of data region */
    uint64_t data_blocks_count;     /* Number of data blocks */
    
    uint64_t root_inode;            /* Inode number of root directory (1) */
    uint32_t state;                 /* FUSEFS_STATE_CLEAN or FUSEFS_STATE_DIRTY */
    uint32_t mount_count;           /* Times filesystem has been mounted */
    
    uint8_t  padding[FUSEFS_BLOCK_SIZE - 128]; /* Pad out to exactly 1 block */
};

/*
 * On-Disk Inode Structure (exactly 256 bytes)
 */
struct fusefs_inode {
    uint16_t mode;                  /* File mode (type and permissions: S_IFREG, S_IFDIR, S_IFLNK, etc.) */
    uint16_t nlink;                 /* Number of hard links */
    uint32_t uid;                   /* Owner user ID */
    uint32_t gid;                   /* Owner group ID */
    uint32_t flags;                 /* File attribute flags */
    uint64_t size;                  /* Size of file content in bytes */
    
    uint64_t atime;                 /* Last access time (seconds) */
    uint64_t mtime;                 /* Last modification time (seconds) */
    uint64_t ctime;                 /* Last status change time (seconds) */
    
    uint64_t direct[FUSEFS_DIRECT_BLOCKS]; /* 12 direct block indices (96 bytes) */
    uint64_t single_indirect;       /* Single indirect block index */
    uint64_t double_indirect;       /* Double indirect block index */
    
    char     symlink_inline[64];    /* Fast symlink target for symlinks <= 63 bytes */
    uint8_t  padding[32];           /* Pad to exactly 256 bytes */
};

/*
 * On-Disk Directory Entry (256 bytes)
 * Exactly 16 directory entries per 4096-byte block.
 */
struct fusefs_dirent {
    uint64_t inode;                 /* Inode number (0 = unused/deleted entry) */
    uint8_t  file_type;             /* DT_REG, DT_DIR, DT_LNK, etc. */
    uint8_t  name_len;              /* Name length in bytes */
    uint8_t  padding[6];            /* Alignment padding */
    char     name[FUSEFS_NAME_MAX]; /* File name (null-terminated) */
};

#define FUSEFS_DIRENTS_PER_BLOCK (FUSEFS_BLOCK_SIZE / sizeof(struct fusefs_dirent))

_Static_assert(sizeof(struct fusefs_superblock) == FUSEFS_BLOCK_SIZE, "Superblock must be 4096 bytes");
_Static_assert(sizeof(struct fusefs_inode) == FUSEFS_INODE_SIZE, "Inode must be 256 bytes");
_Static_assert(sizeof(struct fusefs_dirent) == 256, "Dirent must be 256 bytes");

#endif /* FUSEFS_H */
