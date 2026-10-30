#!/usr/bin/env python3
"""
own02.py: put our own channel assignments (GPS, BeiDou, GLONASS) into a
probe script.

    tools/gnss/own02.py SCRIPT BRDC.rnx LAT LON HEIGHT START_UTC [OFFSET_HZ] > own.script

SCRIPT is a replay script (stpgps-script.py) of a libmnl session. Every GPS
02 frame in it is rewritten: its Doppler becomes the one gpseph.py computes
from the broadcast ephemeris BRDC.rnx for the receiver at LAT/LON/HEIGHT, at
the time the probe will send it (START_UTC, ISO, plus the frame's script
time), plus OFFSET_HZ (default -150: libmnl's 02 Doppler sits a common
145-152 Hz below the geometric one; docs/gnss.md). Its other fields are held
at that satellite's first frame in the script (search mode), so nothing of
libmnl's tracking carries over. BeiDou and GLONASS 02 frames are rewritten
the same way from their broadcast ephemeris in the same file, with the
offsets libmnl's own frames show against ours (a base capture, 2026-10-30):
BeiDou -494 Hz on B1I, GLONASS +483 + 13.4 k Hz on its channel k (the
receiver clock's drift, ~24 ppm, scaled to each channel's carrier, it
seems); a GLONASS frame names a frequency channel (k + 8), shared by two
antipodal satellites, so the one above the horizon is used. The setup and
clock frames are replayed as they were. A line `# own02 ...` at the top
says what was done. The output says where the receiver was: keep it
local, like the scripts.
"""
import datetime
import importlib.util
import os
import struct
import sys


