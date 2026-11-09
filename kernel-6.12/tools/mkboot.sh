#!/bin/bash
# Pack a 6.12 boot image for the Titan Pocket (docs/mainline.md). Run on the
# Mac from the project root after tools/mainline/build.sh:
#   tools/mainline/mkboot.sh        # -> out/mainline/boot-6.12.img
#
# Repacked with magiskboot from the boot image that was on the phone, like
# ./mkboot.sh, so the v2 header and cmdline stay as LK expects; the kernel
# is our 6.12 Image.gz, the header's dtb is ours, the ramdisk is the
# first-light one (busybox, reboot-bootloader, udhcpd.conf and
# tools/mainline/firstlight-init), and the cmdline gets the bring-up
# options below.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$ROOT/out/mainline
BASE_IMG=$ROOT/backups/boot_device_magisk_20261010.img

rm -rf "$OUT/rd" "$OUT/repack"
mkdir -p "$OUT/rd/bin" "$OUT/rd/etc" "$OUT/rd/proc" "$OUT/rd/sys" "$OUT/rd/dev" "$OUT/rd/tmp" "$OUT/repack"
cp userspace/build/bin/busybox userspace/build/bin/reboot-bootloader "$OUT/rd/bin/"
cp userspace/rootfs/etc/udhcpd.conf "$OUT/rd/etc/"
cp tools/mainline/firstlight-init "$OUT/rd/init"
# the 6.12 connectivity modules, when built (modules/connectivity/build612.sh)
if ls modules/connectivity/out612/*.ko >/dev/null 2>&1; then
	mkdir -p "$OUT/rd/lib/modules-conn"
	cp modules/connectivity/out612/*.ko "$OUT/rd/lib/modules-conn/"
fi
chmod 755 "$OUT/rd/init" "$OUT/rd/bin/"*
# The container can see a stale, empty rd/ for a few seconds after it was
# recreated here (an image without /init panics in a boot loop), so the
# archive is checked and made again until it has /init and busybox.
rd_ok() {
	cpio -it < "$OUT/ramdisk.cpio" 2>/dev/null | grep -qxE "(\./)?init" &&
		cpio -it < "$OUT/ramdisk.cpio" 2>/dev/null | grep -qxE "(\./)?bin/busybox"
}
for try in 1 2 3 4 5 6; do
	container exec titan_build_ubuntu bash -c \
		'cd /workspace/out/mainline/rd && find . | cpio -o -H newc -R 0:0 --quiet > ../ramdisk.cpio'
	rd_ok && break
	[ $try = 6 ] && { echo "ramdisk.cpio lacks init or bin/busybox" >&2; exit 1; }
	sleep 2
done

cd "$OUT/repack"
"$ROOT/magiskboot" unpack -h "$BASE_IMG" >/dev/null 2>&1
# Bring-up cmdline, appended to the stock one LK expects:
# - clk_ignore_unused, pd_ignore_unused, regulator_ignore_unused: Linux
#   must not switch off the clocks, power domains and PMIC supplies LK
#   left on (display, UFS);
#   IGNORE_UNUSED lists which of the three to pass (default all:
#   IGNORE_UNUSED="clk pd regulator"; "" passes none);
# - ignore_loglevel: everything reaches the console, so pstore keeps it all;
# - DEBIAN=1 adds titan.root (firstlight-init boots Debian from titanroot)
#   and the sleep Debian's power key gets when it writes "mem": deep sleep
#   through the SPM firmware (mt6771_spm.deep=1 mem_sleep_default=deep),
#   with the SPM's default wake sources less the SCP's (bit 11,
#   mt6771_spm.wake_mask=0xe87d5ef: the sensor hub's SCP stays awake and
#   its request would end every suspend at once; the modem's lines stay),
#   the SCP's own sleep on (mt6771_scp.sleep=1) and the A73 buck cut in
#   sleep (mt6771_spm.pcm_flags1=0x300, BIG_BUCK_OFF/ON): together 35 -> 23
#   mA asleep with the modem stopped (2026-11-05, docs/power.md); any of
#   them given in EXTRA_CMDLINE wins (e.g. mt6771_scp.sleep=0);
#   SLEEP=s2idle gives suspend to idle instead (no SPM firmware), as
#   before 2026-11-05;
# - EXTRA_CMDLINE is appended as is (bisecting, e.g. initcall_blacklist=).
# (dt_idle_states.off=1 would start the device tree's idle states disabled,
# WFI only, for testing new ones from the shell; cpufreq.default_governor=
# userspace keeps the CPU clocks where they are until set from the shell;
# docs/mainline.md.)
flags=
for f in ${IGNORE_UNUSED-clk pd regulator}; do flags="$flags ${f}_ignore_unused"; done
if [ "${DEBIAN:-}" = 1 ]; then
	flags="$flags titan.root"
	case "${SLEEP:-deep}" in
	s2idle) flags="$flags mem_sleep_default=s2idle" ;;
	deep) flags="$flags mem_sleep_default=deep"
	      case " ${EXTRA_CMDLINE:-} " in
	      *" mt6771_spm.deep=1 "*) ;;
	      *) flags="$flags mt6771_spm.deep=1" ;;
	      esac
	      case " ${EXTRA_CMDLINE:-} " in
	      *" mt6771_spm.wake_mask="*) ;;
	      *) flags="$flags mt6771_spm.wake_mask=0xe87d5ef" ;;
	      esac
	      case " ${EXTRA_CMDLINE:-} " in
	      *" mt6771_scp.sleep="*) ;;
	      *) flags="$flags mt6771_scp.sleep=1" ;;
	      esac
	      case " ${EXTRA_CMDLINE:-} " in
	      *" mt6771_spm.pcm_flags1="*) ;;
	      *) flags="$flags mt6771_spm.pcm_flags1=0x300" ;;
	      esac ;;
	*) echo "SLEEP: deep or s2idle" >&2; exit 1 ;;
	esac
fi
sed -i '' "/^cmdline=/s/\$/$flags ignore_loglevel${EXTRA_CMDLINE:+ $EXTRA_CMDLINE}/" header
grep '^cmdline=' header
cp "$OUT/Image.gz" kernel
# LK reads the header's dtb as an Android DT table (magic 0xd7b7ab1e), as
# in the stock image: a 32-byte header, one 32-byte entry, then the FDT.
python3 - "$OUT/mt6771-unihertz-titan-pocket.dtb" dtb <<'PY'
import struct, sys
fdt = open(sys.argv[1], 'rb').read()
hdr = struct.pack('>8I', 0xd7b7ab1e, 0x40 + len(fdt), 0x20, 0x20, 1, 0x20, 2048, 0)
ent = struct.pack('>8I', len(fdt), 0x40, 0, 0, 0, 0, 0, 0)
open(sys.argv[2], 'wb').write(hdr + ent + fdt)
PY
cp "$OUT/ramdisk.cpio" ramdisk.cpio
"$ROOT/magiskboot" repack "$BASE_IMG" "$OUT/boot-6.12.img"
ls -la "$OUT/boot-6.12.img"
