#!/bin/sh
# mdmock.sh SECONDS: the modem against mdd.py with a scratch file root
# (tmpfs: empty at each run, kept with KEEP=1, or a fresh copy of the
# phone's NVRAM with NV=real; the partitions are never written).
# Watchdog pinger as md0.sh.
rm -f /run/wdstop /root/mdmock.log /root/mdmock.done
setsid sh -c "exec 3>/dev/watchdog; while [ ! -e /run/wdstop ]; do printf k >&3; sleep 5; done; printf V >&3" </dev/null >/dev/null 2>&1 &
# NV=real: start from a copy of the phone's NVRAM (nvcopy.sh) instead of
# an empty root; KEEP=1: keep the root from the last run
if [ "$NV" = real ]; then
	sh /root/nvcopy.sh /run/titan-modem-mock
	export NV_QUIET=1
elif [ -z "$KEEP" ]; then
	rm -rf /run/titan-modem-mock
fi
mkdir -p /run/titan-modem-mock/Z
sleep 1; sync
modprobe wwan		# insmod does not load the WWAN core
insmod /root/titan_modem.ko $MDARGS
FS_ROOT=/run/titan-modem-mock python3 /root/mdd.py ${1:-15} > /root/mdmock.log 2>&1
dmesg | grep -i "titan-modem" | tail -20 > /root/mdmock.dmesg
# the exception overview (non-cached smem + 2 KB) before the modem goes off
dd if=/sys/kernel/debug/titan-modem/smem of=/root/mdmock-ee.bin bs=2048 skip=1 count=2 2>/dev/null
python3 /root/rings.py > /root/mdmock.rings 2>&1
# the modem's own log: the cached region (CCB users, DHL) and its control
cp /sys/kernel/debug/titan-modem/ccb /root/mdmock-ccb.bin 2>/dev/null
dd if=/sys/kernel/debug/titan-modem/smem of=/root/mdmock-ccbctl.bin bs=1024 skip=96 count=4 2>/dev/null
cp /sys/kernel/debug/titan-modem/smem /root/mdmock-smem.bin
cp /sys/kernel/debug/titan-modem/sram /root/mdmock-sram.bin
rmmod titan_modem
touch /run/wdstop
echo done > /root/mdmock.done
