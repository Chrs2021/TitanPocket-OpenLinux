#!/usr/bin/python3
# Android's AT commands on the mux's WWAN ports, each on its own channel, one at a
# time; registration results only, identities masked. ModemManager stopped.
# usage: atreplay.py SEQFILE [LISTEN_SECONDS]; SEQFILE lines are channel<TAB>command,
# the channel as the RIL's log names it.
import os, re, select, sys, time
# the RIL's names, in titan_modem's at_dlci order: wwan0at0, wwan0at1, ...
NAMES = ("CMD_READER_1", "URC_READER", "CMD_READER_4", "CMD_READER_2", "CMD_READER_3",
         "ATCI_READER", "CMD_RT_7", "CMD_NRT_8", "CMD_RADIO_9", "CMD_DATA2_10", "CMD_11",
         "CMD2_READER_1", "URC2_READER", "CMD2_READER_4", "CMD2_READER_2", "CMD2_READER_3",
         "ATCI2_READER", "CMD2_RT_7", "CMD2_NRT_8", "CMD2_RADIO_9", "CMD2_DATA2_10", "CMD2_11",
         "NW_CMD", "NW_URC")
KEEP = ("+CEREG", "+CSCON", "+EIAREG", "+CGEV", "+EIND", "+CIREG", "+EREG: ", "+COPS:", "+CGATT:",
        "+CESQ:", "+ESBPID", "+ESIMS", "+EUSIM", "+CEER", "+ECCAUSE", "+PSBEARER", "+EPDN", "+CEN1")
TAGS = os.environ.get("TAGS") == "1"
t0 = time.time()
fds, names, txt, last = {}, {}, {}, {}
for i, n in enumerate(NAMES):
    fd = os.open("/dev/wwan0at%d" % i, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    fds[n] = fd; names[fd] = n; txt[fd] = ""
def log(s):
    print("%6.1f %s" % (time.time() - t0, s), flush=True)
def show(l):
    return re.sub(r'"[^"]*"', '"*"', re.sub(r"\d{7,}", "<n>", l))
def pump(secs, stop=None):
    end = time.time() + secs
    while time.time() < end:
        if stop and stop():
            return
        for fd in select.select(list(names), [], [], 0.05)[0]:
            txt[fd] += os.read(fd, 4096).decode(errors="replace")
            while "\r\n" in txt[fd]:
                l, txt[fd] = txt[fd].split("\r\n", 1)
                l = l.strip()
                if l in ("OK", "ERROR") or l.startswith("+CME ERROR"):
                    last[fd] = l
                elif l.startswith(KEEP):
                    log("[%s] %s" % (names[fd], show(l)[:100]))
                elif TAGS and "URC" in names[fd] and l.startswith("+"):
                    # the others by name only: their arguments can be identities
                    log("[%s] %s" % (names[fd], re.split(r"[:=]", l)[0]))
def at(name, c, t=5):
    fd = fds[name]
    last.pop(fd, None)
    os.write(fd, (c + "\r").encode())
    pump(t, lambda: fd in last)
    return last.get(fd)
bad = []; sent = 0
for line in open(sys.argv[1]).read().splitlines():
    n, c = line.split("\t", 1)
    r = at(n, c); sent += 1
    if c.startswith(("AT+EFUN=", "AT+ESADM", "AT+EDSBP", "AT+EBOOT", "AT+ESIMS", "AT+EAPN", "AT+EIAAPN", "AT+EDALLOW", "AT+ERAT")):
        log("%s %s -> %s" % (n, c[:48], r))
    if r != "OK":
        bad.append("%s %s:%s" % (n, c[:22], (r or "none")[-6:]))
log("sequence done, %d sent; not OK %d; last: %s" % (sent, len(bad), " | ".join(bad[-2:])))
listen = int(sys.argv[2]) if len(sys.argv) > 2 else 60
for c in ("AT+CSCON=1", "AT+CEREG=3"):
    log("%s -> %s" % (c, at("CMD_READER_1", c)))
for i in range(listen // 5):
    pump(5)
    if i % 3 == 2:
        at("CMD_READER_1", "AT+CEREG?", 1)
for c in ("AT+CSCON?", "AT+COPS?", "AT+CGATT?", "AT+CESQ", "AT+CEER"):
    at("CMD_READER_1", c, 2)
print("end", flush=True)
