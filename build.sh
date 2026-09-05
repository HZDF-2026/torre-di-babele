#!/bin/sh
# build.sh — POSIX build (Linux ARM64 / any g++ >= 9). No cross toolchain needed.
set -e
cd "$(dirname "$0")"
make all
echo "built dist/greenroom"
