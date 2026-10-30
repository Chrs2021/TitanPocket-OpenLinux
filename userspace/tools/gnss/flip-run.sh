#!/bin/sh
# flip-run.sh BYTE:MASK SECONDS [B2] [ID]: titan-mnld bit-flip experiment
# flip-run.sh ID=BYTE:MASK,... SECONDS [B2]: a different bit for each satellite
# (TITAN_MNLD_FLIP, docs/gnss.md). Run on the phone as /root/flip-run.sh with
# Location off: it sets the switches in a runtime drop-in, runs the receiver
# for SECONDS, removes the drop-in and leaves the measurement log (with its F
# lines) in /tmp/flip-BYTE:MASK.log (/tmp/flip-sats.log with ID=). PERIOD=s
# in the environment sets the on/off window (default 10). The log says where
# the phone was: keep it local.
D=/run/systemd/system/titan-mnld.service.d
mkdir -p $D
{ echo "[Service]"
  case $1 in *=*) F="TITAN_MNLD_FLIP_SATS=$1" ;; *) F="TITAN_MNLD_FLIP=$1" ;; esac
  echo "Environment=TITAN_MNLD_MEAS=/data/vendor/gps/meas.log $F TITAN_MNLD_FLIP_B2=${3:-08} TITAN_MNLD_FLIP_PERIOD=${PERIOD:-10}"
  [ -n "$4" ] && echo "Environment=TITAN_MNLD_FLIP_ID=$4"; } > $D/flip.conf
systemctl daemon-reload
titan-location on >/dev/null 2>&1
sleep $2
titan-location off >/dev/null 2>&1
rm -f $D/flip.conf; systemctl daemon-reload
case $1 in *=*) L=sats ;; *) L=$1 ;; esac
cp /opt/android-gps/data/vendor/gps/meas.log /tmp/flip-$L.log
grep -c "^F" /tmp/flip-$L.log; grep -c "^M" /tmp/flip-$L.log
