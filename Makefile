CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra -O2 -g -D_FILE_OFFSET_BITS=64 -Iinclude
FUSE_CFLAGS := $(shell pkg-config fuse3 --cflags 2>/dev/null || echo -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3)
FUSE_LIBS := $(shell pkg-config fuse3 --libs 2>/dev/null || echo -lfuse3) -lpthread

BIN_DIR := bin
SRC_DIR := src
INC_DIR := include

COMMON_SRCS := src/storage.c src/cache.c src/bitmap.c src/inode.c src/dir.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

FUSE_SRCS := src/main.c src/fs_ops.c $(COMMON_SRCS)
FUSE_OBJS := $(FUSE_SRCS:.c=.o)

MKFS_SRCS := src/mkfs.c
MKFS_OBJS := $(MKFS_SRCS:.c=.o)

FSCK_SRCS := src/fsck.c
FSCK_OBJS := $(FSCK_SRCS:.c=.o)

TARGETS := $(BIN_DIR)/fusefs $(BIN_DIR)/mkfs.fusefs $(BIN_DIR)/fsck.fusefs $(BIN_DIR)/fusefs-version

.PHONY: all clean test directories

all: directories $(TARGETS)

directories:
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/fusefs: $(FUSE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(FUSE_LIBS)

$(BIN_DIR)/mkfs.fusefs: $(MKFS_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/fsck.fusefs: $(FSCK_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/fusefs-version: scripts/fusefs-version | directories
	cp $< $@
	chmod +x $@

src/fs_ops.o: src/fs_ops.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c $< -o $@

src/main.o: src/main.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BIN_DIR) src/*.o *.img mnt

test: all
	@chmod +x tests/*.sh
	@bash tests/test_suite.sh
