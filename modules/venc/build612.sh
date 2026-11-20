#!/bin/bash
# Build titan_venc.ko (docs/video-encoder.md) for the running 6.12 kernel
# without rebuilding it, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/venc/build612.sh
# The source is in the 6.12 tree (drivers/media/platform/mediatek/mt6771-venc,
# CONFIG_VIDEO_MT6771_VENC); this builds it out of tree against the same build.
# Output: modules/venc/out612/titan_venc.ko
set -euo pipefail
# KDIR/KSRC, O and OUT612 may be set to build against another 6.12 tree (another stable release)
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${KSRC:-/workspace/kernel_vol_mnt/mainline/linux-6.12.115}
SRC=/root/titan-venc-612
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$KSRC/drivers/media/platform/mediatek/mt6771-venc/titan_venc.c" "$SRC/"
echo 'obj-m := titan_venc.o' > "$SRC/Kbuild"
# KBUILD_MODPOST_WARN: the build's Module.symvers may lack the media modules'
# symbols (a vmlinux-only build rewrites it); the kernel has no MODVERSIONS, so
# symbols resolve by name at load and depmod on the phone finds the dependencies
make -C "$KSRC" O="${O:-/root/titan-6.12-build}" ARCH=arm64 M="$SRC" KBUILD_MODPOST_WARN=1 -j"$(nproc)" modules
mkdir -p "${OUT612:-$HERE/out612}"
cp "$SRC/titan_venc.ko" "${OUT612:-$HERE/out612}/"
ls -l "${OUT612:-$HERE/out612}/titan_venc.ko"
