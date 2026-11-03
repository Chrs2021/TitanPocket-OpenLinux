#!/usr/bin/env python3
"""Speech parameters for the MT6771 modem (gen 93), built from the stock
XML audio parameters, as the stock audio HAL's SpeechParserGen93 packs
them (docs/modem.md, "Speech parameters"). The modem reads them from the
speech shared memory's parameter ring; speech on (0x2f20) says where.

Usage: sphparam.py XMLDIR [--profile Normal] [--volume 3] [--mode 0] [-o out]
XMLDIR holds Speech*_AudioParam.xml and *_ParamUnitDesc.xml (the phone's
/vendor/etc/audio_param). The output is the blob for one profile and
volume: the Speech unit (every network and band), DMNR, General and
MagiClarity units, in that order.
"""
import argparse
import re
import struct
import sys
import xml.etree.ElementTree as ET

# bytes per element of each XML parameter type
TYPE_SIZE = {"byte_array": 1, "ubyte_array": 1, "short_array": 2,
             "ushort_array": 2, "int_array": 4, "uint_array": 4,
             "int": 4, "uint": 4, "float": 4}
TYPE_FMT = {"byte_array": "b", "ubyte_array": "B", "short_array": "h",
            "ushort_array": "H", "int_array": "i", "uint_array": "I",
            "int": "i", "uint": "I"}

SPEECH_PARAMS = ("speech_mode_para", "sph_in_fir", "sph_out_fir",
                 "sph_in_iir_mic1_dsp", "sph_in_iir_mic2_dsp",
                 "sph_in_iir_enh_dsp", "sph_out_iir_enh_dsp")
BANDS = {"NB": 0x1000, "WB": 0x2000, "SWB": 0x3000, "FB": 0x4000}
VERSION = (2, 0)            # SPH_PARAM_VERSION in AudioParamOptions.xml


class AudioType:
    """one audio type: its category types (in order), parameter types,
    path tree and parameter units"""

    def __init__(self, xmldir, name):
        desc = ET.parse("%s/%s_ParamUnitDesc.xml" % (xmldir, name)).getroot()
        self.categories = {ct.get("name"): [c.get("name") for c in ct.iter("Category")]
                           for ct in desc.iter("CategoryType")}
        self.ptype = {p.get("name"): p.get("type") for p in desc.iter("Param")}
        param = ET.parse("%s/%s_AudioParam.xml" % (xmldir, name)).getroot()
        self.tree = [(p.get("path"), int(p.get("param_id")))
                     for p in param.find("ParamTree")]
        self.units = {int(u.get("param_id")): {p.get("name"): p.get("value")
                                               for p in u}
                      for u in param.find("ParamUnitPool")}

    def unit(self, values):
        """the parameter unit for the category values (in category type
        order): the most specific tree path whose items all match, in
        order"""
        best = None
        for path, pid in self.tree:
            items = [i for i in path.split(",") if i] if path else []
            k = 0
            for want in values:
                if k < len(items) and items[k] == want:
                    k += 1
            if k == len(items) and (best is None or len(items) > best[0]):
                best = (len(items), pid)
        if best is None:
            raise KeyError("no parameter unit for %s" % ",".join(values))
        return self.units[best[1]]

    def pack(self, unit, names):
        """the listed parameters' raw values, concatenated (missing ones
        skipped), as getSpeechParamFromAppParser does"""
        out = b""
        for n in names:
            if n not in unit:
                continue
            t = self.ptype[n]
            vals = [int(v, 0) for v in unit[n].split(",") if v.strip()]
            fmt = TYPE_FMT[t]
            if fmt.isupper():
                vals = [v & ((1 << (8 * TYPE_SIZE[t])) - 1) for v in vals]
            out += struct.pack("<%d%s" % (len(vals), fmt), *vals)
        return out


def unit_hdr(n_lo, n_hi, kind, magic):
    """setMDParamUnitHdr: version 1, 2 layers, counts, kind, band mask
    (1, 3, 7, 15 for 1-4 bands), XML version, magic"""
    mask = (1, 3, 7, 15)[n_hi - 1] if 1 <= n_hi <= 4 else 3
    return struct.pack("<8H", 1, 2, (n_lo & 0xf) | (n_hi & 0xf) << 4, kind,
                       mask, VERSION[0] << 4 | VERSION[1], 0, magic)


def speech_unit(xmldir, profile, volume, mode):
    at = AudioType(xmldir, "Speech")
    net = AudioType(xmldir, "SpeechNetwork")
    networks = []
    for name in net.categories["Network"]:
        u = net.unit([name])
        bit = int(u["speech_network_support"].split(",")[0], 0)
        networks.append((name, bit))
    bands = at.categories["Band"]
    out = unit_hdr(len(networks), len(bands), 0x1f, 0xaa01)
    out += struct.pack("<H", volume << 4 | mode)
    for name, bit in networks:
        for band in bands:
            u = at.unit(["Default", band, profile, str(volume), name])
            out += struct.pack("<H", BANDS[band] | bit)
            out += at.pack(u, SPEECH_PARAMS)
    return out


def dmnr_unit(xmldir):
    at = AudioType(xmldir, "SpeechDMNR")
    bands, profiles = at.categories["Band"], at.categories["Profile"]
    out = unit_hdr(len(profiles), len(bands), 3, 0xaa03)
    for i, band in enumerate(bands):
        for j, prof in enumerate(profiles):
            out += struct.pack("<H", (i + 1) << 4 | (j + 1))
            out += at.pack(at.unit([band, prof]), ("dmnr_para",))
    return out


def common_unit(xmldir, name, params, magic):
    at = AudioType(xmldir, name)
    out = struct.pack("<8H", 1, 1, 1, 1, 0, 0, 0, magic)
    out += struct.pack("<H", 0xf)
    return out + at.pack(at.unit(["Common"]), params)


def build(xmldir, profile="Normal", volume=3, mode=0):
    units = [speech_unit(xmldir, profile, volume, mode), dmnr_unit(xmldir),
             common_unit(xmldir, "SpeechGeneral",
                         ("speech_common_para", "debug_info"), 0xaa02),
             common_unit(xmldir, "SpeechMagiClarity",
                         ("shape_rx_fir_para",), 0xaa04)]
    return b"".join(units), [len(u) for u in units]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xmldir")
    ap.add_argument("--profile", default="Normal")
    ap.add_argument("--volume", type=int, default=3)
    ap.add_argument("--mode", type=int, default=0,
                    help="0 handset, 1 headset, 2 speaker, 3 BT")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    blob, sizes = build(a.xmldir, a.profile, a.volume, a.mode)
    print("speech %d, dmnr %d, general %d, magiclarity %d: %d bytes" % (
        *sizes, len(blob)), file=sys.stderr)
    if a.out:
        open(a.out, "wb").write(blob)


if __name__ == "__main__":
    main()
