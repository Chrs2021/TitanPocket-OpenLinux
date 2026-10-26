#!/bin/sh
# tools/power/drainlog.sh [SECONDS]: run on the phone. Every 5 s while the
# phone is awake: time, charge (mAh, the OZ8806 coulomb counter, which keeps
# counting while the SoC sleeps), current, voltage, plugged, screen. The
# counter's difference over a stretch is the real average current, sleep
# included. Output: /root/drain.csv
G=/sys/class/power_supply/oz8806-battery
END=$(( $(date +%s) + ${1:-3600} ))
echo "t,charge_mAh,current_mA,mV,plugged,screen" > /root/drain.csv
while [ $(date +%s) -lt $END ]; do
	s=on; [ -e /run/titan-screen.off ] && s=off
	echo "$(date +%s),$(( $(cat $G/charge_now) / 1000 )),$(( $(cat $G/current_now) / 1000 )),$(( $(cat $G/voltage_now) / 1000 )),$(cat /sys/class/power_supply/usb/online),$s" >> /root/drain.csv
	sleep 5
done
