#!/usr/bin/env python3
"""
stpgps-script.py: turn a capture of libmnl's /dev/stpgps traffic into a
replay script for titan-gps-probe (userspace/titan-gps).

    tools/gnss/stpgps-script.py gps2.trace > session.script
    tools/gnss/stpgps-script.py gps2.trace --seconds 30 > session.script
    tools/gnss/stpgps-script.py gps2.trace --fd 5 > session.script   # strace attached late

Capture as in stpgps-decode.py (strace -f -ttt -s 65535 -xx ...), attached
before titan-mnld opens the device (/root/gps-capture.sh on the phone does
this). One line per frame libmnl wrote, in order:

    <ms since the first write> <type, 2 hex digits> <payload hex>

The payload is what follows [type] FE, without the checksum; the probe adds
FE, the checksum, the escapes and the AA F0 / AA 0F framing. A frame that
libmnl wrote in the same write() as the line before has its time marked
"+" (`+518 02 ...`): the probe writes them together too. libmnl batches each
moment's frames (at most 1424 bytes a write); one frame per write fills the
STP link's window and the driver answers ENOSPC. The firmware
patch download (the type 0C chunks) becomes one line:

    <ms> PATCH

and the probe sends the patch from the phone's own libmnl.so: the image is
MediaTek's and is never written into a script. Scripts hold libmnl's
session frames (settings, clock, satellite search assignments), which say
where and when they were captured: keep them local, like the captures.
"""
import importlib.util
import os
import sys


def load_decoder():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("stpgps_decode",
                                                  os.path.join(here, "stpgps-decode.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seconds = None
    if "--seconds" in sys.argv:
        seconds = float(sys.argv[sys.argv.index("--seconds") + 1])
    fd = int(sys.argv[sys.argv.index("--fd") + 1]) if "--fd" in sys.argv else None
    dec = load_decoder()
    t0 = None
    patching = False
    for t, op, data in dec.messages(sys.argv[1], fd):
        if op != "write":
            continue
        t = float(t)
        if t0 is None:
            t0 = t
        ms = round((t - t0) * 1000)
        if seconds is not None and ms > seconds * 1000:
            break
        first = True
        for head, body in dec.frames(data):
            kind = body[0]
            if kind == 0x0C:                    # a firmware chunk
                if not patching:
                    print("%d PATCH" % ms)
                    patching = True
                continue
            patching = False
            print("%s%d %02x %s" % ("" if first else "+", ms, kind, body[2:-2].hex()))
            first = False


if __name__ == "__main__":
    main()
