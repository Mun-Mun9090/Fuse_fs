# FuseFS — A Userspace Filesystem in C11 (FUSE 3)

**FuseFS** is a complete, mountable filesystem implemented in **C11** using the Linux **FUSE 3** API, backed by a single raw disk image file. It implements on-disk structures, multi-level block mapping (`bmap`), directory tree resolution, caching, crash consistency, and consistency verification.

---

## Features & Project Deliverables

| Category | Features |
| :--- | :--- |
| **Core Storage Spec** | Mountable FUSE daemon, Superblock, Inode Table, Free Bitmaps (Inodes & Blocks), Raw image I/O via `pread`/`pwrite`. |
| **Operations** | `create`, `open`, `read`, `write` (with dynamic file growth), `truncate`, `unlink`, `mkdir`, `rmdir`, `readdir`, `chmod`, `chown`, `utimens`, `rename`, `statfs`. |
| **Large-File Support** | **Direct blocks** (12 blocks = 48 KB), **Single Indirect blocks** (up to 2 MB), and **Double Indirect blocks** (up to 1 GB files). |
| **Links & Metadata** | Hard links (`link`) with accurate `nlink` tracking, Symbolic links (`symlink`, `readlink`), and timestamp updates (`atime`, `mtime`, `ctime`). |
| **Buffer Cache** | LRU write-back buffer cache with hash table indexing, dirty block tracking, and cache toggle (`--no-cache`). |
| **Concurrency** | Thread-safe recursive mutex synchronization supporting concurrent multi-process access without metadata corruption. |
| **FSCK Checker** | 5-pass consistency checker (`fsck.fusefs`) detecting leaked blocks, phantom blocks, corrupted link counts, with `--repair` capability. |
| **File Version History** | Manual snapshots, listing, and restoring prior file contents with the project-specific `fusefs-version` command. |
| **Test Suites** | Automated functional battery, persistence tests, large-file tests, cache benchmark, and concurrency stress tests. |

---

## Directory Structure

```
Fusefs/
├── Makefile                 # Compiles FuseFS utilities and installs the version command
├── README.md                # Complete setup and execution guide
├── DESIGN.md                # In-depth architectural specification
├── include/                 # Header declarations
│   ├── fusefs.h             # On-disk structures (superblock, inode, dirent)
│   ├── storage.h            # Low-level image I/O and mutex locking
│   ├── cache.h              # LRU buffer cache interface
│   ├── bitmap.h             # Inode and block allocation bitmaps
│   ├── inode.h              # Inode allocation, bmap, truncate, read/write
│   ├── dir.h                # Directory parsing and path traversal
│   └── fs_ops.h             # FUSE 3 callback interfaces
├── src/                     # C11 implementation files
│   ├── main.c               # FUSE driver daemon entry point
│   ├── mkfs.c               # Disk image formatting utility (mkfs.fusefs)
│   ├── fsck.c               # Filesystem consistency checker (fsck.fusefs)
│   ├── storage.c            # Device layer and cache integration
│   ├── cache.c              # LRU buffer cache implementation
│   ├── bitmap.c             # Bitmap management logic
│   ├── inode.c              # Multi-level bmap and file I/O
│   ├── dir.c                # Directory entry management
│   └── fs_ops.c             # FUSE 3 operation implementations
├── tests/                   # Automated validation suites
│   ├── test_suite.sh        # Core operations & persistence test
│   ├── large_file_test.sh   # Single & double indirect blocks test
│   ├── bench_cache.sh       # Cache throughput benchmark (with/without cache)
│   └── concurrency_test.sh  # Multi-process concurrent stress test
├── scripts/
│   ├── setup_ubuntu.sh      # One-click Ubuntu environment preparation
│   └── fusefs-version       # Snapshot/list/restore utility
```

---

## How to Run on Ubuntu (Step-by-Step)

### Step 1: Install Dependencies
Run the included setup script or install via `apt`:
```bash
chmod +x scripts/setup_ubuntu.sh tests/*.sh
./scripts/setup_ubuntu.sh
```
*Or manually:*
```bash
sudo apt update
sudo apt install -y build-essential gcc make pkg-config libfuse3-dev fuse3
```

*(Note for WSL2 / Docker users: ensure `/dev/fuse` is accessible: `sudo chmod 666 /dev/fuse`)*

---

### Step 2: Build the Project
Compile the filesystem daemon, formatter, and consistency checker:
```bash
make
```
This produces the following commands in `bin/`:
- `bin/fusefs` — The FUSE mount daemon
- `bin/mkfs.fusefs` — The disk formatter
- `bin/fsck.fusefs` — The consistency checker
- `bin/fusefs-version` — Manual file version-history utility

---

