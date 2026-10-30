#!/usr/bin/env python3
"""
gpseph.py: GPS broadcast ephemeris (RINEX 3 navigation file) to satellite
position, velocity and the Doppler a receiver sees, for making the chip's
02 channel assignments ourselves (docs/gnss.md, "Replacing libmnl").

    tools/gnss/gpseph.py BRDC.rnx LAT LON HEIGHT [UTC]   # Doppler of each GPS satellite above the horizon

Orbit from IS-GPS-200 (20.3.3.4.3); velocity by differencing position over
±0.5 s, which is plenty for a Doppler search centre. Doppler is
-(range rate) / L1 wavelength, without the receiver's clock drift: the
convention of the chip's 02 frames.
"""
import datetime
import math
import sys

MU = 3.986005e14
OMEGA_E = 7.2921151467e-5
C = 299792458.0
F1 = 1575.42e6
GPS_EPOCH = datetime.datetime(1980, 1, 6, tzinfo=datetime.timezone.utc)
LEAP = 18       # GPS - UTC, seconds (since 2017)


def gps_seconds(utc):
    """seconds since the GPS epoch, for a UTC datetime"""
    return (utc - GPS_EPOCH).total_seconds() + LEAP


def _num(s):
    s = s.strip().replace("D", "E")
    return float(s) if s else 0.0


def load(path):
    """{prn: [ephemeris dict, ...]} for GPS records in a RINEX 3 navigation file"""
    eph = {}
    lines = open(path).read().splitlines()
    i = next(k for k, l in enumerate(lines) if "END OF HEADER" in l) + 1
    while i < len(lines):
        l = lines[i]
        if not l.startswith("G"):
            i += 1
            continue
        rec = lines[i:i + 8]
        i += 8
        vals = [_num(rec[0][23 + 19 * k:42 + 19 * k]) for k in range(3)]
        for r in rec[1:]:
            vals += [_num(r[4 + 19 * k:23 + 19 * k]) for k in range(4)]
        names = ["af0", "af1", "af2", "iode", "crs", "dn", "m0", "cuc", "e", "cus", "sqrta",
                 "toe", "cic", "omega0", "cis", "i0", "crc", "w", "omegadot", "idot", "l2codes",
                 "week", "l2p", "sva", "svh", "tgd", "iodc", "tom", "fit"]
        e = dict(zip(names, vals))
        if e["svh"]:
            continue
        e["t_toe"] = e["week"] * 604800 + e["toe"]
        eph.setdefault(int(rec[0][1:3]), []).append(e)
    return eph


def nearest(ephs, t):
    return min(ephs, key=lambda e: abs(t - e["t_toe"]))


# ---- BeiDou and GLONASS (for the 02 search assignments of all three) ----

MU_BDS = 3.986004418e14
OMEGA_BDS = 7.2921150e-5
F_B1I = 1561.098e6
BDS_GEO = set(range(1, 6)) | set(range(59, 64))
MU_GLO = 3.9860044e14
J2_GLO = 1.0826257e-3
RE_GLO = 6378136.0
OMEGA_GLO = 7.292115e-5


def load_gnss(path):
    """{(system, number): [ephemeris dict, ...]} for GPS ("G", PRN), BeiDou
    ("C", PRN) and GLONASS ("R", slot) records of a RINEX 3 navigation file.
    Each dict has "sys", "t_toe" (GPS seconds) and "freq" (carrier, Hz);
    GLONASS's has "k", its frequency channel."""
    out = {("G", prn): l for prn, l in load(path).items()}
    for l in out.values():
        for e in l:
            e["sys"], e["freq"] = "G", F1
    lines = open(path).read().splitlines()
    i = next(k for k, l in enumerate(lines) if "END OF HEADER" in l) + 1
    while i < len(lines):
        l = lines[i]
        if l.startswith("C"):
            rec = lines[i:i + 8]
            i += 8
            vals = [_num(rec[0][23 + 19 * k:42 + 19 * k]) for k in range(3)]
            for r in rec[1:]:
                vals += [_num(r[4 + 19 * k:23 + 19 * k]) for k in range(4)]
            names = ["af0", "af1", "af2", "aode", "crs", "dn", "m0", "cuc", "e", "cus", "sqrta",
                     "toe", "cic", "omega0", "cis", "i0", "crc", "w", "omegadot", "idot", "spare1",
                     "week", "spare2", "sva", "svh", "tgd", "tgd2", "ttm", "aodc"]
            e = dict(zip(names, vals))
            if e["svh"]:
                continue
            # BDT week 0 = GPS week 1356; BDT = GPST - 14 s
            e["t_toe"] = (e["week"] + 1356) * 604800 + e["toe"] + 14
            e["sys"], e["freq"], e["prn"] = "C", F_B1I, int(l[1:3])
            out.setdefault(("C", e["prn"]), []).append(e)
        elif l.startswith("R"):
            rec = lines[i:i + 4]
            i += 4
            y, mo, d, hh, mi, ss = (int(x) for x in rec[0][4:23].split())
            vals = [_num(rec[0][23 + 19 * k:42 + 19 * k]) for k in range(3)]
            for r in rec[1:]:
                vals += [_num(r[4 + 19 * k:23 + 19 * k]) for k in range(4)]
            e = dict(zip(["mtaun", "gamma", "tk", "x", "vx", "ax", "svh", "y", "vy", "ay", "k",
                          "z", "vz", "az", "age"], vals))
            if e["svh"]:
                continue
            utc = datetime.datetime(y, mo, d, hh, mi, ss, tzinfo=datetime.timezone.utc)
            e["t_toe"] = gps_seconds(utc)
            e["k"] = int(e["k"])
            e["sys"], e["freq"] = "R", 1602e6 + e["k"] * 0.5625e6
            out.setdefault(("R", int(l[1:3])), []).append(e)
        else:
            i += 1
    return out


