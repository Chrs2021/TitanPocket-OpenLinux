#!/usr/bin/env python3
"""Prototype modem daemon on /dev/titan-modem (the titan_modem driver):
answers the modem's RPC requests as the vendor kernel's port_rpc.c does,
serves its file requests from a mock file system (a scratch directory, see
"mock file service"), and logs the rest, to learn what the modem asks for
while it boots. Usage: mdd.py [seconds, 0: until stopped]"""
import fcntl
import mmap
import os
import re
import select
import signal
import struct
import sys
import time

CCCI_CONTROL_RX = 0
CCCI_PCM_RX, CCCI_PCM_TX = 4, 5         # speech control (Android: ccci_aud)
CCCI_UART2_RX, CCCI_UART2_TX = 10, 12   # the AT (MUX) channel
CCCI_FS_RX, CCCI_FS_TX = 14, 15
CCCI_RPC_RX, CCCI_RPC_TX = 32, 33
RPC_API_RESP_ID = 0xFFFF0000
IPC_RPC_GET_EINT_ATTR_OP = 0x4005
ERR_SIM_HOT_PLUG_QUERY_STRING = -15     # no hot-plug node in the board's DT
# The SIM hot-plug interrupts, by query type (port_rpc.h: number, debounce in
# ms, polarity, sensitivity, socket type, dedicated, source pin), as Android's
# kernel answers them on this phone (measured: ccci_dump's md_eint lines). The
# modem asks SIM 2 only for types 6, 0 and 5; the others are taken from SIM 1.
SIM_EINT = {
    "MD1_SIM1_HOT_PLUG_EINT": (0, 100, 0, 0, 0, 0, 1),
    "MD1_SIM2_HOT_PLUG_EINT": (1, 100, 0, 0, 0, 0, 2),
}
IPC_RPC_DTSI_QUERY_OP = 0x400e
IPC_RPC_CCCI_LHIF_MAPPING = 0x400d       # a net interface's uplink queue
# LK's /md_attr_node gives 0x0f0f0f0f for both properties the modem asks
# (md_product_name_model_id, md_drdi_rf_set_idx): the vendor's "unset" fill
MD_ATTR_VALUE = b"\x0f" * 68           # ccci_rpc_md_dtsi_output
FS_NO_OP = -1
# reply parameter counts per file operation, as learnt from the modem's
# asserts (ccci_fs.c: expected, got, op): "0x100e=2,0x1010=1"
FS_COUNTS = {int(k, 16): int(v) for k, v in
             (i.split("=") for i in os.environ.get("FS_COUNTS", "").split(",") if i)}


def rpc_params(body, n):
    """the request's parameters: each a 32-bit length, data padded to 4"""
    out, off = [], 0
    for _ in range(n):
        (ln,) = struct.unpack_from("<I", body, off)
        out.append(body[off + 4:off + 4 + ln])
        off += 4 + (ln + 3) // 4 * 4
    return out


# A request or reply larger than one packet goes in fragments: CCCI header
# word 0 bit 31 set on all but the last; each fragment's payload starts with
# the operation word, the first one's with the operation and its parameters
# (measured: a 6500-byte FS_Write came as 3476 + 3088-byte packets).
FRAG_MORE = 0x80000000
FRAG_MAX = 3476                 # the largest packet the modem sent


def rpc_reply(hdr, op, params, ch=CCCI_RPC_TX):
    """-> the reply packets"""
    d0, _, _, _, res = struct.unpack_from("<IIHHI", hdr)
    d0 &= ~FRAG_MORE
    opw = struct.pack("<I", op | RPC_API_RESP_ID)
    body = struct.pack("<I", len(params))
    for p in params:
        body += struct.pack("<I", len(p)) + p + b"\0" * (-len(p) % 4)
    room = FRAG_MAX - 16 - 4
    pkts = []
    while True:
        chunk, body = body[:room], body[room:]
        more = FRAG_MORE if body else 0
        # the driver sets the sequence number; the buffer index stays
        pkts.append(struct.pack("<IIHHI", d0 | more, 16 + 4 + len(chunk),
                                ch, 0, res) + opw + chunk)
        if not body:
            return pkts


