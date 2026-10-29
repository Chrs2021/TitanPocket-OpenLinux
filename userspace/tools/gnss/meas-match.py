#!/usr/bin/env python3
"""
meas-match.py: line up the chip's 0x29 measurement records with libmnl's own
measurements of the same session, for decoding the records (docs/gnss.md,
"Replacing libmnl", stage 2).

    tools/gnss/meas-match.py gps2.trace meas.log            # per satellite
    tools/gnss/meas-match.py gps2.trace meas.log --rows     # every match

gps2.trace: tools/gnss/gps-capture.sh's strace capture. meas.log:
titan-mnld's TITAN_MNLD_MEAS log, recorded in the same session (both are
CLOCK_REALTIME). Each libmnl measurement is paired with the latest record
for the same satellite in the second before it, and the decoded fields are
printed beside libmnl's values. Both files say where the phone was: keep
them local.
"""
import bisect
import collections
import importlib.util
import os
import struct
import sys

C = 299792458.0


def load_decoder():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("stpgps_decode",
                                                  os.path.join(here, "stpgps-decode.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def bits(p, start, width, signed=False):
    v = (int.from_bytes(p, "little") >> start) & ((1 << width) - 1)
    return v - (1 << width) if signed and v >> (width - 1) else v


def record(p):
    """the 0x29 fields decoded so far (docs/gnss.md)"""
    con = {0x08: "G", 0x80: "R", 0x01: "C"}.get(p[2])
    if con is None:
        return None
    sv = bits(p, 11, 6 if con == "C" else 5)
    return {
        "channel": p[0] >> 3,
        "flags": p[0] & 7,
        # R: frequency channel k + 8; C: PRN mod 32, + 32 (C1 and C33 alike)
        "sat": (con, sv - 32 if con == "C" else sv),
        "cn0": ((p[3] >> 3) | (p[3] & 1) << 5) - 3,     # dB-Hz
        "layout": p[5],
        "time": bits(p, 160, 32) / 2 ** 27,             # seconds, modulo 32
        "doppler": bits(p, 240, 24, True) / 2 ** 11,    # Hz, receiver drift excluded
    }


def chip_records(dec, path):
    """(time, payload) of each 0x29 frame, XOR undone"""
    stream, offs, times = bytearray(), [], []
    for t, op, data in dec.messages(path):
        if op == "read":
            offs.append(len(stream))
            times.append(float(t))
            stream += data
    stream = bytes(stream)
    pos = 0
    for _, body in dec.frames(stream):
        j = stream.find(b"\xaa\xf0", pos)
        pos = j + 2
        if body[0] == 0x29:
            t = times[bisect.bisect_right(offs, j) - 1]
            yield t, bytes(x ^ 0xCA for x in body[2:-2])


def libmnl(path):
    """libmnl's entries (titan-mnld.c, meas_log: 0xA4 bytes each)"""
    for line in open(path):
        f = line.split()
        if f[0] != "M":
            continue
        b = bytes.fromhex(f[4])
        con = {1: "G", 3: "R", 5: "C"}.get(b[10])
        if con is None:
            continue
        sv = struct.unpack_from("<h", b, 8)[0]
        cf = struct.unpack_from("<f", b, 0x54)[0]
        yield {
            "t": int(f[1]) / 1000,
            "con": con,
            "svid": sv,
            # GLONASS: libmnl gives the slot; the chip numbers by frequency.
            # BeiDou: the chip's id is the PRN modulo 32
            "key": (con, round((cf - 1602e6) / 562500) + 8 if con == "R"
                    else sv % 32 if con == "C" else sv),
            "state": struct.unpack_from("<I", b, 0x14)[0],
            "tsv": struct.unpack_from("<q", b, 0x18)[0],
            "cn0": struct.unpack_from("<d", b, 0x28)[0],
            "doppler": -struct.unpack_from("<d", b, 0x30)[0] * cf / C,
        }


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dec = load_decoder()
    recs = collections.defaultdict(list)
    for t, p in chip_records(dec, sys.argv[1]):
        r = record(p)
        if r:
            recs[r["sat"]].append((t, r))
    rows = []
    for m in libmnl(sys.argv[2]):
        l = recs.get(m["key"], [])
        i = bisect.bisect_right([t for t, _ in l], m["t"]) - 1
        if i >= 0 and m["t"] - l[i][0] < 1.0:
            rows.append((m, l[i][1]))
    # receiver drift per epoch and constellation (in Hz it scales with the
    # carrier): libmnl's Doppler minus the record's
    drift = collections.defaultdict(list)
    for m, r in rows:
        drift[m["t"], m["con"]].append(m["doppler"] - r["doppler"])
    drift = {k: sorted(v)[len(v) // 2] for k, v in drift.items()}
    if "--rows" in sys.argv:
        for m, r in rows:
            print("%.3f %s%-3d ch%-2d cn0 %2.0f/%2d  doppler %9.1f/%9.1f  state %5d tsv %d" % (
                m["t"], m["con"], m["svid"], r["channel"], m["cn0"], r["cn0"],
                m["doppler"] - drift[m["t"], m["con"]], r["doppler"], m["state"], m["tsv"]))
        return
    seen = collections.Counter(m["key"] for m, _ in rows)
    total = collections.Counter(m["key"] for m in libmnl(sys.argv[2]))
    print("satellite  libmnl  matched  records  cn0 equal")
    for key in sorted(total):
        same = sum(m["cn0"] == r["cn0"] for m, r in rows if m["key"] == key)
        print("%s%-3d %12d %8d %8d %10d" % (key[0], key[1], total[key], seen[key],
                                          len(recs.get(key, [])), same))


if __name__ == "__main__":
    main()