def _bds_position(e, t):
    tk = t - e["t_toe"]
    a = e["sqrta"] ** 2
    n = math.sqrt(MU_BDS / a ** 3) + e["dn"]
    m = e["m0"] + n * tk
    ek = m
    for _ in range(10):
        ek = m + e["e"] * math.sin(ek)
    v = math.atan2(math.sqrt(1 - e["e"] ** 2) * math.sin(ek), math.cos(ek) - e["e"])
    phi = v + e["w"]
    u = phi + e["cus"] * math.sin(2 * phi) + e["cuc"] * math.cos(2 * phi)
    r = a * (1 - e["e"] * math.cos(ek)) + e["crs"] * math.sin(2 * phi) + e["crc"] * math.cos(2 * phi)
    i = e["i0"] + e["cis"] * math.sin(2 * phi) + e["cic"] * math.cos(2 * phi) + e["idot"] * tk
    x, y = r * math.cos(u), r * math.sin(u)
    toe = e["toe"]
    if e["prn"] in BDS_GEO:
        # geostationary: the orbit in an inertial-ish frame, then rotated by
        # the earth's turn and -5 degrees about x (BDS ICD 5.2.4.12)
        om = e["omega0"] + e["omegadot"] * tk - OMEGA_BDS * toe
        xg = x * math.cos(om) - y * math.cos(i) * math.sin(om)
        yg = x * math.sin(om) + y * math.cos(i) * math.cos(om)
        zg = y * math.sin(i)
        f = math.radians(-5.0)
        y1 = yg * math.cos(f) + zg * math.sin(f)
        z1 = -yg * math.sin(f) + zg * math.cos(f)
        th = OMEGA_BDS * tk
        return (xg * math.cos(th) + y1 * math.sin(th), -xg * math.sin(th) + y1 * math.cos(th), z1)
    om = e["omega0"] + (e["omegadot"] - OMEGA_BDS) * tk - OMEGA_BDS * toe
    return (x * math.cos(om) - y * math.cos(i) * math.sin(om),
            x * math.sin(om) + y * math.cos(i) * math.cos(om),
            y * math.sin(i))


def _glo_deriv(s, acc):
    x, y, z, vx, vy, vz = s
    r2 = x * x + y * y + z * z
    r3 = r2 * math.sqrt(r2)
    a = 1.5 * J2_GLO * MU_GLO * RE_GLO ** 2 / r2 / r3
    b = 5.0 * z * z / r2
    c = -MU_GLO / r3 - a * (1.0 - b)
    w2 = OMEGA_GLO ** 2
    return (vx, vy, vz,
            (c + w2) * x + 2 * OMEGA_GLO * vy + acc[0],
            (c + w2) * y - 2 * OMEGA_GLO * vx + acc[1],
            (c - 2 * a) * z + acc[2])


def _glo_position(e, t):
    """the broadcast state (PZ-90, km) integrated to t (GLONASS ICD A.3.1.2,
    Runge-Kutta 4 in steps of at most 60 s)"""
    s = [e["x"] * 1e3, e["y"] * 1e3, e["z"] * 1e3, e["vx"] * 1e3, e["vy"] * 1e3, e["vz"] * 1e3]
    acc = (e["ax"] * 1e3, e["ay"] * 1e3, e["az"] * 1e3)
    t_from = e["t_toe"]
    # carry on from the last state integrated, when it's nearer: callers
    # mostly step forward through time
    near = e.get("_last")
    if near and abs(t - near[0]) < abs(t - t_from):
        t_from, s = near[0], list(near[1])
    dt = t - t_from
    n = max(1, int(math.ceil(abs(dt) / 60.0)))
    h = dt / n
    for _ in range(n):
        k1 = _glo_deriv(s, acc)
        k2 = _glo_deriv([s[j] + h / 2 * k1[j] for j in range(6)], acc)
        k3 = _glo_deriv([s[j] + h / 2 * k2[j] for j in range(6)], acc)
        k4 = _glo_deriv([s[j] + h * k3[j] for j in range(6)], acc)
        s = [s[j] + h / 6 * (k1[j] + 2 * k2[j] + 2 * k3[j] + k4[j]) for j in range(6)]
    e["_last"] = (t, tuple(s))
    return tuple(s[:3])