def handle_rpc(fd, pkt):
    op, n = struct.unpack_from("<II", pkt, 16)
    params = rpc_params(pkt[24:], n)
    if op == IPC_RPC_GET_EINT_ATTR_OP:
        name = params[0].rstrip(b"\0").decode(errors="replace")
        err = struct.pack("<i", ERR_SIM_HOT_PLUG_QUERY_STRING)
        qtype = struct.unpack("<I", params[2][:4])[0] if len(params) > 2 else 99
        attrs = SIM_EINT.get(name)
        if attrs and qtype < len(attrs):
            print("rpc %#x eint %s type %d: %d" % (op, name, qtype, attrs[qtype]))
            for r in rpc_reply(pkt[:16], op, [i32(0), i32(attrs[qtype])]):
                os.write(fd, r)
            return
        print("rpc %#x eint %s type %d: none" % (op, name, qtype))
        for r in rpc_reply(pkt[:16], op, [err, err]):
            os.write(fd, r)
        return
    if op == IPC_RPC_CCCI_LHIF_MAPPING:
        # (net interface, queue): the vendor moves IMS to its own queue; here every
        # interface shares uplink queue 0
        print("rpc %#x uplink queue: %s" % (op, params[0].hex() if params else "?"))
        for r in rpc_reply(pkt[:16], op, [i32(0), i32(0)]):
            os.write(fd, r)
        return
    if op == IPC_RPC_DTSI_QUERY_OP:     # (req, index, name[64]) -> output
        print("rpc %#x dtsi %s" % (op, params[0][2:66].rstrip(b"\0")
                                   .decode(errors="replace")))
        for r in rpc_reply(pkt[:16], op, [i32(0), MD_ATTR_VALUE]):
            os.write(fd, r)
        return
    print("rpc %#x (%d params) unhandled: %s" % (op, n,
          " ".join(hexq(p, 32) for p in params)))
    err = struct.pack("<i", FS_NO_OP)
    for r in rpc_reply(pkt[:16], op, [err, err]):
        os.write(fd, r)


# NV_QUIET=1 (the phone's real NVRAM, per-phone data): no file contents
# in the log, only paths, small values and the lengths of anything larger
QUIET = bool(os.environ.get("NV_QUIET"))


def show(p):
    """a parameter: UTF-16 text if it looks like it, else hex"""
    if len(p) >= 4 and len(p) % 2 == 0 and p[1::2].count(0) == len(p) // 2:
        return repr(p.decode("utf-16-le").rstrip("\0"))
    if QUIET and len(p) > 4:
        return "<%d bytes>" % len(p)
    return p[:24].hex()


def hexq(p, n=16):
    """raw bytes for the log, or only their length when QUIET"""
    if QUIET and len(p) > 4:
        return "<%d>" % len(p)
    return p[:n].hex()


# --- mock file service -----------------------------------------------------
#
# The modem's file system (CCCI_FS, served on Android by ccci_fsd) answered
# from a scratch directory: FS_ROOT (a tmpfs directory) holds the drives as
# Z/ (nvdata md/), X/ (protect_f md/), Y/ (protect_s md/), ... The run
# script fills it: empty (the modem creates its own defaults), kept, or a
# copy of the phone's NVRAM (nvcopy.sh); the partitions are never written.
#
# Operation numbers, structures and reply shapes follow FirmWire's MediaTek
# FSD emulation (firmwire/vendor/mtk/hw/FSD.py, Team FirmWire,
# BSD-3-Clause), and were checked against what this modem asks and asserts
# (ccci_fs.c: expected reply count, sent count, op). Each request and reply
# is logged (only lengths of file contents with NV_QUIET).

