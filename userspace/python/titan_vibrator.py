"""The Titan Pocket's vibration motor, on both kernels.

pulse(ms) starts one pulse and returns at once. On 4.14 that is MediaTek's
vibrator LED (/sys/class/leds/vibrator: duration, then activate). On Linux
6.12 it is upstream's regulator-haptic, an input device with force feedback:
a rumble effect of that length is uploaded (EVIOCSFF) and played (an EV_FF
event). An effect lives as long as the file that uploaded it, so the device
stays open for the life of the program, with one effect reused.
"""

import fcntl
import os
import struct

LED = "/sys/class/leds/vibrator/"
FF_NAME = "regulator-haptic"
FF_RUMBLE = 0x50
EV_FF = 0x15
EVIOCSFF = 0x40304580       # _IOW('E', 0x80, struct ff_effect), 48 bytes on arm64
# struct ff_effect: type, id, direction, trigger (2 x u16), replay (length,
# delay), padding, the rumble magnitudes (strong, weak), the union's rest
FF_EFFECT = struct.Struct("<HhHHHHH2xHH28x")
INPUT_EVENT = struct.Struct("<qqHHi")   # timeval, type, code, value

_fd = None
_id = -1


def _ff_device():
    for name in sorted(os.listdir("/sys/class/input")):
        try:
            with open("/sys/class/input/%s/device/name" % name) as f:
                if name.startswith("event") and f.read().strip() == FF_NAME:
                    return "/dev/input/" + name
        except OSError:
            pass
    return None


def pulse(ms):
    """one pulse of ms milliseconds (at most 15 s); errors are ignored"""
    global _fd, _id
    ms = max(1, min(int(ms), 15000))
    try:
        if os.path.isdir(LED):
            with open(LED + "duration", "w") as f:
                f.write(str(ms))
            with open(LED + "activate", "w") as f:
                f.write("1")
            return
        if _fd is None:
            dev = _ff_device()
            if dev is None:
                return
            _fd = os.open(dev, os.O_RDWR | os.O_CLOEXEC)
        eff = bytearray(FF_EFFECT.pack(FF_RUMBLE, _id, 0, 0, 0, ms, 0, 0xffff, 0xffff))
        fcntl.ioctl(_fd, EVIOCSFF, eff)
        _id = struct.unpack_from("<h", eff, 2)[0]
        os.write(_fd, INPUT_EVENT.pack(0, 0, EV_FF, _id, 1))
    except OSError:
        pass
