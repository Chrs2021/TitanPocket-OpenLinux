#!/usr/bin/env python3
"""
stpgps-decode.py: decode mnld <-> /dev/stpgps traffic from an strace log.

Capture on the phone with tools/gnss/gps-capture.sh (strace -f -ttt
-s 65535 -xx, attached before titan-mnld opens the device), then:
    tools/gnss/stpgps-decode.py gps.trace            # summary by frame type
    tools/gnss/stpgps-decode.py gps.trace --dump     # every frame, in order
    tools/gnss/stpgps-decode.py gps.trace --fd 6     # strace attached after the open

Frame format (see docs/gnss.md):
    AA F0 | len | body (len bytes) | AA 0F       (len and body escaped)
    len = 12-bit length + a 4-bit sequence number on top (chip -> host)
    body = [type] FE ... [u16 sum]   sum = 16-bit sum of the length's two
                                     bytes and the body bytes before it
    escapes inside the body: DE DF = AA, DE E0 = DE
Chip -> host measurement payloads are XORed with 0xCA (after type, FE).
"""
import collections
import re
import sys

DATA = re.compile(r'^(?:\d+\s+)?(\S+) (read|write)\((\d+), "((?:[^"\\]|\\.)*)"(?:\.\.\.)?, \d+\) = (-?\d+)')
OPEN = re.compile(r'openat\(AT_FDCWD, "((?:[^"\\]|\\.)*)".*= (\d+)')
CLOSE = re.compile(r'close\((\d+)\) = 0')
UNFINISHED = re.compile(r'^(?:\d+\s+)?(\S+) (read|write)\((\d+),\s+(?:"((?:[^"\\]|\\.)*)"(?:\.\.\.)?, \d+\s*)?<unfinished')
RESUMED = re.compile(r'^(?:\d+\s+)?\S+ <\.\.\. (read|write) resumed>(?:"((?:[^"\\]|\\.)*)"(?:\.\.\.)?, \d+)?.*\) = (-?\d+)')
ESC = {0xDF: 0xAA, 0xE0: 0xDE}


def messages(path, fd=None):
    """(time, 'read'|'write', bytes) for each transfer on /dev/stpgps (or on
    fd, for a trace that starts after the device was opened).
    Calls that strace splits across threads ("<unfinished ...>" then
    "<... read resumed>") are joined: a write's data is on the first line,
    a read's on the second, the result on the second."""
    pending = {}            # pid -> (time, op, fd, data) of an unfinished call
    for line in open(path, errors="replace"):
        pid = line.split(None, 1)[0] if line[:1].isdigit() else ""
        m = OPEN.search(line)
        if m:   # the path is plain, or hex escaped (strace -xx)
            path_s = bytes(m.group(1), "latin1").decode("unicode_escape")
            if path_s == "/dev/stpgps":
                fd = int(m.group(2))
            continue
        m = CLOSE.search(line)
        if m and fd is not None and int(m.group(1)) == fd:
            fd = None
            continue
        m = UNFINISHED.match(line)
        if m:
            pending[pid] = (m.group(1), m.group(2), int(m.group(3)), m.group(4) or "")
            continue
        m = RESUMED.match(line)
        if m and pid in pending:
            t, op, f, data = pending.pop(pid)
            if op != m.group(1):
                continue
            if op == "read":
                data = m.group(2) or ""
            n = int(m.group(3))
            if f == fd and n > 0:
                raw = bytes(data, "latin1").decode("unicode_escape").encode("latin1")
                yield t, op, raw[:n]
            continue
        m = DATA.match(line)
        if m and fd is not None and int(m.group(3)) == fd and int(m.group(5)) > 0:
            raw = bytes(m.group(4), "latin1").decode("unicode_escape").encode("latin1")
            yield m.group(1), m.group(2), raw[:int(m.group(5))]


def unescape(p):
    out = bytearray()
    i = 0
    while i < len(p):
        if p[i] == 0xDE and i + 1 < len(p):
            out.append(ESC.get(p[i + 1], p[i + 1]))
            i += 2
        else:
            out.append(p[i])
            i += 1
    return bytes(out)


def frames(stream, between=None):
    """(length bytes, unescaped body) for each AA F0 ... AA 0F frame, read
    as the chip's driver would: AA F0, the length (escaped like the body: a
    length byte DE is sent as DE E0), that many body bytes, AA 0F. Bytes
    outside frames are appended to `between` (a list), if given."""
    i, n = 0, len(stream)
    while i < n:
        j = stream.find(b"\xaa\xf0", i)
        if j < 0:
            if between is not None and i < n:
                between.append(stream[i:])
            return
        if between is not None and j > i:
            between.append(stream[i:j])
        k = j + 2
        out = bytearray()
        need = None
        while k < n and (need is None or len(out) < need + 2):
            b = stream[k]
            if b == 0xDE and k + 1 < n:
                out.append(ESC.get(stream[k + 1], stream[k + 1]))
                k += 2
            else:
                out.append(b)
                k += 1
            if need is None and len(out) == 2:
                # 12 bits of length; the chip puts a sequence number in the
                # top 4 (the host leaves them 0)
                need = out[0] | (out[1] & 0x0F) << 8
        if need is None or len(out) < need + 2:
            return                                  # cut off at the end
        if stream[k:k + 2] == b"\xaa\x0f":
            yield bytes(out[:2]), bytes(out[2:])
            i = k + 2
        else:                                       # not a frame: resync
            if between is not None:
                between.append(stream[j:j + 2])
            i = j + 2


def checksum_ok(head, body):
    """the last two body bytes: the 16-bit sum of the two length bytes as
    sent (sequence nibble included) and every body byte before them"""
    return (len(body) == (head[0] | (head[1] & 0x0F) << 8) and
            (head[0] + head[1] + sum(body[:-2])) & 0xFFFF ==
            int.from_bytes(body[-2:], "little"))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fd = int(sys.argv[sys.argv.index("--fd") + 1]) if "--fd" in sys.argv else None
    msgs = list(messages(sys.argv[1], fd))
    if not msgs:
        sys.exit("no /dev/stpgps traffic found (was the openat captured?)")
    dump = "--dump" in sys.argv
    stats = collections.defaultdict(lambda: [0, 0, collections.Counter(), 0])
    for direction in ("write", "read"):
        # reads are a byte stream (frames split across reads); writes are whole frames
        stream = b"".join(b for _, op, b in msgs if op == direction)
        for n, body in frames(stream):
            if not body:
                continue
            s = stats[(direction, body[0])]
            s[0] += 1
            s[1] += len(body)
            s[2][len(body)] += 1
            s[3] += checksum_ok(n, body)
            if dump:
                payload = body[2:-2]
                if direction == "read":
                    payload = bytes(x ^ 0xCA for x in payload)
                print("%s %02x %4d %s" % ("H>C" if direction == "write" else "C>H",
                                          body[0], len(body), payload[:48].hex()))
    print("%s .. %s, %d transfers" % (msgs[0][0], msgs[-1][0], len(msgs)))
    for (direction, t), (n, total, sizes, ok) in sorted(stats.items(), key=lambda kv: (kv[0][0] != "write", -kv[1][0])):
        print("%s type 0x%02x  frames %4d  bytes %7d  checksum ok %4d  sizes %s" % (
            "H>C" if direction == "write" else "C>H", t, n, total, ok, sizes.most_common(3)))


if __name__ == "__main__":
    main()