ROOT = os.environ.get("FS_ROOT", "/run/titan-modem-mock")
FS_NO_ERROR = 0
FS_PARAM_ERROR = -2
FS_FILE_NOT_FOUND = -9
FS_INVALID_FILE_HANDLE = -10
FS_NO_MORE_FILES = -6
FS_GENERAL_FAILURE = -18
FS_PATH_NOT_FOUND = -19
FS_FILE_EXISTS = -36

FS_READ_ONLY = 0x100
FS_OPEN_DIR = 0x800
FS_CREATE = 0x10000
FS_CREATE_ALWAYS = 0x20000

FS_ATTR_READ_ONLY = 0x01
FS_ATTR_DIR = 0x10

# FS_CMPT_READ's steps (FSCompact)
CMPT_OPEN, CMPT_GETFILESIZE, CMPT_SEEK, CMPT_READ, CMPT_CLOSE = 1, 2, 4, 8, 16

handles = {}            # handle -> open file
finds = {}              # find handle -> remaining (name, path) entries
stats = {}              # op -> count


def i32(v):
    return struct.pack("<i", v)


def u32(p):
    return struct.unpack_from("<I", p)[0] if len(p) >= 4 else 0


def modem_name(p):
    return p.decode("utf-16-le", errors="replace").rstrip("\0")


def local(p):
    """a modem path (Z:\\A\\B) under ROOT, matching existing names
    without regard to case (the modem's FAT names)"""
    name = modem_name(p)
    path = os.path.join(ROOT, name[:1].upper())
    for part in [c for c in name[2:].split("\\") if c]:
        try:
            match = [e for e in os.listdir(path) if e.lower() == part.lower()]
        except OSError:
            match = []
        path = os.path.join(path, match[0] if match else part)
    return path


def new_handle(table, obj):
    h = 1
    while h in handles or h in finds:
        h += 1
    table[h] = obj
    return h


def fs_open(path, flags):
    if os.path.isdir(path):
        return FS_FILE_NOT_FOUND if not flags & FS_OPEN_DIR else \
            new_handle(handles, None)
    if not os.path.isdir(os.path.dirname(path)):
        return FS_PATH_NOT_FOUND
    if flags & FS_CREATE_ALWAYS:
        mode = "w+b"
    elif os.path.isfile(path):
        mode = "rb" if flags & FS_READ_ONLY else "r+b"
    elif flags & FS_CREATE:
        mode = "w+b"
    else:
        return FS_FILE_NOT_FOUND
    return new_handle(handles, open(path, mode))


def dir_entry(name, path):
    """FS_DOSDirEntry (52 bytes, FirmWire's DOSDirEntry): 8.3 name,
    attributes, times, clusters, size, then index, stamp, drive, serial"""
    base, _, ext = name.upper().partition(".")
    attr = FS_ATTR_DIR if os.path.isdir(path) else 0
    size = 0 if os.path.isdir(path) else os.path.getsize(path)
    return struct.pack("<8s3sBBBiHHiHIIIIII", base[:8].encode().ljust(8),
                       ext[:3].encode().ljust(3), attr, 0, 0, 0, 0, 0, 0, 0,
                       size, 0, 0, 0, 0, 0)


def find_reply(h, maxlen):
    if not finds.get(h):
        return [i32(FS_NO_MORE_FILES), b"\0" * 52, b""]
    name, path = finds[h].pop(0)
    fname = name.encode("utf-16-le")[:max(maxlen - 2, 0)] + b"\0\0"
    return [i32(h), dir_entry(name, path), fname]


def disk_info():
    """FS_DiskInfo: label, drive letter, write protect, serial, then the
    geometry and free-space counts the modem asked for (flags 3)"""
    st = os.statvfs(ROOT)
    clusters = st.f_blocks * st.f_frsize // 4096
    free = st.f_bavail * st.f_frsize // 4096
    return struct.pack("<24sBBH14I", b"MOCK", ord("Z"), 0, 0, 0x4d4f434b,
                       0, 32, 2, 512, 512, 8, clusters, 0, free, 0, 0, 0,
                       free)


