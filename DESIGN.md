# FuseFS Architecture & Design Document

## 1. Executive Summary
**FuseFS** is a robust, POSIX-compliant userspace filesystem implemented in C11 using the Linux FUSE 3 interface. Backed by a single disk image file, it realizes all fundamental operating system storage abstractions:
- Disk layout with superblocks, bitmap allocation, and an inode table.
- Multi-level hierarchical block mapping (`bmap`) supporting direct blocks, single indirect blocks, and double indirect blocks (up to 1 GB files).
- Directory tree management with fixed-size directory records and hierarchical path resolution.
- An LRU write-back buffer cache providing measurable throughput speedup.
- Thread-safe recursive mutex synchronization for multi-process concurrency.
- A 5-pass consistency checker (`fsck.fusefs`) that validates structures, link counts, and detects block leaks.

---

## 2. On-Disk Layout
The backing image is segmented into 4096-byte (4 KB) blocks:

```
+---------------+-------------------+--------------------+-------------------+-----------------------+
| Block 0       | Block 1 .. IB     | Block IB+1 .. BB   | Block BB+1 .. IT  | Block IT+1 .. Total   |
| Superblock    | Inode Bitmap      | Data Block Bitmap  | Inode Table       | Data Blocks           |
+---------------+-------------------+--------------------+-------------------+-----------------------+
```

### 2.1 Superblock (`struct fusefs_superblock`, Block 0)
The superblock occupies the first 4096 bytes and defines filesystem geometry and metadata boundaries:
- `magic`: `0x46534653ULL` ("FSFS").
- `block_size`: 4096 bytes.
- `inode_size`: 256 bytes (16 inodes per 4KB block).
- `total_blocks`, `total_inodes`: Total capacity counters.
- `free_blocks`, `free_inodes`: Real-time allocation counters.
- `inode_bitmap_start`, `inode_bitmap_blocks`: Boundary of the inode allocation bitmap.
- `block_bitmap_start`, `block_bitmap_blocks`: Boundary of the data block allocation bitmap.
- `inode_table_start`, `inode_table_blocks`: Boundary of the linear inode table.
- `data_blocks_start`, `data_blocks_count`: Region holding file and directory contents.
- `state`: `FUSEFS_STATE_CLEAN` (0) or `FUSEFS_STATE_DIRTY` (1). Set to dirty on mount and clean on orderly unmount.

### 2.2 Bitmaps
Bitmaps track allocation state using 1 bit per entity (1 = allocated, 0 = free):
- **Inode Bitmap**: Tracks inodes 0 through `total_inodes - 1`. Inode 0 is permanently reserved; Inode 1 is the Root directory (`/`).
- **Data Block Bitmap**: Tracks relative data block indices 0 through `data_blocks_count - 1`, where relative index $i$ corresponds to physical block `data_blocks_start + i`.

### 2.3 On-Disk Inode Structure (`struct fusefs_inode`, 256 Bytes)
Each inode structure is exactly 256 bytes, guaranteeing clean power-of-two alignment with zero cross-block tearing:
- Metadata: `mode` (type & permissions), `nlink` (hard links), `uid`, `gid`, `size` (bytes), `atime`, `mtime`, `ctime`.
- Block Map:
  - `direct[12]`: 12 direct block pointers (up to $12 \times 4\text{ KB} = 48\text{ KB}$).
  - `single_indirect`: Points to a block containing 512 `uint64_t` block pointers (up to $512 \times 4\text{ KB} = 2\text{ MB}$).
  - `double_indirect`: Points to a block containing 512 indirect pointer blocks (up to $512 \times 512 \times 4\text{ KB} = 1\text{ GB}$).
- Inline symlink buffer: `symlink_inline[128]` for zero-allocation fast symbolic links.

---

## 3. Block Mapping Algorithm (`bmap`)
When resolving file block index $B = \lfloor \text{offset} / 4096 \rfloor$:

```
                             File Block Index B
                                      |
         +----------------------------+----------------------------+
         |                                                         |
      B < 12                                                 12 <= B < 524
   Direct Blocks                                         Single Indirect Block
   inode->direct[B]                                 inode->single_indirect[B - 12]
                                                                   |
                                                            524 <= B < 262668
                                                          Double Indirect Block
                                                      L1 = (B - 524) / 512
                                                      L2 = (B - 524) % 512
```

