#!/bin/bash
# Build the MediaTek connectivity modules (WMT core, Wi-Fi char device,
# gen3 Wi-Fi driver, Bluetooth char device, FM radio) out of tree against
# our kernel. Runs in the build
# container:
#   container exec titan_build_ubuntu bash /workspace/modules/connectivity/build.sh [clean]
# Output: modules/connectivity/out/*.ko
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=${KDIR:-$(cd "$HERE/../../kernel_vol_mnt/kernel_workspace" && pwd)}
JOBS=${JOBS:-$(nproc)}

# The vendor Makefiles include each other's headers via
# $(TOP)/vendor/mediatek/kernel_modules/connectivity/...
TOP=/tmp/titan-conn-top
mkdir -p $TOP/vendor/mediatek/kernel_modules
ln -sfn "$HERE" $TOP/vendor/mediatek/kernel_modules/connectivity

COMMON=(MTK_CONSYS_ADIE=MT6631 TOP=$TOP)

mk() {
	local dir=$1; shift
	make -C "$KDIR" M="$HERE/$dir" -j"$JOBS" "${COMMON[@]}" "$@"
}

if [ "${1:-}" = clean ]; then
	for d in common wlan_adaptor wlan_gen3 bt/legacy fm; do mk $d clean; done
	rm -rf "$HERE/out"
	exit 0
fi

mk common modules
mk wlan_adaptor KBUILD_EXTRA_SYMBOLS="$HERE/common/Module.symvers" modules
mk wlan_gen3 KBUILD_EXTRA_SYMBOLS="$HERE/common/Module.symvers $HERE/wlan_adaptor/Module.symvers" \
	modules

# Bluetooth: /dev/stpbt (raw HCI over WMT/STP), FM: /dev/fm (MT6631)
mk bt/legacy KBUILD_EXTRA_SYMBOLS="$HERE/common/Module.symvers" modules
mk fm KBUILD_EXTRA_SYMBOLS="$HERE/common/Module.symvers" CFG_FM_CHIP=mt6631 modules

mkdir -p "$HERE/out"
cp "$HERE/common/wmt_drv.ko" "$HERE/wlan_adaptor/wmt_chrdev_wifi.ko" "$HERE/wlan_gen3/wlan_drv_gen3.ko" \
   "$HERE/bt/legacy/bt_drv.ko" "$HERE/fm/fmradio_drv.ko" "$HERE/out/"
for m in "$HERE"/out/*.ko; do
	echo "$(basename "$m"): $(modinfo -F vermagic "$m")"
done