def cmpt_read(path, block):
    """FS_CMPT_READ, a compact read: the steps in CMPTRead.Operation
    (open, size, seek, read, close) on one file. CMPTRead: Operation,
    Return0, Return1, OpenFlag, FileSize, Offset, From, Ptr, Length,
    BytesRead (the modem asks for one record after the 64-byte header).
    -> (steps done, error) as one pack, file size, bytes read, data"""
    ops, _, _, flags, _, offset, whence, _, length, _ = \
        struct.unpack_from("<IIiIIIIIII", block)
    done, size, data = 0, 0, b""

    def reply(err):
        return [struct.pack("<Ii", done, err), struct.pack("<I", size),
                struct.pack("<I", len(data)), data]

    if not ops & CMPT_OPEN:
        return reply(FS_GENERAL_FAILURE)
    h = fs_open(path, flags)
    if h < 0:
        # the name only: a failed read here is invisible in the quiet log
        print("fs cmpt read failed: %s: %d" % (os.path.relpath(path, ROOT), h))
        return reply(h)
    f = handles.pop(h)
    done |= CMPT_OPEN
    with f:
        if ops & CMPT_GETFILESIZE:
            size = os.fstat(f.fileno()).st_size
            done |= CMPT_GETFILESIZE
        if ops & CMPT_SEEK:
            f.seek(offset, whence)
            done |= CMPT_SEEK
        if ops & CMPT_READ:
            data = f.read(length)
            done |= CMPT_READ
    if ops & CMPT_CLOSE:
        done |= CMPT_CLOSE
    return reply(FS_NO_ERROR)


def fs_op(op, params):
    """-> the reply parameters"""
    path = local(params[0]) if params and len(params[0]) >= 4 and \
        params[0][1:2] == b"\0" else None
    if op == 0x1001:                        # FS_Open(name, flags)
        return [i32(fs_open(path, u32(params[1])))]
    if op == 0x1021:                        # restore (name, ...)
        # Android restores the file from the raw nvram partition's backup
        # (the BIN region); here the copy is the backup: present, success
        return [i32(FS_NO_ERROR if os.path.isfile(path)
                    else FS_FILE_NOT_FOUND)]
    if op == 0x1002:                        # FS_Seek(h, offset, whence)
        f = handles.get(u32(params[0]))
        if not f:
            return [i32(FS_INVALID_FILE_HANDLE)]
        off = struct.unpack_from("<i", params[1])[0]
        return [i32(f.seek(off, u32(params[2])))]
    if op == 0x1003:                        # FS_Read(h, size)
        f = handles.get(u32(params[0]))      # -> result, bytes read, data
        if not f:                           # (the modem checks the data's
            return [i32(FS_INVALID_FILE_HANDLE), i32(0), b""]   # length)
        data = f.read(u32(params[1]))
        return [i32(FS_NO_ERROR), i32(len(data)), data]
    if op == 0x1004:                        # FS_Write(h, data, size)
        f = handles.get(u32(params[0]))      # -> result, bytes written
        if not f:
            return [i32(FS_INVALID_FILE_HANDLE), i32(0)]
        f.write(params[1])
        f.flush()
        return [i32(FS_NO_ERROR), i32(len(params[1]))]
    if op == 0x1005:                        # FS_Close(h)
        f = handles.pop(u32(params[0]), None)
        if f:
            f.close()
        return [i32(FS_NO_ERROR)]
    if op == 0x1007:                        # FS_CreateDir(name)
        if os.path.exists(path):
            return [i32(FS_FILE_EXISTS)]
        if not os.path.isdir(os.path.dirname(path)):
            return [i32(FS_PATH_NOT_FOUND)]
        os.mkdir(path)
        return [i32(FS_NO_ERROR)]
    if op == 0x1009:                        # FS_GetFileSize(h)
        f = handles.get(u32(params[0]))
        if not f:
            return [i32(FS_INVALID_FILE_HANDLE), i32(0)]
        return [i32(FS_NO_ERROR), i32(os.fstat(f.fileno()).st_size)]
    if op == 0x100e:                        # FS_GetDiskInfo(drive, flags)
        return [i32(FS_NO_ERROR), disk_info()]
    if op == 0x100f:                        # FS_Delete(name)
        if not os.path.isfile(path):
            return [i32(FS_FILE_NOT_FOUND)]
        os.remove(path)
        return [i32(FS_NO_ERROR)]
    if op == 0x1010:                        # FS_GetAttributes(name)
        if os.path.isdir(path):
            return [i32(FS_ATTR_DIR)]
        return [i32(0 if os.path.isfile(path) else FS_FILE_NOT_FOUND)]
    if op == 0x1012:                        # FS_FindFirst(pattern, ...)
        d, pat = os.path.split(path)
        if not os.path.isdir(d):
            return [i32(FS_PATH_NOT_FOUND), b"\0" * 52, b""]
        import fnmatch
        hits = [(e, os.path.join(d, e)) for e in sorted(os.listdir(d))
                if fnmatch.fnmatch(e.lower(), pat.lower())]
        if not hits:
            return [i32(FS_NO_MORE_FILES), b"\0" * 52, b""]
        h = new_handle(finds, hits)
        return find_reply(h, u32(params[-1]) if len(params) > 3 else 512)
    if op == 0x1013:                        # FS_FindNext(h, ..., maxlen)
        h = u32(params[0])
        r = find_reply(h, u32(params[-1]))
        if r[0] == i32(h):
            r[0] = i32(FS_NO_ERROR)
        return r
    if op == 0x1014:                        # FS_FindClose(h)
        finds.pop(u32(params[0]), None)
        return [i32(FS_NO_ERROR)]
    if op == 0x1022:                        # FS_CMPT_READ(name, CMPTRead)
        return cmpt_read(path, params[1])
    return None


