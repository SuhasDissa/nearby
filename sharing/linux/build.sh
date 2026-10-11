#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
bazelisk build //sharing/linux/lib:quickshare
cmake -S sharing/linux/app -B build -G Ninja \
  -DQUICKSHARE_LIB_DIR="$root/bazel-bin/sharing/linux/lib"
cmake --build build
echo "app: $root/build/quickshare"
