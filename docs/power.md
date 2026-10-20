# Power management

Status (2026-10-20): CPU frequency scaling works (it was pinned at the top
OPP). The GPU powers off when idle. The power button blanks the panel and
suspends to RAM (Wi-Fi stays up by default, see below); Weston stops drawing
while the screen is off. Suspend and power-key wake work, including with
Weston running. The occasional reset during sleep was a deliberate kernel
oops, now a refused suspend (kernel #33, [below](#the-suspend-crash)); a
rarer hang while bringing CPUs back on resume is not understood yet.

## CPU frequency

The BSP's cpufreq driver is `mt-cpufreq` (two clusters: LL 793 MHz – 1.99
GHz, L 793 MHz – 2.11 GHz, 16 OPPs each) with the **schedutil** governor.
MediaTek's PPM (`/proc/ppm`) combines limits from several policies (thermal,
DLPT battery limit, user limit, ...). Kernel drivers request floors and
ceilings through `cpu_ctrl` "KIRs" (`/proc/perfmgr/boost_ctrl/cpu_ctrl/current_freq`
shows the aggregate).

### The CPUs were pinned at maximum

`scaling_min_freq == scaling_max_freq` on both clusters: PPM's
`USER_LIMIT` policy held min OPP index 0. The requester was **USB boost**
(`drivers/misc/mediatek/usb_boost`, KIR `CPU_KIR_USB`):
- on any USB traffic it asks for a 2.5 GHz floor, all cores online and DRAM
  at OPP 0, held for 5 s;
- with SSH over USB networking that was permanent;
- its sysfs switches were created read-only (0400).

Kernel #28: off by default on MT6771 (`usb_boost_plat.c`, first parameter
field = enable). The switches are now root-writable
(`/sys/class/usb_boost/{cpu_freq,cpu_core,dram_vcore}/enable`).

### A kworker used a whole CPU (USB gadget Ethernet)

`eth_work` (`drivers/usb/gadget/function/u_ether.c`) ran **~51,000 times/s**
on CPU 0 whenever USB networking was up:
- `rx_fill()` submits rx requests until one fails, then reschedules itself;
- MediaTek's `u_ether` keeps more rx requests than mtu3's descriptor ring
  holds, so one submission always failed with `-EAGAIN` (ring full,
  `mtu3_prepare_transfer()`) and the work requeued forever.

Kernel #28 reschedules only on `-ENOMEM`; completions refill a full ring
anyway. Afterwards: ~70 work items/s system-wide at idle, and USB networking
still moves ~23 MB/s.

The load average stays high (~20) even at idle. That is cosmetic: many
MediaTek kernel threads (`wdtk-*`, `gauge_timer_thr`, `hang_detect`,
`power_misc_thre`, ...) sleep in uninterruptible state (`D`), which Linux
counts as load.

How these were found: `top`, then
`/sys/kernel/debug/tracing/events/workqueue/workqueue_execute_start` (tally
`function ...`), then `events/gadget/usb_ep_queue` (status `-11`).

## CPU idle

`mtk_acao_cpuidle` driver, two states per CPU (WFI and MediaTek's deeper
MCDI). Not tuned further yet.

## Suspend to RAM

`/sys/power/mem_sleep` = `deep`. `echo mem > /sys/power/state` goes through
MediaTek's SPM (`[SLP] Chip_pm_begin`):
- user space freezes;
- all devices suspend (display, Wi-Fi/WMT, Bluetooth, GPU, battery, thermal,
  PPM, mtu3 USB);
- resume works.

**With a USB cable connected, suspend is aborted** at the noirq stage:
`pm_system_irq_wakeup: 197 triggered usbcd_eint` (cable detect). This is
expected; the phone only deep-sleeps unplugged.

Before suspending, the panel must be blanked: writing `4`
(`FB_BLANK_POWERDOWN`) to `/sys/class/graphics/fb0/blank` runs MediaTek's
`primary_display_suspend` (panel and backlight off) and releases
`pri_disp_wakelock`; `0` turns it back on. The harmless warning `disable
regulator lcm_ldo fail` appears there.