def handle_fs(fd, pkt):
    op, n = struct.unpack_from("<II", pkt, 16)
    try:
        params = rpc_params(pkt[24:], n)
    except struct.error:
        d0, d1, ch, seq, res = struct.unpack_from("<IIHHI", pkt)
        print("fs %#x: %d params do not fit in %d bytes: d %08x %08x ch %d "
              "seq %#x res %08x: %s" % (op, n, len(pkt), d0, d1, ch, seq, res,
                                       hexq(pkt[16:96], 80)))
        return
    stats[op] = stats.get(op, 0) + 1
    try:
        reply = fs_op(op, params)
    except (OSError, IndexError, struct.error) as e:
        print("   ! %r" % e)
        reply = [i32(FS_PARAM_ERROR)]
    if reply is None:                       # not understood yet
        reply = [i32(FS_NO_ERROR)] * FS_COUNTS.get(op, 1)
        print("fs %#x UNKNOWN: %s" % (op, " | ".join(
            show(p) if QUIET else p.hex() for p in params)))
    if stats[op] <= int(os.environ.get("FS_LOG_EACH", "40")):
        f = handles.get(u32(params[0])) if params and len(params[0]) == 4 \
            and op in (0x1002, 0x1003, 0x1004, 0x1005, 0x1009) else None
        name = " <%s>" % os.path.relpath(f.name, ROOT) if f else ""
        print("%.3f fs %#x%s: %s  [%s]" % (time.monotonic(), op, name,
              " ".join(show(p) for p in params),
              " ".join("%d:%s" % (len(p), hexq(p)) for p in params)))
        print("   -> %s" % " ".join(hexq(r) + ("..%d" % len(r) if
                                    len(r) > 16 and not QUIET else "")
                                    for r in reply))
    for r in rpc_reply(pkt[:16], op, reply, CCCI_FS_TX):
        os.write(fd, r)


def at_send(fd, cmd):
    """one AT command on the AT channel (CCCI_UART2_TX), CR terminated"""
    body = cmd.encode() + b"\r"
    os.write(fd, struct.pack("<IIHHI", 0, 16 + len(body), CCCI_UART2_TX, 0, 0)
             + body)
    print("at > %s" % cmd, flush=True)


