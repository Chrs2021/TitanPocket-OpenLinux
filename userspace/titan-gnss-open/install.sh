#!/bin/bash
# Install titan-gnss-open on the phone over SSH (run on the Mac from the
# project root; build titan-gps-probe first: userspace/titan-gps/build.sh):
#   userspace/titan-gnss-open/install.sh [user@host]
# (over Wi-Fi: SSH_OPTS="-o HostKeyAlias=172.16.42.1" ... root@<address>)
# The daemon and its Python modules go to /usr/local/lib/titan-gnss-open,
# the probe to /usr/local/bin, the unit to /etc/systemd/system (not
# enabled). The DSP firmware patch is extracted once from the phone's own
# libmnl.so into /lib/firmware/titan/ (never into the repository); after
# that the daemon needs no part of libmnl. Its state,
# /var/lib/titan-gnss-open, is made on the phone; before the first fix it
# needs `position`: "lat lon" within some tens of km. It says where the
# phone is: never in the repository.
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST=${1:-root@172.16.42.1}
O=${SSH_OPTS:-}
L=/usr/local/lib/titan-gnss-open
ssh $O "$HOST" "mkdir -p $L /var/lib/titan-gnss-open"
scp -q $O userspace/titan-gnss-open/titan-gnss-open tools/gnss/gpseph.py tools/gnss/snapfix.py \
	tools/gnss/own02.py tools/gnss/meas-match.py tools/gnss/stpgps-decode.py tools/gnss/navsub.py tools/gnss/session.py "$HOST:$L/"
scp -q $O out/titan-gps-probe "$HOST:/usr/local/bin/titan-gps-probe"
scp -q $O userspace/titan-gnss-open/titan-gnss-open.service "$HOST:/etc/systemd/system/"
ssh $O "$HOST" "chmod 755 $L/titan-gnss-open /usr/local/bin/titan-gps-probe && systemctl daemon-reload && \
	{ test -s /lib/firmware/titan/mt6631-gnss-patch.bin || /usr/local/bin/titan-gps-probe --extract-patch /lib/firmware/titan/mt6631-gnss-patch.bin; }"
echo "installed; state in /var/lib/titan-gnss-open: $(ssh $O "$HOST" 'ls /var/lib/titan-gnss-open | tr "\n" " "')"
