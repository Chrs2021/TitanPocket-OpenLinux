#!/bin/bash
# Build the modem driver for Linux 6.12 (linux612/titan_modem.ko), out of
# tree against the 6.12 build, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/modem/build612.sh
# Output: modules/modem/out612/titan_modem.ko
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=/root/titan-modem-612
KDIR=/workspace/kernel_vol_mnt/mainline/linux-6.12.115
O=/root/titan-6.12-build
export ARCH=arm64

rm -rf "$SRC"; mkdir -p "$SRC"
tar -C "$HERE" --exclude=out612 --exclude='*.o' --exclude='*.ko' --exclude='.*.cmd' \
	--exclude='*.mod' --exclude='*.mod.c' --exclude=Module.symvers --exclude=modules.order \
	-cf - linux612 | tar -C "$SRC" -xf -
make -C "$KDIR" O="$O" M="$SRC/linux612" -j"$(nproc)" W=1 modules
mkdir -p "$HERE/out612"
cp "$SRC/linux612/titan_modem.ko" "$HERE/out612/"
# the WWAN core it needs (CONFIG_WWAN=m), for the phone's module tree
cp "$O/drivers/net/wwan/wwan.ko" "$HERE/out612/"
ls -l "$HERE/out612/titan_modem.ko"
