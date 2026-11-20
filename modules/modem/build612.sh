#!/bin/bash
# Build the modem driver for Linux 6.12 from the kernel tree
# (drivers/net/wwan/mt6771/titan_modem.c, CONFIG_MTK_MT6771_MODEM=m) with the
# WWAN core, against the 6.12 build, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/modem/build612.sh
# Output: modules/modem/out612/titan_modem.ko and wwan.ko, for tools/modem/install.sh.
# The driver needs the kernel's MD1 power domain (mt6771-power-controller) and the
# DT that goes with it: flash a kernel built from the same tree.
set -euo pipefail
# KDIR/KSRC, O and OUT612 may be set to build against another 6.12 tree (another stable release)
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=${KDIR:-/workspace/kernel_vol_mnt/mainline/linux-6.12.118}
O=${O:-/root/titan-6.12.118-build}
export ARCH=arm64

grep -q '^CONFIG_MTK_MT6771_MODEM=m' "$O/.config" || {
	"$KDIR/scripts/config" --file "$O/.config" -m MTK_MT6771_MODEM
	make -C "$KDIR" O="$O" -s olddefconfig
}
make -C "$KDIR" O="$O" -j"$(nproc)" W=1 drivers/net/wwan/wwan.ko \
	drivers/net/wwan/mt6771/titan_modem.ko
mkdir -p "${OUT612:-$HERE/out612}"
cp "$O/drivers/net/wwan/mt6771/titan_modem.ko" "$O/drivers/net/wwan/wwan.ko" "${OUT612:-$HERE/out612}/"
ls -l "${OUT612:-$HERE/out612}/titan_modem.ko"
