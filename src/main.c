#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include "fusefs.h"
#include "storage.h"
#include "fs_ops.h"

static void handle_signal(int sig) {
    (void)sig;
    storage_sync();
    storage_close();
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [options] <image_file> <mountpoint>\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  --no-cache        Disable buffer cache (direct image I/O)\n");
        fprintf(stderr, "  --cache-size=N    Set buffer cache capacity in blocks (default: 1024)\n");
        fprintf(stderr, "  -f                Run FUSE in foreground\n");
        fprintf(stderr, "  -d                Enable FUSE debug output\n");
        fprintf(stderr, "  -s                Run in single-threaded mode\n");
        return 1;
    }

    bool cache_enabled = true;
    int cache_size = 1024;
    const char *image_path = NULL;

    int fuse_argc = 0;
    char *fuse_argv[64];
    fuse_argv[fuse_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            cache_enabled = false;
        } else if (strncmp(argv[i], "--cache-size=", 13) == 0) {
            cache_size = atoi(argv[i] + 13);
            if (cache_size <= 0) cache_size = 1024;
        } else if (image_path == NULL && argv[i][0] != '-') {
            image_path = argv[i];
        } else {
            if (fuse_argc < 63) {
                fuse_argv[fuse_argc++] = argv[i];
            }
        }
    }

    if (!image_path) {
        fprintf(stderr, "Error: missing backing image file path.\n");
        return 1;
    }

    /* Signal handlers for clean persistence */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int ret = storage_open(image_path, cache_enabled, cache_size);
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize FuseFS on '%s': error %d\n", image_path, ret);
        return 1;
    }

    printf("[FuseFS] Mounting backing image: %s (Cache: %s, Size: %d blocks)\n",
           image_path, cache_enabled ? "ON" : "OFF", cache_size);

    int fuse_ret = fuse_main(fuse_argc, fuse_argv, &fusefs_oper, NULL);

    storage_close();
    printf("[FuseFS] Filesystem safely unmounted and synced.\n");

    return fuse_ret;
}