def position_any(e, t):
    """ECEF position (m) at GPS seconds t, any of the three systems"""
    if e.get("sys") == "C":
        return _bds_position(e, t)
    if e.get("sys") == "R":
        return _glo_position(e, t)
    return position(e, t)


def clock_drift(e):
    """satellite clock drift, s/s"""
    return e["gamma"] if e.get("sys") == "R" else e["af1"]


def look_any(e, t, rx, lat, lon):
    """(Doppler Hz on the satellite's own carrier, elevation deg), as look()"""
    omega = OMEGA_BDS if e.get("sys") == "C" else OMEGA_GLO if e.get("sys") == "R" else OMEGA_E

    def rng(tt):
        tau = 0.075
        for _ in range(2):
            s = position_any(e, tt - tau)
            th = omega * tau
            s = (s[0] * math.cos(th) + s[1] * math.sin(th),
                 -s[0] * math.sin(th) + s[1] * math.cos(th), s[2])
            d = [s[k] - rx[k] for k in range(3)]
            tau = math.sqrt(sum(x * x for x in d)) / C
        return tau * C, d
    r1, _ = rng(t + 0.5)
    r0, d = rng(t - 0.5)
    rate = r1 - r0 - clock_drift(e) * C
    la, lo = math.radians(lat), math.radians(lon)
    up = (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))
    el = math.degrees(math.asin(sum(d[k] * up[k] for k in range(3)) / math.sqrt(sum(x * x for x in d))))
    return -rate * e["freq"] / C, el


def position(e, t):
    """ECEF position (m) at GPS seconds t"""
    tk = t - e["t_toe"]
    a = e["sqrta"] ** 2
    n = math.sqrt(MU / a ** 3) + e["dn"]
    m = e["m0"] + n * tk
    ek = m
    for _ in range(10):
        ek = m + e["e"] * math.sin(ek)
    v = math.atan2(math.sqrt(1 - e["e"] ** 2) * math.sin(ek), math.cos(ek) - e["e"])
    phi = v + e["w"]
    du = e["cus"] * math.sin(2 * phi) + e["cuc"] * math.cos(2 * phi)
    dr = e["crs"] * math.sin(2 * phi) + e["crc"] * math.cos(2 * phi)
    di = e["cis"] * math.sin(2 * phi) + e["cic"] * math.cos(2 * phi)
    u = phi + du
    r = a * (1 - e["e"] * math.cos(ek)) + dr
    i = e["i0"] + di + e["idot"] * tk
    x, y = r * math.cos(u), r * math.sin(u)
    om = e["omega0"] + (e["omegadot"] - OMEGA_E) * tk - OMEGA_E * e["toe"]
    return (x * math.cos(om) - y * math.cos(i) * math.sin(om),
            x * math.sin(om) + y * math.cos(i) * math.cos(om),
            y * math.sin(i))


def ecef(lat, lon, h):
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    la, lo = math.radians(lat), math.radians(lon)
    n = a / math.sqrt(1 - e2 * math.sin(la) ** 2)
    return ((n + h) * math.cos(la) * math.cos(lo), (n + h) * math.cos(la) * math.sin(lo),
            (n * (1 - e2) + h) * math.sin(la))


def look(e, t, rx, lat, lon):
    """(Doppler Hz, elevation deg) of the satellite seen from rx (ECEF) at GPS seconds t"""
    def rng(tt):
        tau = 0.075                                 # light time, iterated once
        for _ in range(2):
            s = position(e, tt - tau)
            th = OMEGA_E * tau                      # earth rotation during the flight
            s = (s[0] * math.cos(th) + s[1] * math.sin(th),
                 -s[0] * math.sin(th) + s[1] * math.cos(th), s[2])
            d = [s[k] - rx[k] for k in range(3)]
            tau = math.sqrt(sum(x * x for x in d)) / C
        return tau * C, d
    r1, _ = rng(t + 0.5)
    r0, d = rng(t - 0.5)
    rate = r1 - r0 - e["af1"] * C                   # satellite clock drift as range rate
    la, lo = math.radians(lat), math.radians(lon)
    up = (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))
    el = math.degrees(math.asin(sum(d[k] * up[k] for k in range(3)) / math.sqrt(sum(x * x for x in d))))
    return -rate * F1 / C, el


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    eph = load(sys.argv[1])
    lat, lon, h = map(float, sys.argv[2:5])
    utc = (datetime.datetime.fromisoformat(sys.argv[5]).replace(tzinfo=datetime.timezone.utc)
           if len(sys.argv) > 5 else datetime.datetime.now(datetime.timezone.utc))
    t = gps_seconds(utc)
    rx = ecef(lat, lon, h)
    for prn in sorted(eph):
        dop, el = look(nearest(eph[prn], t), t, rx, lat, lon)
        if el > 0:
            print("G%02d  elevation %5.1f  Doppler %+8.1f Hz" % (prn, el, dop))


if __name__ == "__main__":
    main()
