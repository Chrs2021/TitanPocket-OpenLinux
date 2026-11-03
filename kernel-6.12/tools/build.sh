#!/bin/bash
# Build the Linux 6.12 kernel for the Titan Pocket inside the
# titan_build_ubuntu container (docs/mainline.md):
#   container exec titan_build_ubuntu bash /workspace/tools/mainline/build.sh [config|build|modules|all]
# Source: kernel_vol_mnt/mainline/linux-6.12.115 (branch titanpocket-6.12).
# Objects go to the container's own disk: on the shared /workspace mount
# `sed -i` fails (merge_config.sh needs it) and builds are slower. Image.gz
# and the dtb are copied to out/mainline/. "modules" builds the kernel's
# modules and installs them (stripped) under $O/staging, for the out-of-tree
# drivers that use them (the camera's media core); nothing copies the whole
# set to the phone.
set -euo pipefail

KSRC=/workspace/kernel_vol_mnt/mainline/linux-6.12.115
O=/root/titan-6.12-build
DEST=/workspace/out/mainline
LOG=$DEST/build.log
DTB=mediatek/mt6771-unihertz-titan-pocket.dtb
export ARCH=arm64
mkdir -p "$O" "$DEST"
cd "$O"

do_config() {
	make -C "$KSRC" O="$O" defconfig titanpocket.config
	# report fragment options Kconfig dropped (unmet dependencies)
	grep -E '^CONFIG_[A-Z0-9_]+=' "$KSRC/arch/arm64/configs/titanpocket.config" |
	while IFS= read -r line; do
		grep -qxF "$line" "$O/.config" || echo "WARN: not applied: $line"
	done
}

do_build() {
	make -C "$KSRC" O="$O" -j"$(nproc)" Image.gz "$DTB"
	cp "$O/arch/arm64/boot/Image.gz" "$O/arch/arm64/boot/dts/$DTB" "$O/.config" "$DEST/"
	ls -la "$DEST"
	# grep -m1 closes the pipe early: SIGPIPE, not a failure
	strings "$O/vmlinux" | grep -m1 'Linux version' || true
}

do_modules() {
	make -C "$KSRC" O="$O" -j"$(nproc)" modules
	rm -rf "$O/staging"
	make -C "$KSRC" O="$O" INSTALL_MOD_PATH="$O/staging" INSTALL_MOD_STRIP=1 modules_install
	find "$O/staging/lib/modules" -name '*.ko' | wc -l
}

case "${1:-all}" in
	config)  do_config ;;
	build)   do_build 2>&1 | tee "$LOG" ;;
	modules) do_modules 2>&1 | tee "$DEST/modules.log" ;;
	all)     { do_config; do_build; do_modules; } 2>&1 | tee "$LOG" ;;
esac
