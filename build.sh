#!/usr/bin/env bash
# Cross-compile chusanwide.dll for Windows from Linux.
#   sudo apt install mingw-w64 cmake git
set -euo pipefail
cd "$(dirname "$0")"

cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw-i686.cmake" "$@"
cmake --build build -j"$(nproc)"
i686-w64-mingw32-strip --strip-unneeded build/chusanwide.dll

echo
echo "Built: build/chusanwide.dll"
