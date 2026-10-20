# Debugging

## Getting logs

**Crash logs survive reboot.** The stock dtb reserves a pstore/ramoops region
(`ramoops.mem_address=0x54410000`), so after a panic or watchdog reset the
previous boot's console is readable on the next boot:

```sh
mount -t pstore pstore /sys/fs/pstore; cat /sys/fs/pstore/console-ramoops*
```
`tools/device/capture_log.py` does this automatically for every boot it sees
(writes `logs/boot-*.log`). This is how the ~19 s bootloop was diagnosed.
`PANIC_TIMEOUT=1`, so a panic reboots after one second; the bootloader then
reports `androidboot.bootreason=kernel_panic` on the next command line. A plain `fastboot reboot` shows up as `PMIC_cold_reboot` (pstore is empty after it), so don't mistake a flash for a crash.

On Debian, **systemd-pstore** collects them at boot: the previous crash's
console is in `/var/lib/systemd/pstore/console-ramoops*` (2026-10-20: this
found the suspend `BUG_ON`, [power.md](power.md#the-suspend-crash)). Check
`/proc/aed/reboot-reason` too (`WDT status: 2` and all SPM fields 0 = a
reset outside the sleep firmware).

**The journal is in RAM** (`/etc/systemd/journald.conf.d/titan.conf`:
`Storage=volatile`, 96 MB in `/run/log/journal`): nothing reaches the flash,
and it starts empty at every boot, so `journalctl -b -1` shows nothing.
Crash evidence comes from pstore above. To keep a journal across reboots
while chasing something, set `Storage=persistent` there and restart
`systemd-journald`. (Persistent, it had grown to 4 GB in a week: ~2.4
million lines a day, mostly MediaTek kernel debug output; see
[power.md](power.md#log-volume).)

**Live:** `sh_cmd.py 'dmesg | tail'`, or `telnet 172.16.42.1`.

## Useful interfaces

| Path / tool | What |
|---|---|
| `/sys/kernel/debug/mtksocaudio` | full MediaTek AFE register dump |
| `devmem 0x11220000+off 32` | AFE registers live (DL1 base/cur/end at 0x40/0x44/0x48, I2S3 at 0x4C) |
| `/proc/asound/card0/pcmNp/sub0/{status,hw_params}` | stream state, hw/appl pointers |
| `/proc/interrupts` | e.g. `Afe_ISR_Handle`, `aw9523_irq` counts |
| `/proc/mtkfb` | display debug commands, e.g. `echo helper:DISP_OPT_IDLE_MGR,0 > /proc/mtkfb` |
| `/sys/devices/platform/1000b000.pinctrl/mt_gpio` | pin mode/dir/in/out for every GPIO |
| `i2cget -f -y <bus> <addr> <reg> w` | read device registers next to a bound driver (`-f`) |

## Gotchas we hit (read these)

- **BusyBox `i2cget` parses the register argument as hex** even without
  `0x`: `i2cget ... 98` reads register 0x98. Always pass `0x..`.
- **SMBus words are little-endian**; the FS1860's registers are big-endian,
  so `i2cget` shows `0x0358` for a register value of `0x5803`.
- **Never use a bare `wait`** in a serial-shell command while `spk-hold` (or
  anything long-running) is in the background: it waits forever and the
  serial shell appears dead. Wait on specific PIDs. Recovery: telnet in and
  `kill` the stuck processes.
- **`tinyplay` with raw input sizes the input by file length**: `/dev/zero`
  and stdin play nothing.
- **pcm-probe/tinyplay "playing" 3 s of audio in 0.13 s** means writes are
  not being paced by the hardware — see the tinyalsa and DL copy bugs in
  [audio.md](audio.md).
- **Two transducers.** The earpiece and the loudspeaker can both be fed from
  DL1. Confirm which one is making sound.
- **Amp "on" isn't "playing".** FS1860 `0x00 = 0x005F` (PLL locked) can
  coexist with a silent DSP (`0xBD = 0x0803`). Look for `0xBD = 0x5803`.
- **`merge_config.sh` inside the container leaves 0-byte `sedXXXXXX` files**
  in the kernel tree (mode 000, deletable only from the Mac side).
- **The phone's USB re-enumerates** on every reboot; scripts must wait for
  `/dev/cu.usbmodemtitanpocket*` (and give DHCP a few seconds for the network).

## Comparing against stock Android

Many questions are fastest to answer by observing stock Android on the same
phone: flash `backups/boot_device_magisk_20261010.img`, `adb shell su`, and
read the hardware directly. The amp configuration was captured this way
(`userspace/build/android/fsdump` over `/dev/smartpa_i2c`). Magisk's
**Superuser access** must be set to *Apps and ADB*; if the Magisk app hangs
at its splash screen, `adb shell pm clear com.topjohnwu.magisk` fixes it
without affecting root. Stock Android has debugfs disabled, so AFE dumps are
only available on our kernel.