TITAN_MD_AT_PORT = 0x7401     # _IO('t', 1): the AT channel to wwan0at0

# Speech control, as the stock audio HAL's SpeechDriverNormal sends it.
# Mailbox: a bare header, d0 0xffffffff, d1 = id << 16 | 16-bit parameter,
# reserved = 32-bit parameter. Payload: d0 0, d1 the packet length,
# reserved = id << 16 | (size + 6), then sync (AP 0xa2a2, modem 0x1234),
# data type, size (the modem adds page index and count) and the data.
# A2M ids are 0x2fxx, M2A 0xafxx; the modem acks a command with its id |
# 0x8000, and the AP acks these modem requests with id & 0x7fff:
SPH_MD_NEEDS_ACK = (0xaf78, 0xaf90, 0xafa0, 0xafb5)    # 0xafa0: modem alive
SPH_ON, SPH_OFF = 0x2f20, 0x2f21
# gains and mutes, the value in the 16-bit parameter (SetDownlinkGain ...)
SPH_DL_GAIN, SPH_UL_GAIN, SPH_UL_MUTE, SPH_DL_MUTE = 0x2f00, 0x2f01, 0x2f02, 0x2f03
SPH_UL_SRC_MUTE, SPH_DL_CODEC_MUTE = 0x2f08, 0x2f09
# sent once speech is on: "2f03:0,2f00:0" (id:parameter, hex id)
SPH_AFTER_ON = [tuple(int(v, 16 if i == 0 else 0) for i, v in enumerate(x.split(":")))
                for x in os.environ.get("SPH_AFTER_ON", "").split(",") if x]
SPH_INFO_TYPE = 0x19            # SPH_ON's payload: sph_info_t, 128 bytes
SPH_APP_LOOPBACK = 2
SPH_RATE_32K = 2                # sample rate enum: 8, 16, 32, 48 kHz


def sph_mailbox(fd, msg, p16=0, p32=0):
    os.write(fd, struct.pack("<IIHHI", 0xffffffff, msg << 16 | p16,
                             CCCI_PCM_TX, 0, p32))
    print("sph > %04x %04x %08x" % (msg, p16, p32), flush=True)


def sph_payload(fd, msg, dtype, data):
    n = len(data)
    os.write(fd, struct.pack("<IIHHIHHH", 0, 22 + n, CCCI_PCM_TX, 0,
                             msg << 16 | (n + 6), 0xa2a2, dtype, n) + data)
    print("sph > %04x type %#x %d bytes" % (msg, dtype, n), flush=True)


SHM_SIZE = 52 * 1024            # the speech shared memory (mmap of the device)
SHM_AP_FLAG = 0x20              # bit 0 formatted (the driver), bit 1 writing
SHM_PARAM_RING = 0x28           # offset, size, read and write index


def sph_param_write(fd, blob):
    """the speech parameters into the shared memory's parameter ring, as
    SpeechExtMemCCCI::writeSphParamToShareMemory: returns the index they
    start at (for speech on)"""
    shm = mmap.mmap(fd, SHM_SIZE)
    try:
        off, size, rd, wr = struct.unpack_from("<4I", shm, SHM_PARAM_RING)
        if len(blob) > size - 16 - (wr - rd) % size:
            raise ValueError("speech parameters: %d bytes, ring full" % len(blob))
        flag, = struct.unpack_from("<I", shm, SHM_AP_FLAG)
        struct.pack_into("<I", shm, SHM_AP_FLAG, flag | 2)
        n = min(len(blob), size - wr)
        shm[off + wr:off + wr + n] = blob[:n]
        shm[off:off + len(blob) - n] = blob[n:]
        struct.pack_into("<I", shm, SHM_PARAM_RING + 12, (wr + len(blob)) % size)
        struct.pack_into("<I", shm, SHM_AP_FLAG, flag & ~2)
    finally:
        shm.close()
    return wr