1. **Direct Region ($0 \le B < 12$)**:
   Directly indexed in `inode->direct[B]`. If unallocated during write, a new block is fetched from `bitmap_alloc_block()`.
2. **Single Indirect Region ($12 \le B < 524$)**:
   Relative index $idx = B - 12$. The pointer block `inode->single_indirect` is read; `ptrs[idx]` resolves to the physical block.
3. **Double Indirect Region ($524 \le B < 262,668$)**:
   Relative index $D = B - 524$. Level 1 index $L_1 = \lfloor D / 512 \rfloor$, Level 2 index $L_2 = D \pmod{512}$. Reads Level 1 pointer block $\to$ reads Level 2 pointer block $\to$ resolves physical data block.

---

## 4. Directory Subsystem
Directories are stored as special files (`S_IFDIR`) containing 256-byte directory entry records (`struct fusefs_dirent`):
- `inode`: Target inode number ($0$ marks a deleted or empty slot).
- `file_type`: `DT_REG`, `DT_DIR`, `DT_LNK`.
- `name_len`: Length of filename.
- `name[240]`: Null-terminated name string.

### Characteristics:
- Exactly 16 directory entries per 4096-byte block.
- Fast linear scan with slot re-use for deleted entries.
- Invariant: Every directory is initialized with `.` (pointing to self) and `..` (pointing to parent).
- Deletion safety: `rmdir` enforces that a directory contains only `.` and `..` before permitting unlinking.

---

## 5. Buffer Cache Architecture
To decouple I/O bottlenecks and minimize image disk writes, FuseFS incorporates an in-memory buffer cache:
- **Structure**:
  - Hash table with chaining for $O(1)$ block retrieval.
  - Doubly-linked list maintaining strict Least-Recently-Used (LRU) eviction order.
- **Write-Back Policy**:
  - Blocks modified in memory are tagged `dirty = true`.
  - When the cache capacity is exhausted, the LRU block at the tail is evicted; if dirty, it is flushed via `pwrite()` before reuse.
  - Explicit synchronization (`storage_sync()`, unmount, or `fsync`) flushes all dirty cache entries to disk.
- **Benchmark Toggle**:
  - Can be toggled at mount time (`--no-cache` or `--cache-size=N`), allowing direct measurement of cache acceleration.

---

## 6. Crash Consistency & Ordering
FuseFS adopts disciplined write ordering to prevent catastrophic on-disk inconsistencies:
1. **Zeroing New Blocks**: Newly allocated data and pointer blocks are zeroed prior to publishing their pointers in inodes or indirect tables. This eliminates security leakage and dangling garbage pointers.
2. **Metadata Flush Ordering**:
   - Data blocks are written before updating the inode pointer map.
   - Inodes are written before directory entries referencing them are made visible.
   - Bitmap allocations are committed before parent directory records are persisted.
3. **Dirty State Tracking**: On mount, `sb.state` is marked `FUSEFS_STATE_DIRTY`. On orderly unmount, all caches are flushed, `sb.state` is transitioned to `FUSEFS_STATE_CLEAN`, and the file is synced. If an unmount was abnormal, `fsck.fusefs` detects the dirty flag and verifies structural consistency.

---

## 7. Consistency Checker (`fsck.fusefs`)
`fsck.fusefs` operates a 5-pass verification algorithm:
- **Pass 1 (Superblock)**: Validates magic number, geometry, and metadata bounds.
- **Pass 2 (Inode & Block Mapping)**: Reads all allocated inodes; traverses direct, single indirect, and double indirect pointers; verifies boundary compliance and identifies double-allocated blocks.
- **Pass 3 (Directory Tree Traversal)**: Traverses the hierarchy from root (`inode 1`), checks `.` and `..` mappings, validates link counts against actual directory references, and flags orphaned inodes.
- **Pass 4 (Bitmap Verification)**: Reconciles block and inode reference counts against disk bitmaps:
  - Detects **Leaked Blocks** (marked allocated in bitmap, but unreferenced).
  - Detects **Phantom Blocks** (in active use, but marked free in bitmap).
- **Pass 5 (Repair Mode)**: When invoked with `--repair`, fixes discrepancies, updates link counts, reclaims leaked blocks, and marks the superblock clean.
