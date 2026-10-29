#!/usr/bin/env python3
"""
gen-dip-tuning.py: generate modules/camera/dip/dip_tuning.h, the per-sensor
pass-2 (DIP) tuning the camera drivers use, from your own phone's vendor
libraries.

The tables are MediaTek's and the sensor vendors' (in the stock
libCamera_<sensor>mipiraw_*.so and libcam.hal3a.v3.so), so the project
doesn't ship them: the published dip_tuning.h is a placeholder with neutral
values (an identity colour matrix, a straight gamma, no noise reduction),
which builds and runs but isn't tuned. Run this against the libraries from
your phone's vendor partition to get the real ones:

    gen-dip-tuning.py --vendor VENDOR/lib64 [--system SYSTEM/lib64]
                      [-o modules/camera/dip/dip_tuning.h] [--no-nbc]

(modules/camera/tools/ in the repository, with the emulator it uses,
emu_hal.py; tools/camera/ in the working tree.)

  --vendor  the vendor partition's lib64 (libCamera_ov16880mipiraw_*.so,
            libCamera_ov8865mipiraw_*.so, libcam.hal3a.v3.so)
  --system  the system partition's lib64 (its libc++.so), for the noise
            reduction tables only
  --no-nbc  skip noise reduction (it needs Unicorn: pip install unicorn);
            the drivers leave it off by default anyway (titan_camera nr=0)

What it takes (docs/camera.md, "Pass 2"):

  udm  demosaic: the first 20 words of <sensor>mipiraw_UDM_0040 (OV16880)
       and _UDM_0080 (OV8865), from the Scene_Preview library
  ccm  colour: the first matrix of _CCM_0000 / _CCM_0001 (Scene_Capture: the
       six words after its 4-word header), plus two fixed control words
  ggm  gamma: _GMA_0001's first curve, 192 words of B | G << 10 | R << 20
  nbc  noise reduction (NBC/ANR): the stock ISP_MGR_NBC::apply run in
       emulation (emu_hal.py) on the middle ANR table, as
       {register offset, value} pairs

The result is yours: keep it local, like the libraries it came from.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def first(*paths):
    return next((p for p in paths if os.path.exists(p)), paths[0])


# laid out as in the repository (modules/camera/tools/, emu_hal.py beside
# it) or the working tree (tools/camera/, tools/re/emu_hal.py)
EMU_DIR = first(HERE, os.path.join(HERE, "..", "re"))
DIP_DIR = first(os.path.join(HERE, "..", "dip"),
                os.path.join(HERE, "..", "..", "modules", "camera", "dip"))

# index 0 rear, 1 front: sensor, UDM symbol, CCM symbol
SENSORS = [("ov16880", "UDM_0040", "CCM_0000"),
           ("ov8865", "UDM_0080", "CCM_0001")]
# the HAL's fixed words after the matrix (the same for both sensors)
CCM_CONTROL = (0x1FBE0419, 0x1CE729CE)


def elf_symbols(path):
    """{name: bytes} of an AArch64 shared library's defined data symbols"""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        sys.exit("%s: not a 64-bit ELF" % path)
    phoff, shoff = struct.unpack_from("<QQ", d, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", d, 0x36)
    loads = []
    for i in range(phnum):
        kind, _, off, va, _, filesz = struct.unpack_from("<IIQQQQ", d, phoff + i * phentsize)
        if kind == 1:                           # PT_LOAD
            loads.append((va, off, filesz))
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next(s for s in secs if s[1] == 11)  # SHT_DYNSYM
    strtab = secs[dynsym[6]][4]
    out = {}
    for k in range(dynsym[5] // 24):
        name, _, _, shndx, value, size = struct.unpack_from("<IBBHQQ", d, dynsym[4] + k * 24)
        if not size or not shndx:
            continue
        for va, off, filesz in loads:
            if va <= value < va + filesz:
                n = d[strtab + name:d.index(b"\0", strtab + name)].decode()
                out[n] = d[value - va + off:value - va + off + size]
                break
    return out


def words(b):
    return list(struct.unpack_from("<%dI" % (len(b) // 4), b))


def lib(vendor, sensor, scene):
    path = os.path.join(vendor, "libCamera_%smipiraw_%s.so" % (sensor, scene))
    if not os.path.exists(path):
        sys.exit("%s not found (is --vendor the vendor partition's lib64?)" % path)
    return elf_symbols(path)


def nbc_tables(vendor, system):
    """{sensor: [(offset, value)]} from the stock ISP_MGR_NBC::apply"""
    sys.path.insert(0, EMU_DIR if os.path.exists(os.path.join(EMU_DIR, "emu_hal.py"))
                    else os.path.join(HERE, "..", "re"))
    try:
        from emu_hal import Hal
    except ImportError as e:
        sys.exit("noise reduction needs Unicorn (pip install unicorn), or use --no-nbc: %s" % e)
    hal3a = os.path.join(vendor, "libcam.hal3a.v3.so")
    libcxx = os.path.join(system, "libc++.so")
    for p in (hal3a, libcxx):
        if not os.path.exists(p):
            sys.exit("%s not found (use --system, or --no-nbc)" % p)
    sym = {
        "ggm_inst": "_ZN13NSIspTuningv311ISP_MGR_GGM11getInstanceEN11NSIspTuning12ESensorDev_TE",
        "ggm_put": "_ZN13NSIspTuningv311ISP_MGR_GGM3putIN11NSIspTuning15ISP_NVRAM_GGM_TEEERS0_RKT_",
        "inst": "_ZN13NSIspTuningv311ISP_MGR_NBC11getInstanceEN11NSIspTuning12ESensorDev_TE",
        "put": "_ZN13NSIspTuningv311ISP_MGR_NBC3putIN11NSIspTuning15ISP_NVRAM_ANR_TEEERS0_RKT_",
        "apply": "_ZN13NSIspTuningv311ISP_MGR_NBC5applyEN11NSIspTuning13EIspProfile_TEPV13_dip_x_reg_t_",
    }
    out = {}
    for sensor, _, _ in SENSORS:
        h = Hal(verbose=False)
        h.lenient = True
        h.load(libcxx)
        h.load(hal3a)
        h.start()
        # the 3A sensor map is empty without the full HAL: dev <-> idx return 0
        for name in ("_ZN6NS3Av317mapSensorDevToIdxEi", "_ZN6NS3Av317mapSensorIdxToDevEi"):
            h.uc.mem_write(h.sym(name), struct.pack("<II", 0x52800000, 0xD65F03C0))
        regs = h.malloc(0x6000)                # a DIP register image
        h.w32(regs + 0x04, 0x60)               # YUV_EN: NBC 5, NBC2 6
        h.w32(regs + 0x0C, 0x20)               # RGB_EN: LCE 5 (NBC reads it)
        pre = lib(vendor, sensor, "Scene_Preview")
        # apply() reads the gamma: seed GGM with the real one
        gma = pre["%smipiraw_GMA_0001" % sensor]
        buf = h.malloc(len(gma))
        h.uc.mem_write(buf, gma)
        h.call(h.sym(sym["ggm_put"]), h.call(h.sym(sym["ggm_inst"]), 0), buf)
        anr = sorted(n for n in pre if n.startswith("%smipiraw_NBC_ANR_" % sensor))
        table = pre[anr[len(anr) // 2]]        # a mid-ISO table
        mgr = h.call(h.sym(sym["inst"]), 0)
        buf = h.malloc(len(table) + 0x400)
        h.uc.mem_write(buf, table)
        h.call(h.sym(sym["put"]), mgr, buf)
        before = bytes(h.uc.mem_read(regs, 0x6000))
        h.call(h.sym(sym["apply"]), mgr, 0, regs)
        after = bytes(h.uc.mem_read(regs, 0x6000))
        out[sensor] = [(i, struct.unpack_from("<I", after, i)[0]) for i in range(0, 0x6000, 4)
                       if before[i:i + 4] != after[i:i + 4]]
        print("%s: noise reduction from %s, %d registers" % (sensor, anr[len(anr) // 2],
                                                             len(out[sensor])), file=sys.stderr)
    return out


def c_array(name, values, per_line=6):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("\t" + " ".join("0x%08x," % v for v in values[i:i + per_line]))
    return "static const u32 %s[%d] = {\n%s\n};\n" % (name, len(values), "\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vendor", required=True)
    ap.add_argument("--system")
    ap.add_argument("-o", "--output", default=os.path.join(DIP_DIR, "dip_tuning.h"))
    ap.add_argument("--no-nbc", action="store_true")
    a = ap.parse_args()

    text = ["/* SPDX-License-Identifier: GPL-2.0 */",
            "/* Stock per-sensor DIP tuning, extracted from the vendor libCamera_<sensor>_*.so tables",
            " * by tools/camera/gen-dip-tuning.py (see docs/camera.md \"Pass 2\"). Index 0 rear",
            " * OV16880, 1 front OV8865. Generated from your phone's libraries: keep it local. */",
            ""]
    for sensor, udm, ccm in SENSORS:
        pre = lib(a.vendor, sensor, "Scene_Preview")
        cap = lib(a.vendor, sensor, "Scene_Capture")
        text.append(c_array("%s_udm" % sensor, words(pre["%smipiraw_%s" % (sensor, udm)])[:20]))
        text.append("/* first matrix of %smipiraw_%s, plus the HAL fixed control words */"
                    % (sensor, ccm))
        text.append(c_array("%s_ccm" % sensor,
                            words(cap["%smipiraw_%s" % (sensor, ccm)])[4:10] + list(CCM_CONTROL),
                            per_line=8))
        text.append("/* %smipiraw_GMA_0001, first curve: B | G << 10 | R << 20 */" % sensor)
        text.append(c_array("%s_ggm" % sensor, words(pre["%smipiraw_GMA_0001" % sensor])[:192]))

    if a.no_nbc:
        nbc = {s: [] for s, _, _ in SENSORS}
        text.append("/* Noise reduction (NBC/ANR): not generated (--no-nbc), so none */")
    else:
        nbc = nbc_tables(a.vendor, a.system or os.path.join(os.path.dirname(a.vendor.rstrip("/")),
                                                            "..", "system", "lib64"))
        text.append("/* Noise reduction (NBC/ANR) registers, from the stock ISP_MGR_NBC::apply")
        text.append(" * run on a mid-ISO ANR table. {offset, value} pairs. */")
    for sensor, _, _ in SENSORS:
        pairs = "".join("\t{ 0x%04x, 0x%08x },\n" % p for p in nbc[sensor])
        text.append("static const u32 %s_nbc[%s][2] = {\n%s};" % (
            sensor, len(nbc[sensor]) or "", pairs))
    with open(a.output, "w") as f:
        f.write("\n".join(text) + "\n")
    print("wrote %s" % a.output, file=sys.stderr)


if __name__ == "__main__":
    main()
