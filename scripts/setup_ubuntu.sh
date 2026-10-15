#!/usr/bin/env bash
# ==============================================================================
# FuseFS Ubuntu Environment Setup Script
# Installs required compilers, build tools, and libfuse3 development headers
# ==============================================================================

set -e

echo "=== Installing FuseFS Dependencies on Ubuntu ==="
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc \
    make \
    pkg-config \
    libfuse3-dev \
    fuse3

echo "=== Ensuring FUSE Permissions ==="
# Enable user mounting permissions if needed
sudo chmod 666 /dev/fuse 2>/dev/null || true

echo "=== Setup Complete! You can now build with 'make' ==="
