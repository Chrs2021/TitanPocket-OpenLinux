#!/usr/bin/env python3
"""
mnl-dbglog.py: decode libmnl's debug log (titan-mnld TITAN_MNLD_DBG).

    tools/gnss/mnl-dbglog.py dbg.log > dbg.txt

libmnl obscures most of its debug lines (FUN_00025aa8, behind
mtk_gps_sys_alps_gps_dbg2file_encrypt_output): each printable byte is sent
as ((previous plain byte + byte) mod 0x60) + 0x20, the chain starting at 0x4D
after every line break; a control byte other than CR/LF passes the rest of
the line through unchanged. Some lines are written in the clear; both kinds
are interleaved, so each line is decoded and kept in whichever form reads
as text. The log describes the session (and the fix): keep it local.
"""
import sys


def decode(line):
    out = bytearray()
    prev, state = 0x4D, 0
    for b in line:
        if b < 0x20 or b > 0x7F:
            out.append(b)
            state = 2
            continue
        if state == 2:
            out.append(b)
            continue
        p = ((b - 0x20 - prev - 0x20) % 0x60) + 0x20
        out.append(p)
        prev, state = p, 1
    return bytes(out)


def texty(s):
    good = sum(1 for b in s if 0x20 <= b < 0x7F and (chr(b).isalnum() or chr(b) in ",.$*:-_ =[]()%#/"))
    return good / max(1, len(s))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    for line in data.replace(b"\r", b"").split(b"\n"):
        d = decode(line)
        best = d if texty(d) > texty(line) else line
        sys.stdout.write(best.decode("latin1") + "\n")


if __name__ == "__main__":
    main()
