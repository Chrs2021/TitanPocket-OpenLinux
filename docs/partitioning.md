# Storage and partitioning

## UFS layout

The 128 GB UFS exposes three logical units: `sda`, `sdb` (4 MiB each,
boot LUs) and **`sdc`** (LU2, 119.2 GiB, **4096-byte sectors**), which holds
the GPT with all 43 partitions (`/sys/block/sdc/sdcN/uevent` has `PARTNAME`).

On 2026-10-11 `userdata` was split to make room for a Linux root:

| # | Name | First LBA | Last LBA | Size |
|---|---|---|---|---|
| 38 | super | 161792 | 1341439 | 4.5 GiB |
| 39 | cache | 1341440 | 1452031 | 0.42 GiB |
| 40 | apd | 1452032 | 1456127 | 16 MiB |
| 41 | **userdata** | 1456128 | 14458623 | **49.6 GiB** (was 113.6 GiB, ending at 31236087) |
| 43 | **linux** | 14458624 | 31235839 | **64 GiB**, Linux filesystem type GUID |
| 42 | flashinfo | 31236088 | 31240183 | 16 MiB (unchanged; physically last) |

(LBAs are 4 KiB.) `linux` is `/dev/sdc43`: ext4, label **`titanroot`**,
created with `mke2fs -t ext4 -L titanroot -O ^orphan_file,^metadata_csum_seed`
— e2fsprogs 1.47 enables `orphan_file` by default, which a 4.14 kernel cannot
mount read-write.

## Android after the split

Android's `userdata` is fully encrypted (metadata encryption + FBE), so its
contents could not be preserved and are now invalid. Android itself (system,
vendor in `super`) is untouched; when booted it will need `/data` formatted
(factory reset from recovery, or `fastboot -w`). It then uses the 49.6 GiB
`userdata`. The `linux` partition is invisible to Android.

## How the change was made

`tools/gpt/titan_gpt.py` edits offline copies of the GPT:

```sh
# on the phone: dump primary (LBA 0-5) and backup (last 16 LBAs) GPT
dd if=/dev/sdc of=/tmp/gpt_head.bin bs=4096 count=6
dd if=/dev/sdc of=/tmp/gpt_tail.bin bs=4096 skip=$((31240192-16)) count=16
# on the Mac
tools/device/pull_net.sh /tmp/gpt_head.bin backups/gpt_sdc_head_20261011.bin
python3 tools/gpt/titan_gpt.py show  head.bin tail.bin
python3 tools/gpt/titan_gpt.py split head.bin tail.bin new_head.bin new_tail.bin 64
```
`split` validates both headers and entry arrays (CRCs, primary == backup),
shrinks `userdata`, adds `linux` (1 MiB aligned, ending where `userdata`
ended), rewrites CRCs and re-validates the result. Only four sectors differ
(primary header + entry block 2, and the same two in the backup). Those four
were written with `dd ... conv=notrunc,fsync`, read back and compared, then
`blockdev --rereadpt /dev/sdc` made the kernel pick up `sdc43` live.

## Reverting

The original GPT is in `backups/gpt_sdc_head_20261011.bin` and
`backups/gpt_sdc_tail_20261011.bin`. From our kernel:

```sh
tools/device/push_net.sh backups/gpt_sdc_head_20261011.bin /tmp/h.bin
tools/device/push_net.sh backups/gpt_sdc_tail_20261011.bin /tmp/t.bin
# on the phone
dd if=/tmp/h.bin of=/dev/sdc bs=4096 count=6 conv=notrunc,fsync
dd if=/tmp/t.bin of=/dev/sdc bs=4096 seek=$((31240192-16)) count=16 conv=notrunc,fsync
blockdev --rereadpt /dev/sdc
```
Then format `userdata` from Android recovery. If the phone cannot boot to our
kernel, the same images can be written from BROM mode with mtkclient.

## Tools on the device

The ramdisk carries static e2fsprogs 1.47.1 (`mke2fs`/`mkfs.ext4`,
`e2fsck`/`fsck.ext4`, `tune2fs`, `resize2fs`), built by `userspace/build.sh`
from `userspace/third_party/e2fsprogs` (tarball + sha256 in `third_party/dl`).

## Other storage

A ~30 GB FAT32 microSD card (`/dev/mmcblk0p1`) is present and untouched.