def sph_info(app, rate=SPH_RATE_32K, loopback=0, param=None):
    """sph_info_t: app, bt, rate enum, opendsp, parameter path (1: the
    shared memory), valid, size and index (param = (size, index)), ext
    dev, loopback flag (+0xc) and delay, echo/mic delay, mute,
    enhancement mask, ..."""
    b = bytearray(128)
    b[0], b[2], b[0xc] = app, rate, loopback
    if param:
        b[4], b[5] = 1, 1
        struct.pack_into("<HH", b, 6, *param)
    return bytes(b)


def speech_on(fd, app, loopback=0):
    """speech on (0x2f20) for an application, with the speech parameters
    of SPH_PARAM (a file from tools/modem/sphparam.py) written first"""
    param = None
    if os.path.isfile(os.environ.get("SPH_PARAM", "")):
        blob = open(os.environ["SPH_PARAM"], "rb").read()
        param = (len(blob), sph_param_write(fd, blob))
        print("sph param: %d bytes at %d" % param, flush=True)
    sph_payload(fd, SPH_ON, SPH_INFO_TYPE, sph_info(
        app, loopback=loopback, param=param,
        rate=int(os.environ.get("SPH_RATE", SPH_RATE_32K))))


def handle_sph(fd, pkt):
    d0, d1, ch, seq, res = struct.unpack_from("<IIHHI", pkt)
    if d0 == 0xffffffff:
        msg, p16 = d1 >> 16, d1 & 0xffff
        print("sph < %04x %04x %08x" % (msg, p16, res), flush=True)
        if msg in SPH_MD_NEEDS_ACK:
            sph_mailbox(fd, msg & 0x7fff, p16, res)
        if msg == SPH_ON | 0x8000:
            for m, v in SPH_AFTER_ON:
                sph_mailbox(fd, m, v)
    else:
        sync, dtype, n = struct.unpack_from("<HHH", pkt, 16)
        print("sph < %04x payload sync %04x type %#x %d bytes" % (
            res >> 16, sync, dtype, n), flush=True)


def at_mask(text):
    """AT text for the log: quoted strings and long digit/hex runs masked
    (the SIM's URCs carry the IMSI, ICCID and IMS identities)"""
    text = re.sub(r'"[^"]*"', '"*"', text)
    return re.sub(r"[0-9A-Fa-f]{6,}", "*", text)


