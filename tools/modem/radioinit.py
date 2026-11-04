#!/usr/bin/python3
# The modem's start-up on its radio channel, as Android's RIL: the SIM up and the
# radio off, ready for ModemManager (or AT+EFUN=1). It replaces AT+CFUN=4 as the first
# command: from that state the modem refuses AT+ESIMS and AT+ESADM=3. Runs once the
# mux's ports exist; a command it refuses is logged and the next one still goes.
# AT+CEMODE=2 is ours, not Android's: the modem starts voice-centric, and with no IMS
# registered it leaves LTE 60 s after radio on (measured). It forgets this when restarted.
import os, select, sys, time
PORT, LAST = "/dev/wwan0at8", "/dev/wwan0at23"      # cmd9, the table's last
SEQ = ("ATE0Q0V1", "AT+CMEE=1", "AT+ESIMS=1", "AT+ESIMS", "AT+EGTP=1", "AT+EMPPCH=1",
       "AT+ESADM=3", "AT+EBOOT=0", "AT+EDSBP=1", "AT+EPOC", "AT+EFUN=0", "AT+CEMODE=2")
end = time.time() + 60
while not os.path.exists(LAST):
    if time.time() > end:
        sys.exit("no AT ports")
    time.sleep(0.1)
fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
for c in SEQ:
    os.write(fd, (c + "\r").encode())
    b, end = "", time.time() + 10
    while time.time() < end and not any(x in b for x in ("OK\r\n", "ERROR")):
        if select.select([fd], [], [], 0.2)[0]:
            b += os.read(fd, 4096).decode(errors="replace")
    if "OK\r\n" not in b:
        print("%s: %s" % (c, "refused" if "ERROR" in b else "no answer"), flush=True)
print("radio channel ready", flush=True)