mtkfb resets its pixel format to its native order on unblank. The KMS driver
(`mtkfb_kms`) now re-applies XRGB8888 before every frame. Without that, red
and blue were swapped in Weston after a resume.

Test without the power key:
```sh
echo 4 > /sys/class/graphics/fb0/blank; rtcwake -m mem -s 15; echo 0 > /sys/class/graphics/fb0/blank
```

## GPU power (Panfrost)

Panfrost 5.10 never powered the GPU down: its suspend hook only calls
`panfrost_gpu_power_off()`, which is a no-op, so MFG (MTCMOS, clock) stayed
on. A suspend with Weston running then hung in SPM deep sleep (hardware
watchdog reset, `/proc/aed/reboot-reason`: WDT status 2).

Now (`panfrost_mtk.c`, `panfrost_device.c`, `panfrost_drv.c`):
- **Runtime suspend** (~50 ms after the last job): MFG idle wait, clock gate
  and MTCMOS off, like kbase's `pm_callback_power_off`. Runtime resume
  powers it back on and fully resets the GPU.
- **System suspend**: additionally the VGPU/VSRAM bucks off (kbase
  `pm_callback_power_suspend`). Buck and MFG state are tracked, so regulator
  use counts stay balanced.
- `panfrost_remove()` took a runtime-PM reference it never dropped (as in
  upstream 5.10). The count lives in the platform device, so every module
  reload left the GPU permanently "in use". Balanced now.

Verified: GPU `runtime_status` = suspended and `pg_mfg*` domains off at
idle; 30 power cycles pixel-exact; suspend/resume with Weston running.

## What wakes it from sleep

Unattended tests (`rtcwake -m mem -s 30` × 5 per case, cable unplugged,
`[SPM] wake up by` from the log):

| Case | Sleep length | Woken by |
|---|---|---|
| Wi-Fi associated, Bluetooth on | 0.2–0.5 s | `R12_CONN2AP_SPM_WAKEUP_B` (connectivity chip) |
| Bluetooth off | 0.5–1.3 s | CONN2AP |
| Wi-Fi driver suspend mode (`SETSUSPENDMODE 1`) | 0.4–0.7 s, occasionally 30 s | CONN2AP |
| **Wi-Fi down** + Bluetooth off | **30 s every time** | RTC alarm (`mt6358` PMIC EINT) |

So deep sleep works, but an associated Wi-Fi link wakes the SoC
continuously, even with the gen3 driver's suspend-mode packet filter
(`titan-wlan-cmd SETSUSPENDMODE 1`; Android sends it with wpa_supplicant's
`DRIVER` command).

Taking `wlan0` down for sleep (`TITAN_SLEEP_WIFI=off` in titan-powerkey)
gives full-length sleeps in the unattended test, which also had Bluetooth
off. In real use, with Bluetooth on, it **hung the phone once** (hardware
watchdog reset; `SPM Suspend debug = 0x0`, so before SPM took over). So the
default keeps Wi-Fi up: sleep works and the power key wakes it reliably,
but the connectivity chip wakes the SoC every ~0.5 s. Next step: unattended
tests of Wi-Fi down with Bluetooth on, logging each step to a synced file.

`titan-wlan-cmd [-i wlan0] CMD ...` sends any gen3 driver private command
(ioctl `SIOCDEVPRIVATE+1`, `struct priv_driver_cmd_s`).

## The suspend crash

The "watchdog resets" during sleep (2026-10-18/19) were a deliberate kernel
oops. Before the SoC sleeps, MediaTek's `slp_suspend_ops_enter()` calls
`subsys_if_on()` (`clk-mt6771-pg.c`), which checks the MTCMOS power domains
and did `BUG_ON(1)` if the display, GPU (MFG), ISP, camera, video or VPU
domain was still on; the panic handler then reset the phone. pstore had it:
`Internal error: Oops - BUG` in titan-powerkey's `pm_suspend` →
`slp_suspend_ops_enter` → `subsys_if_on`. Which domain was on that time fell
into a gap in the saved log.

Kernel #33: the check sets `slp_subsys_busy` instead, and suspend entry
refuses (-EBUSY) with "CANNOT SLEEP: N power domain(s) still on"; the
"suspend warning: SYS_xxx is on!!!" lines before it name them.
titan-powerkey treats EBUSY as "retry in 30 s". The next time it happens the
journal says which driver to fix.

