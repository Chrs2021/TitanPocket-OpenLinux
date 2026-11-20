#!/usr/bin/env python3
"""
extract-patch.py: copy the GNSS DSP's firmware patch out of your own phone's
libmnl.so (docs/gnss-open.md). The receiver takes it at the start of every
session; it ships only inside MediaTek's library, so it is never in this
repository. With the patch extracted, libmnl.so itself is not needed.

    tools/gnss/extract-patch.py VENDOR/lib/libmnl.so mt6631-gnss-patch.bin
    scp mt6631-gnss-patch.bin root@PHONE:/lib/firmware/titan/

In libmnl.so the patch is an 8-byte header, [u16 size in 16-bit words]
[u16 0x8000] [u16 0x8004] [u16 0x007B], then the image. The library holds
several images with the same header; the Titan Pocket's is recognised by its
CRC-32. The output is the header and the image, as titan-gps-probe
--extract-patch writes it.
"""
import sys
import zlib

IMAGE_CRC32 = 0x5A7874A9        # the image this phone's GNSS DSP takes (61440 bytes)
IMAGE_OFFSET = 0x3E0106         # where it was in the libmnl.so this was worked out on
MARK = bytes([0x00, 0x80, 0x04, 0x80, 0x7B, 0x00])  # the header after its size field


def image_at(lib, at):
    """the header and image starting at `at` (the image), if it is the one"""
    if at < 8:
        return None
    header = lib[at - 8:at]
    if header[2:] != MARK:
        return None
    size = int.from_bytes(header[:2], "little") * 2
    image = lib[at:at + size]
    if len(image) != size or zlib.crc32(image) != IMAGE_CRC32:
        return None
    return header + image


def find(lib):
    found = image_at(lib, IMAGE_OFFSET)
    i = lib.find(MARK)
    while found is None and i >= 0:
        found = image_at(lib, i + len(MARK))
        i = lib.find(MARK, i + 1)
    return found


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split("\n\n")[1])
    lib = open(sys.argv[1], "rb").read()
    patch = find(lib)
    if patch is None:
        sys.exit(f"{sys.argv[1]}: no MT6631 GNSS patch with CRC-32 {IMAGE_CRC32:08x}")
    open(sys.argv[2], "wb").write(patch)
    print(f"{sys.argv[2]}: {len(patch) - 8} bytes of patch image and its 8-byte header")


if __name__ == "__main__":
    main()
