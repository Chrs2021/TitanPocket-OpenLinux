#!/bin/sh
# own02-run.sh BASE.script BRDC.rnx LAT LON HEIGHT SECONDS LABEL [OFFSET_HZ|stale]:
# run titan-gps-probe on the phone (Location off, titan-gps-driver started)
# with our own GPS 02 assignments, computed by own02.py for the moment the
# probe starts, or with BASE.script as it is ("stale"). Leaves
# /tmp/LABEL.script, the probe's log /tmp/LABEL.log and its summary
# /tmp/LABEL.err (docs/gnss.md). Run from /tmp, with gpseph.py, own02.py and
# titan-gps-probe beside it.
set -e
cd /tmp
START=$(date -u -d @$(( $(date +%s) + 20 )) +%Y-%m-%dT%H:%M:%S)
if [ "$8" = stale ]; then
	cp "$1" "$7.script"
else
	python3 own02.py "$1" "$2" "$3" "$4" "$5" "$START" ${8:--150} > "$7.script"
fi
while [ "$(date -u +%Y-%m-%dT%H:%M:%S)" \< "$START" ]; do sleep 0.2; done
./titan-gps-probe "$7.script" --seconds "$6" --log "$7.log" 2>"$7.err"
