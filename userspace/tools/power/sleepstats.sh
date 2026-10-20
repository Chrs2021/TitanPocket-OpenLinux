#!/bin/sh
# tools/power/sleepstats.sh SINCE [UNTIL]: run on the phone; suspend cycles, wake sources, sleep lengths, Wi-Fi drops
S="$1"; U="${2:-now}"
journalctl -k --since "$S" --until "$U" -o short-unix --no-pager > /tmp/ss.k
journalctl -u wpa_supplicant@wlan0 --since "$S" --until "$U" -o cat --no-pager > /tmp/ss.w
echo "suspend entries: $(grep -c 'PM: suspend entry' /tmp/ss.k)   exits: $(grep -c 'PM: suspend exit' /tmp/ss.k)"
echo "wake sources (system sleep):"
grep -v SODI /tmp/ss.k | grep '\[SPM\] wake up by' | sed -E 's/.*wake up by +([A-Z0-9_]+).*/\1/' | sort | uniq -c | sort -rn
echo "time asleep per cycle (s):"
python3 - <<'PY'
import re
ent = None; d = []
for l in open("/tmp/ss.k"):
    f = l.split()
    if not f or not re.match(r"^\d+\.\d+$", f[0]): continue
    t = float(f[0])
    if "PM: suspend entry" in l: ent = t
    elif "PM: suspend exit" in l and ent: d.append(t - ent); ent = None
if d:
    d.sort(); n = len(d)
    print("  n=%d  median %.1f  min %.1f  max %.1f  total %.0f" % (n, d[n//2], d[0], d[-1], sum(d)))
PY
echo "Wi-Fi disconnects: $(grep -c CTRL-EVENT-DISCONNECTED /tmp/ss.w)"
