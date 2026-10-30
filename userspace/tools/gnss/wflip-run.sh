#!/bin/sh
# wflip-run.sh TYPE:OFFSET:WIDTH:DELTA SECONDS [SV] [CON]: titan-mnld host-side
# experiment (TITAN_MNLD_WFLIP, docs/gnss.md). Run on the phone as
# /root/wflip-run.sh with Location off: it changes that field of libmnl's
# host frames in alternate windows (PERIOD=s, default 10), records the
# /dev/stpgps traffic with strace and libmnl's measurements, and leaves
# /tmp/wflip.trace and /tmp/wflip.log. Both say where the phone was: keep
# them local.
D=/run/systemd/system/titan-mnld.service.d
mkdir -p $D
{ echo "[Service]"
  echo "Environment=TITAN_MNLD_MEAS=/data/vendor/gps/meas.log TITAN_MNLD_WFLIP=$1 TITAN_MNLD_WFLIP_PERIOD=${PERIOD:-10}"
  [ -n "$3" ] && echo "Environment=TITAN_MNLD_WFLIP_SV=$3"
  [ -n "$4" ] && echo "Environment=TITAN_MNLD_WFLIP_CON=$4"; } > $D/wflip.conf
systemctl daemon-reload
rm -f /tmp/wflip.trace
titan-location on >/dev/null 2>&1 &
i=0
while [ $i -lt 2000 ]; do P=$(pgrep -x titan-mnld) && break; i=$((i+1)); done
strace -f -ttt -s 65535 -xx -e trace=openat,close,read,write -p $P -o /tmp/wflip.trace &
S=$!
sleep $2
kill $S
titan-location off >/dev/null 2>&1
rm -f $D/wflip.conf; systemctl daemon-reload
cp /opt/android-gps/data/vendor/gps/meas.log /tmp/wflip.log
grep -c "^W" /tmp/wflip.log; grep -c "^M" /tmp/wflip.log; wc -l < /tmp/wflip.trace