def main():
    fd = os.open("/dev/titan-modem", os.O_RDWR)
    # seconds to run; 0: until stopped (the titan-modem service)
    secs = float(sys.argv[1] if len(sys.argv) > 1 else 30)
    end = time.time() + secs if secs > 0 else float("inf")
    limit = int(os.environ.get("MAX_PKTS", "0"))
    count = 0
    partial = {}            # channel -> the request so far
    # AT_CMDS="AT;AT+CFUN?": sent one a second, 3 s after handshake 2
    at_cmds = [c for c in os.environ.get("AT_CMDS", "").split(";") if c]
    at_next = None
    # AT_PORT=1: once the modem's AT layer is up (+EIND: 128), send the
    # AT_INIT commands (the unit sends none), each answered, then hand the AT
    # channel to the driver's WWAN port for ModemManager (after 10 s at
    # the latest)
    at_port = os.environ.get("AT_PORT") == "1"
    at_init = [c for c in os.environ.get("AT_INIT", "").split(";") if c]
    at_ready = at_wait = attached = False
    at_buf = ""
    hs2 = None
    # SPH_TEST=loopback: the modem's acoustic loopback (mic -> modem ->
    # earpiece) for SPH_SECS seconds, starting 8 s after handshake 2
    sph_test = os.environ.get("SPH_TEST")
    sph_flag = int(os.environ.get("SPH_LOOPBACK_FLAG", "3"), 0)
    sph_at = sph_off_at = None
    sph_req = []
    signal.signal(signal.SIGUSR1, lambda *_: sph_req.append("on"))
    signal.signal(signal.SIGUSR2, lambda *_: sph_req.append("off"))

    def at_next_init():
        nonlocal at_wait, attached
        if at_init:
            at_send(fd, at_init.pop(0))
            at_wait = True
            return
        fcntl.ioctl(fd, TITAN_MD_AT_PORT)
        attached = True
        print("AT channel handed to the WWAN port", flush=True)

    while time.time() < end and not (limit and count >= limit):
        if (at_port and not attached and hs2 and
                time.monotonic() > hs2 + 10):
            print("AT layer silent: handing the port over anyway", flush=True)
            at_init.clear()
            at_next_init()
        if sph_at and time.monotonic() >= sph_at:
            sph_at = None
            speech_on(fd, int(os.environ.get("SPH_APP", SPH_APP_LOOPBACK)),
                      sph_flag)
            sph_off_at = time.monotonic() + float(
                os.environ.get("SPH_SECS", "10"))
        # a call (until a call daemon does it): SIGUSR1 speech on
        # (application 0, normal), SIGUSR2 speech off
        if sph_req:
            req = sph_req.pop(0)
            if req == "on":
                speech_on(fd, 0)
            else:
                sph_mailbox(fd, SPH_OFF)
        if sph_off_at and time.monotonic() >= sph_off_at:
            sph_off_at = None
            sph_mailbox(fd, SPH_OFF)
        if at_next and time.time() >= at_next and at_cmds:
            at_send(fd, at_cmds.pop(0))
            at_next = time.time() + 1
        if not select.select([fd], [], [], 0.5)[0]:
            continue
        pkt = os.read(fd, 65536)
        count += 1
        d0, d1, ch, seq, res = struct.unpack_from("<IIHHI", pkt)
        if ch not in (CCCI_FS_RX,) or os.environ.get("PKT_LOG"):
            # no payload with NV_QUIET (the service): AT and IMS packets
            # carry the SIM's identities; AT text is logged masked below
            print("%8.3f ch %3d seq %5d d %08x %08x res %08x len %5d %s" % (
                time.monotonic(), ch, seq & 0x7fff, d0, d1, res, len(pkt),
                "" if QUIET or ch == CCCI_UART2_RX else pkt[16:64].hex()),
                flush=True)
        if ch == CCCI_CONTROL_RX and d0 == 0xffffffff and at_next is None:
            at_next = time.time() + 3       # handshake 2: the modem is up
            hs2 = time.monotonic()
            if sph_test == "loopback":
                sph_at = hs2 + 8
        if ch == CCCI_UART2_RX:
            text = pkt[16:].decode(errors="replace")
            print("at < %r" % at_mask(text), flush=True)
            if at_port and not attached:
                at_buf += text
                if not at_ready and "+EIND: 128" in at_buf:
                    at_ready, at_buf = True, ""
                    at_next_init()
                elif at_wait and ("\r\nOK\r\n" in at_buf or
                                  "ERROR" in at_buf):
                    at_wait, at_buf = False, ""
                    at_next_init()
        if ch == CCCI_FS_RX:
            # reassemble: later fragments repeat the operation word
            if ch in partial:
                partial[ch] += pkt[20:]
            else:
                partial[ch] = pkt
            if d0 & FRAG_MORE:
                continue
            pkt = partial.pop(ch)
        if ch == CCCI_PCM_RX:
            handle_sph(fd, pkt)
        elif ch == CCCI_RPC_RX:
            handle_rpc(fd, pkt)
        elif ch == CCCI_FS_RX:
            handle_fs(fd, pkt)
        sys.stdout.flush()
    print("fs ops: " + " ".join("%#x:%d" % kv for kv in sorted(stats.items())))
    for dirpath, _, files in os.walk(ROOT):
        for f in files:
            p = os.path.join(dirpath, f)
            print("file %s %d" % (os.path.relpath(p, ROOT), os.path.getsize(p)))


if __name__ == "__main__":
    main()
