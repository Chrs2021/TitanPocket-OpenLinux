#!/bin/bash
# Build the NFC driver for Linux 6.12 (linux612/st21nfc.ko), out of
# tree against the 6.12 build, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/nfc/build612.sh
# Output: modules/nfc/out612/st21nfc.ko
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=/root/titan-nfc-612
KDIR=/workspace/kernel_vol_mnt/mainline/linux-6.12.115
O=/root/titan-6.12-build
export ARCH=arm64

rm -rf "$SRC"; mkdir -p "$SRC"
tar -C "$HERE" --exclude=out612 --exclude='*.o' --exclude='*.ko' --exclude='.*.cmd' \
	--exclude='*.mod' --exclude='*.mod.c' --exclude=Module.symvers --exclude=modules.order \
	-cf - linux612 | tar -C "$SRC" -xf -
make -C "$KDIR" O="$O" M="$SRC/linux612" -j"$(nproc)" W=1 modules
mkdir -p "$HERE/out612"
cp "$SRC/linux612/st21nfc.ko" "$HERE/out612/"
ls -l "$HERE/out612/st21nfc.ko"