def load_gpseph():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("gpseph", os.path.join(here, "gpseph.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


BDS_OFFSET = -494.0             # Hz, libmnl's BeiDou 02 against ours
GLO_OFFSET = (483.0, 13.4)      # Hz: a + b k for GLONASS channel k


def rewrite(lines, ge, eph, rx, lat, lon, t0, offset=-150.0, gnss=None):
    """the script's lines with every GPS 02 frame rewritten (see above) for a
    probe starting at GPS seconds t0, and BeiDou's and GLONASS's too when
    gnss (gpseph.load_gnss) is given; returns (lines, rewritten, skipped)"""
    held = {}           # (constellation, number) -> payload bytes 12-19 of its first frame
    done = skipped = 0
    out = []
    glo = {}            # frequency channel -> slots
    memo = {}           # (system, number, 2 s step) -> (Doppler, elevation): Doppler
                        # moves under 1 Hz/s, and GLONASS orbits are costly
    def look(key, t):
        m = (key, int(t // 2))
        if m not in memo:
            memo[m] = ge.look_any(ge.nearest(gnss[key], t), t, rx, lat, lon)
        return memo[m]
    for (sy, n), l in (gnss or {}).items():
        if sy == "R":
            glo.setdefault(l[0]["k"], []).append(n)
    for line in lines:
        f = line.split()
        if len(f) == 3 and f[1] == "02":
            p = bytearray.fromhex(f[2])
            t = t0 + int(f[0].lstrip("+")) / 1000 if len(p) == 20 else 0
            dop = None
            if len(p) == 20 and p[1] == 0x01:
                if p[0] in eph:
                    dop, _ = ge.look(ge.nearest(eph[p[0]], t), t, rx, lat, lon)
                    dop += offset
            elif len(p) == 20 and p[1] == 0x20 and gnss is not None:
                if ("C", p[0]) in gnss:
                    dop = look(("C", p[0]), t)[0] + BDS_OFFSET
            elif len(p) == 20 and p[1] == 0x10 and gnss is not None:
                k = p[0] - 8
                seen = [look(("R", n), t) for n in glo.get(k, [])]
                if seen:
                    dop = max(seen, key=lambda de: de[1])[0] + GLO_OFFSET[0] + GLO_OFFSET[1] * k
            if dop is not None:
                struct.pack_into("<i", p, 8, round(dop * 65536))
                p[12:20] = held.setdefault((p[1], p[0]), bytes(p[12:20]))
                line = "%s 02 %s" % (f[0], p.hex())
                done += 1
            elif len(p) == 20 and p[1] in (0x01, 0x10, 0x20):
                skipped += 1
        out.append(line.rstrip("\n"))
    return out, done, skipped


CON = {"G": 0x01, "R": 0x10, "C": 0x20}


def visible(ge, gnss, rx, lat, lon, t, mask=5.0):
    """[(elevation, constellation byte, 02 satellite byte, key)] above the mask
    at GPS seconds t, highest first (GLONASS: one satellite per channel)"""
    out = {}
    for key, l in gnss.items():
        e = ge.nearest(l, t)
        if abs(t - e["t_toe"]) > (3600 if key[0] == "R" else 4 * 3600):
            continue
        _, el = ge.look_any(e, t, rx, lat, lon)
        if el < mask:
            continue
        sat = e["k"] + 8 if key[0] == "R" else key[1]
        if key[0] == "C" and sat > 63:
            continue
        k = (CON[key[0]], sat)
        if k not in out or el > out[k][0]:
            out[k] = (el, CON[key[0]], sat, key)
    return sorted(out.values(), reverse=True)


def regenerate(lines, ge, eph, gnss, rx, lat, lon, t0, offset=-150.0):
    """the script with every write group's 02 frames replaced by frames for
    the satellites above the horizon at that moment (from the ephemeris),
    and every 03 satellite list by theirs: the base capture's own list goes
    stale within hours. Each new frame takes the group's time fields (bytes
    2-7) from the group's first frame of that constellation and bytes 12-19
    (search mode, window) from the constellation's first frame in the
    script. Returns (lines, frames written, satellites at the start)."""
    lines = [l.rstrip("\n") for l in lines]
    tmpl = {}                   # constellation byte -> bytes 12-19 of its first frame
    for line in lines:
        f = line.split()
        if len(f) == 3 and f[1] == "02":
            p = bytes.fromhex(f[2])
            if len(p) == 20:
                tmpl.setdefault(p[1], p[12:20])
    glo = {}
    for (sy, n), l in gnss.items():
        if sy == "R":
            glo.setdefault(l[0]["k"], []).append(n)
    memo = {}

    def doppler(con, key, sat, t):
        m = (key, int(t // 2))
        if m not in memo:
            d, _ = ge.look_any(ge.nearest(gnss[key], t), t, rx, lat, lon)
            if con == 0x01:
                d += offset
            elif con == 0x20:
                d += BDS_OFFSET
            else:
                d += GLO_OFFSET[0] + GLO_OFFSET[1] * (sat - 8)
            memo[m] = d
        return memo[m]

    sky = {}                    # 10 s step -> visible list

    def sats(t):
        s = int(t // 10)
        if s not in sky:
            sky[s] = visible(ge, gnss, rx, lat, lon, t)
        return sky[s]

    out, done = [], 0
    i = 0
    while i < len(lines):
        # a write group: a line and the "+" lines after it
        j = i + 1
        while j < len(lines) and lines[j].startswith("+"):
            j += 1
        group = lines[i:j]
        i = j
        stamp = group[0].split()[0] if group[0] and not group[0].startswith("#") else None
        if stamp is None or not any(len(l.split()) == 3 and l.split()[1] in ("02", "03") for l in group):
            out += group
            continue
        t = t0 + int(stamp.lstrip("+")) / 1000
        vis = sats(t)
        new = []
        cons_done = set()
        for line in group:
            f = line.split()
            if len(f) == 3 and f[1] == "03" and f[2].startswith("00"):
                body = bytes([0x00, len(vis)]) + b"".join(bytes([sat, con]) for _, con, sat, _ in vis)
                new.append(("%s 03 %s" % (stamp, body.hex())))
                continue
            if len(f) == 3 and f[1] == "02":
                p = bytes.fromhex(f[2])
                if len(p) != 20 or p[1] not in tmpl:
                    new.append(line)
                    continue
                if p[1] in cons_done:
                    continue
                cons_done.add(p[1])
                for el, con, sat, key in vis:
                    if con != p[1]:
                        continue
                    q = bytearray(p)
                    q[0] = sat
                    struct.pack_into("<i", q, 8, round(doppler(con, key, sat, t) * 65536))
                    q[12:20] = tmpl[con]
                    new.append("%s 02 %s" % (stamp, q.hex()))
                    done += 1
                continue
            new.append(line)
        # keep the write-group marking: first line plain, the rest "+"
        for k, line in enumerate(new):
            f = line.split(None, 1)
            ts = f[0].lstrip("+")
            out.append(("+" if k else "") + ts + " " + f[1])
    first = sats(t0)
    return out, done, len(first)


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__)
    ge = load_gpseph()
    eph = ge.load(sys.argv[2])
    lat, lon, h = map(float, sys.argv[3:6])
    start = datetime.datetime.fromisoformat(sys.argv[6]).replace(tzinfo=datetime.timezone.utc)
    offset = float(sys.argv[7]) if len(sys.argv) > 7 and not sys.argv[7].startswith("--") else -150.0
    gnss = None if "--gps-only" in sys.argv else ge.load_gnss(sys.argv[2])
    if gnss is not None and "--keep-list" not in sys.argv:
        lines, done, nsat = regenerate(open(sys.argv[1]), ge, eph, gnss, ge.ecef(lat, lon, h),
                                       lat, lon, ge.gps_seconds(start), offset)
        print("# own02 start %s offset %+.0f Hz (satellites from the ephemeris)" % (sys.argv[6], offset))
        print("\n".join(lines))
        print("own02: %d frames written, %d satellites above 5 degrees at the start" % (done, nsat),
              file=sys.stderr)
        return
    lines, done, skipped = rewrite(open(sys.argv[1]), ge, eph, ge.ecef(lat, lon, h), lat, lon,
                                   ge.gps_seconds(start), offset, gnss)
    print("# own02 start %s offset %+.0f Hz" % (sys.argv[6], offset))
    print("\n".join(lines))
    print("own02: %d frames rewritten, %d without ephemeris" % (done, skipped), file=sys.stderr)


if __name__ == "__main__":
    main()
