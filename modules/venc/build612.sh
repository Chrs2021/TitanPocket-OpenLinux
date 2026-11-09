#!/bin/bash
# Build titan_venc.ko for Linux 6.12, out of tree, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/venc/build612.sh
# Output: modules/venc/out612/titan_venc.ko
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=/root/titan-venc-612
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$HERE/titan_venc.c" "$HERE/Kbuild" "$SRC/"
make -C /workspace/kernel_vol_mnt/mainline/linux-6.12.115 O=/root/titan-6.12-build ARCH=arm64 \
	M="$SRC" -j"$(nproc)" modules
mkdir -p "$HERE/out612"
cp "$SRC/titan_venc.ko" "$HERE/out612/"
ls -l "$HERE/out612/titan_venc.ko"