Separately, on 2026-10-18 21:40 a resume stopped while bringing CPU1 back
online (the journal's last line `Detected VIPT I-cache on CPU1`) with no
pstore record: a real hang, not understood yet.

## Log volume

Persistent, the journal grew to 4 GB in a week: ~1,700 lines a minute, 92 %
kernel, much of it MediaTek debug output logged as err/warn/notice (so no
level filter separates it). Now:
- the journal is in RAM, 96 MB ([debugging.md](debugging.md));
- **Weston draws nothing while the screen is off**: the shell watches
  `/run/titan-screen.off` (inotify) and keeps the compositor offscreen while
  it exists (also after input or the idle timer). Before, every commit became
  a refused display update, two error lines each, several per second, after
  the GPU had rendered it (20 s off: ~70 → 0 lines, apart from one burst
  after a touch);
- the Wi-Fi driver logs warnings and errors only (`titan-wifi-on` writes
  `0xff:0x03` to `/proc/net/wlan/dbgLevel`; the default 0x27 adds INFO and
  STATE), and the firmware's own text log only at RX INFO (driver patch);
- kernel #34: no SODI line for each non-timer wake (a summary once a
  minute), PMIC IRQ enable/disable and SD clock changes at debug.
Still there: the watchdog kicker (`[wdk-c]`, ~28 a minute; its records
pointed at the crash), SCSI cache syncs, battery ADC readings.

## Power button: `titan-powerkey`

`/usr/local/sbin/titan-powerkey` (`titan-powerkey.service`) reads
`KEY_POWER` from `mtk-kpd`:

| State | Event | Action |
|---|---|---|
| awake | short press (< 1.5 s) | blank panel, `echo mem > /sys/power/state` |
| suspend refused (USB cable, ...) | — | stay "screen off", retry every 30 s |
| screen off | short press | panel on |
| asleep | power key | wakes; the key press is swallowed, panel on |
| asleep | any other wake (RTC, cable plugged in, ...) | straight back to sleep |

The **volume keys** (same `mtk-kpd` device) step the speaker amp
(`Speaker Amp Volume`, 0–255) by 8 per press or autorepeat, capped at 224
(never full blast), only while the screen is on. The level is kept in
`/var/lib/titan-audio/volume` and applied when the daemon starts (default
160; the amp otherwise boots at 255). While `/run/titan-volume-keys.grab` exists, an app
owns the keys (`titan-remote` sends the TV's volume) and the phone's volume
is left alone.

**Screenshot:** the red key on the left side (`KEY_RED`, 250: `0xfa` in the
stock device tree's `mediatek,kpd-hw-init-map`, on the same `mtk-kpd`
device) together with volume up, in either order, while the screen is on.
`fbgrab` copies the frame on the panel and it's saved in the background as
`~/Pictures/Screenshots/Screenshot_<date>_<time>.png` (written to a dot
file, then renamed, so Photos never sees half a file). As a shutter the
backlight dips to a fifth for 120 ms, then `titan-brightness apply` puts
the saved level back. While the red key is held, volume up doesn't change
the volume; if volume up came first, its step (made under 2 s before) is
undone.

Long presses are ignored; holding the key for ~10 s makes the PMIC reset
the phone. systemd-logind would otherwise act on the key (the device is
tagged `power-switch`, and logind's default is **poweroff**), so
`/etc/systemd/logind.conf.d/titan-powerkey.conf` sets `HandlePowerKey=ignore`.

## Not done

- Connected standby: find which frames still wake the host in Wi-Fi suspend
  mode (DTIM skipping, ARP offload, multicast filters).

- Wake on keyboard/touch (only the power key, RTC and cable wake it now).
- The resume hang (CPU1 coming online, no pstore record) and which power
  domain refused suspend, once kernel #33 logs it.
- DPMS in the KMS driver (titan-powerkey blanks through mtkfb; Weston idle
  ([core] idle-time) locks and asks titan-powerkey for the screen off, and
  stays offscreen while it's off).
- Measured current draw in suspend.
- cpuidle/hotplug tuning, thermal limits.
