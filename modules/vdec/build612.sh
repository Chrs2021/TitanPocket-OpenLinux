#!/bin/bash
# Build titan_vdec.ko (docs/video-decoder.md) for the running 6.12 kernel
# without rebuilding it, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/vdec/build612.sh
# The source is in the 6.12 tree (drivers/media/platform/mediatek/mt6771-vdec,
# CONFIG_VIDEO_MT6771_VDEC); this builds it out of tree against the same build.
# Output: modules/vdec/out612/titan_vdec.ko
set -euo pipefail
# KDIR/KSRC, O and OUT612 may be set to build against another 6.12 tree (another stable release)
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${KSRC:-/workspace/kernel_vol_mnt/mainline/linux-6.12.115}
SRC=/root/titan-vdec-612
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$KSRC/drivers/media/platform/mediatek/mt6771-vdec/titan_vdec.c" "$SRC/"
echo 'obj-m := titan_vdec.o' > "$SRC/Kbuild"
# KBUILD_MODPOST_WARN: the build's Module.symvers may lack the media modules'
# symbols (as modules/venc/build612.sh); no MODVERSIONS, they resolve at load
make -C "$KSRC" O="${O:-/root/titan-6.12-build}" ARCH=arm64 M="$SRC" KBUILD_MODPOST_WARN=1 -j"$(nproc)" modules
mkdir -p "${OUT612:-$HERE/out612}"
cp "$SRC/titan_vdec.ko" "${OUT612:-$HERE/out612}/"
ls -l "${OUT612:-$HERE/out612}/titan_vdec.ko"