### Step 3: Format a Disk Image
Format a 64 MB disk image:
```bash
./bin/mkfs.fusefs my_fs.img 64
```
You will see output displaying the initialized superblock, inode table, and bitmap blocks.

---

### Step 4: Mount the Filesystem
Create a mount directory and mount the filesystem:
```bash
mkdir -p mnt
./bin/fusefs my_fs.img mnt
```
To run with options:
- Run in foreground: `./bin/fusefs -f my_fs.img mnt`
- Run with debug output: `./bin/fusefs -d my_fs.img mnt`
- Disable buffer cache: `./bin/fusefs --no-cache my_fs.img mnt`

---

### Step 5: Test Filesystem Operations
You can now use standard Linux shell commands directly against the mount point:
```bash
# Create and write to a file
echo "Hello, FuseFS!" > mnt/hello.txt
cat mnt/hello.txt

# Append to a file
echo "Appending second line" >> mnt/hello.txt
cat mnt/hello.txt

# Create subdirectories and nested files
mkdir -p mnt/docs/projects
echo "Nested file contents" > mnt/docs/projects/report.txt
ls -la mnt/docs/projects

# Create hard links
ln mnt/hello.txt mnt/hello_hardlink.txt
stat -c "%h" mnt/hello.txt   # Outputs 2

# Create symbolic links
ln -s "hello.txt" mnt/hello_symlink.txt
cat mnt/hello_symlink.txt

# Truncate a file
truncate -s 5 mnt/hello.txt
stat -c "%s" mnt/hello.txt   # Outputs 5

# Save a version before making an important change
./bin/fusefs-version snapshot mnt mnt/hello.txt
echo "New contents" > mnt/hello.txt

# List saved versions and restore one by its ID
./bin/fusefs-version list mnt mnt/hello.txt
./bin/fusefs-version restore mnt mnt/hello.txt VERSION_ID
```

Snapshots are **explicit/manual**, not automatically made on every write. They are stored inside the mounted volume in the reserved hidden directory `.fusefs_versions`, so they consume FuseFS space and are included in the disk image. Keep the filesystem mounted while running the command. The restore operation atomically replaces the selected file; take a snapshot first if you may want to keep its current contents.

---

### Step 6: Unmount Cleanly (Persistence Verification)
Unmount using `fusermount3`:
```bash
fusermount3 -u mnt
```
*(Or `sudo umount mnt` if required)*

**Verify persistence:**
Remount the image and verify that all files survived:
```bash
./bin/fusefs my_fs.img mnt
cat mnt/docs/projects/report.txt
fusermount3 -u mnt
```

---

### Step 7: Check Consistency with FSCK
Run the consistency checker to inspect disk structures and ensure zero leaked blocks:
```bash
./bin/fsck.fusefs my_fs.img
```
If errors ever occur, repair them with:
```bash
./bin/fsck.fusefs --repair my_fs.img
```

---

## Running the Automated Test Batteries

### 1. Full Functional & Persistence Test Battery
Tests file creation, directory tree walking, redirection (`>`, `>>`), hard links, symlinks, truncate, and validates that all files persist across unmount/remount:
```bash
make test
# Or:
bash tests/test_suite.sh
```

### 2. Large-File & Double-Indirect Test
Tests allocating files exceeding direct block limits (48 KB), single-indirect limits (2 MB), and up to double-indirect limits (> 4 MB) with SHA256 checksum verification:
```bash
bash tests/large_file_test.sh
```

### 3. Buffer Cache Throughput Benchmark
Measures and prints sequential read and write speeds with cache **ON** vs **OFF** (`--no-cache`):
```bash
bash tests/bench_cache.sh
```

### 4. Multi-Process Concurrency Stress Test
Launches multiple parallel background workers performing rapid concurrent creation, reading, and appending:
```bash
bash tests/concurrency_test.sh
```

### 5. File Version History Utility Test
Checks snapshot creation, listing, restoration, and rejection of paths outside the selected mount point:
```bash
make
bash tests/version_history_test.sh
```

---

## Grading Rubric & Acceptance Checklist

- [x] **Files and directories persist across remount**: Validated in `tests/test_suite.sh`.
- [x] **Files exceeding direct-block limit read and write correctly**: Multi-level `bmap` handles single and double indirect blocks in `src/inode.c`, verified in `tests/large_file_test.sh`.
- [x] **Unlink and truncate free blocks with no leaks**: Full tree deallocation logic in `inode_truncate()`, verified with `fsck.fusefs`.
- [x] **Concurrent access without metadata corruption**: Recursive mutex synchronization in `src/storage.c`, verified in `tests/concurrency_test.sh`.
- [x] **Measured cache win**: LRU write-back cache in `src/cache.c`, verified via `tests/bench_cache.sh`.
- [x] **Design document**: Detailed architectural breakdown in `DESIGN.md`.
