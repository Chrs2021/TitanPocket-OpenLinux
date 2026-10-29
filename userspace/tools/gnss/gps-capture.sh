#!/bin/sh
# gps-capture.sh [SECONDS]: on the phone, capture titan-mnld <-> /dev/stpgps
# traffic from the very start of a session, and gpsd's JSON alongside.
#
#   scp tools/gnss/gps-capture.sh root@PHONE:/root/ && ssh root@PHONE /root/gps-capture.sh 100
#   -> /tmp/gps2.trace (for stpgps-decode.py, stpgps-script.py), /tmp/gps2.json
#
# Location must be off first. strace attaches to titan-mnld the moment it
# exists, before it opens the device, so the session header and the
# firmware download are in the capture. The JSON holds the fix: keep both
# local.
T=${1:-100}
rm -f /tmp/gps2.trace /tmp/gps2.json
titan-location on >/tmp/gps2.on.log 2>&1 &
i=0
while [ $i -lt 2000 ]; do P=$(pgrep -x titan-mnld) && break; i=$((i+1)); done
echo "titan-mnld pid $P after $i polls"
strace -f -ttt -s 65535 -xx -e trace=openat,close,read,write,ioctl -p $P -o /tmp/gps2.trace &
S=$!
( sleep 3; timeout $T gpspipe -w > /tmp/gps2.json 2>/dev/null ) &
sleep $T
kill $S
titan-location off
wc -l /tmp/gps2.trace /tmp/gps2.json
