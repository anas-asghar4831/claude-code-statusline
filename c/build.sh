#!/bin/bash
# Build the C statusline (max-perf). Requires gcc (MSYS2/mingw-w64 on Windows).
# yyjson.c/.h and uthash.h are vendored alongside this script.
set -e
cd "$(dirname "$0")"
gcc -O3 -march=native -flto -o statusline.exe statusline.c yyjson.c
echo "built: $(pwd)/statusline.exe"
