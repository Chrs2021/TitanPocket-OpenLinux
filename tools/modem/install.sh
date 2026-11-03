#!/bin/bash
# Install the modem service on the phone over SSH (run on the Mac from the
# project root; build the driver first: modules/modem/build612.sh):
#   tools/modem/install.sh [user@host]
# titan_modem.ko goes to the module tree's updates/ with the WWAN core
# (wwan.ko), blacklisted so the DT node does not load it at boot (it must
# start with its file server); mdd.py and nvcopy.sh go to
# /usr/local/lib/titan-modem, the unit to /etc/systemd/system (not enabled).
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST=${1:-root@172.16.42.1}
O=${SSH_OPTS:-}
L=/usr/local/lib/titan-modem
K=$(ssh $O "$HOST" uname -r)
ssh $O "$HOST" "mkdir -p $L /lib/modules/$K/updates /lib/modules/$K/kernel/drivers/net/wwan"
scp -q $O tools/modem/mdd.py tools/modem/nvcopy.sh "$HOST:$L/"
scp -q $O modules/modem/out612/titan_modem.ko "$HOST:/lib/modules/$K/updates/"
scp -q $O modules/modem/out612/wwan.ko "$HOST:/lib/modules/$K/kernel/drivers/net/wwan/"
scp -q $O tools/modem/titan-modem.service "$HOST:/etc/systemd/system/"
ssh $O "$HOST" "echo 'blacklist titan_modem' > /etc/modprobe.d/titan-modem.conf && \
	depmod -a $K && systemctl daemon-reload"
echo installed
