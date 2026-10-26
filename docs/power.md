# Power management

Status (2026-10-20): CPU frequency scaling works (it was pinned at the top
OPP). The GPU powers off when idle. The power button blanks the panel and
suspends to RAM (Wi-Fi stays up by default, see below); Weston stops drawing
while the screen is off. Suspend and power-key wake work, including with
Weston running. The occasional reset during sleep was a deliberate kernel
oops, now a refused suspend (kernel #33, [below](#the-suspend-crash)); a
rarer hang while bringing CPUs back on resume is not understood yet. With
Wi-Fi connected the SoC now sleeps until a real event (2026-10-20: 129 of
130 s asleep; before, it was woken every ~0.4 s, ~105 mA average
overnight), see [What wakes it from sleep](#what-wakes-it-from-sleep).

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

**2026-10-20, what the radio hears:** a 60 s capture on `wlan0` (a
raw-socket counter; no tcpdump on the phone) got 3.4 frames/s, 83 % of them
broadcast ARP requests: the home router checks each of its ~29 clients
every ~10 s, and other access points ask for the router. Only
7/min are for the phone. Then IPv6 router advertisements (13/min), and four
EAPOL frames a minute, which turned out to be reconnections:
- **Wi-Fi dropped at every suspend attempt.** cfg80211 disconnects every
  interface on suspend unless WoWLAN is configured (`iw phy phy0 wowlan
  show`: disabled), and the reconnect took ~4 s after each wake. With the USB
  cable in, titan-powerkey retried a refused suspend every 30 s, so the link
  dropped twice a minute (34 drops in 31 min). Now `wowlan_triggers=any` in
  `wpa_supplicant-wlan0.conf` (the driver offers "any" and "disconnect"), and
  titan-powerkey doesn't attempt suspend while USB or AC power is online.
- **The firmware's suspend mode was never on in normal use.** titan-powerkey
  now sends `SETSUSPENDMODE 1` at screen-off and `0` at screen-on, as
  Android does: the driver hands the firmware the IPv4/IPv6 addresses (it
  answers ARP and NS itself and drops those for other hosts), notifies it
  (`CMD_ID_SET_SUSPEND_MODE`), and narrows multicast to `33:33:00:00:00:01`.
- **802.11 power save was off** (`iw dev wlan0 get power_save`);
  `titan-wifi-on` now turns it on. It didn't change the sleep lengths.
- **An NTP loop:** every resume counts as a clock change (`hrtimers_resume`
  → `clock_was_set`), so systemd-timesyncd queried its server after each
  wake; the phone was asleep again when the reply came, and the reply woke
  it: 86 cycles in 2 min, one NTP exchange each. titan-powerkey now stops
  timesyncd at screen-off and starts it at screen-on.

Unattended tests (a script stops titan-powerkey, blanks the panel, sets the
suspend mode and loops `echo mem` with a 60 s RTC alarm while the cable is
out; a raw socket logs every frame with the kernel's timestamp; the driver's
RX thread is frozen in suspend, so a frame that woke the SoC is stamped
after the resume):

| Setup | Sleeps | Median | Notes |
|---|---|---|---|
| suspend mode, BT off | 249 in 3 min | 0.41 s | `wake up by R12_CONN2AP, timer_out = 6` |
| + power save, BT on/off | 316 in 5 min | 0.40 s | a unicast UDP frame at 315 of 316 wakes |
| (trace) | 86 in 2 min | | those frames: NTP replies |
| + timesyncd stopped | 195 in 3 min | 0.4 s | 45 frames (38 RAs) for 195 wakes |

**Found (2026-10-20 morning): the firmware's own log.** A driver build that
logs every item the chip hands over (`nicRxProcessRFBs`, RX LOUD: type and
first 32 bytes) showed the first thing after 74 of 76 wakes: a type-7 event,
ID `0x27`, text `TX_DIAG: 5G Force Short Slot`. The firmware prints that once
per beacon on 5 GHz (~10/s) and forwards its log to the host as events; each
woke the SoC (`timer_out` 5 ticks: the event was already pending). The
driver only sent the firmware a log level when it was above OFF, so the
firmware kept its own; and a level sent at probe doesn't survive leaving
suspend mode (the messages start again at `SETSUSPENDMODE 0`). Driver
changes (`modules/connectivity/wlan_gen3`):
- `gl_init.c`: the firmware log level (OFF) is sent at probe even when OFF,
  and again on every switch to suspend mode (`wlanSetSuspendMode`), which
  titan-powerkey does at screen-off. Awake on the cable: 0 messages in 20 s
  with suspend mode on, ~7 per 10 s with it off.
- `config.h`: `CFG_SUPPORT_DROP_ALL_MC_PACKET 1`. The home router sends IPv6
  router advertisements every ~3 s and the suspend white list let them
  through (19 wakes in 62 s once the log was quiet). Asleep, the phone now
  misses RAs (addresses outlive a night) and IPv6 neighbour solicitations
  (the router may not reach it over IPv6 until it wakes; IPv4 ARP is still
  answered by the firmware).
- `debug.c` (earlier): the firmware text log is printed only at RX INFO.

| Setup | Sleeps | Median | Asleep |
|---|---|---|---|
| power save + suspend mode, timesyncd stopped | 195 in 3 min | 0.4 s | ~2/3 (incl. resume work) |
| + firmware log OFF at probe | 22 in 62 s | 2.9 s | 55 of 62 s (when it stuck) |
| + multicast dropped | 110 in 90 s | 0.4 s | the log came back |
| + log OFF at each suspend mode | **3 in 130 s** | **60 s** (the test's RTC alarm) | **129 of 130 s** |

`TX_DIAG` strings are readable in `WIFI_RAM_CODE_6759` (plaintext section 3);
the firmware has a whole family of them (register dumps, queue lengths).

**Correction (2026-10-25): the log level wasn't the fix.** In real use (18
minutes unplugged, titan-powerkey in charge) the phone still averaged 142 mA:
559 suspends with a 0.4 s median, all woken by CONN2AP. With the driver's RX
module at INFO the reason was plain: `TX_DIAG: 5G Force Short Slot` came
~98 times per 10 s with suspend mode on and 0 times with it off, and
re-sending log level OFF didn't change that. On 5 GHz the short slot time is
mandatory, but the home access point leaves the Short Slot Time bit clear in
its beacons' capability field. The driver passed that bit on
(`ucUseShortSlotTime` in the RLM parameters). In suspend mode the firmware
handles beacons itself, finds the mismatch, forces short slot and logs it,
once per beacon. Fix (`mgmt/rlm.c`, `rlmFillSyncCmdParam`): on 5 GHz always
send short slot. Afterwards 0 messages in 30 s in suspend mode, and real use
went to 24 suspends in 20 minutes (92 % asleep, sleeps up to 5.5 minutes),
113 mA.

### The sleep floor (2026-10-25/26)

113 mA while asleep 92 % of the time meant the floor itself was high.
`tools/power/sleepfloor.py` measures it: it waits for the cable to come out,
stops titan-powerkey, then keeps the phone in forced suspend (300 s RTC
alarm, re-suspending at once after any other wake) for a set time per phase,
reading the gauge's coulomb counter (0.5 mAh steps) across each phase.

| Phase (forced sleep, > 99 % asleep) | First run | After the fixes below |
|---|---|---|
| Wi-Fi connected + Bluetooth on | 112 mA | **12.4 mA** |
| Wi-Fi only | | 15.6 mA |
| Wi-Fi off, Bluetooth on | 42 mA | 11.9 mA |
| both off | ~15 mA | 11.9 mA |

**Why the radios cost 100 mA asleep.** The SPM's `PCM_REG13` (SPM base
`0x10006000` + `0x134`) shows which subsystems hold the SoC's resources:
bit 14 `CONN_SRCCLKENA` (the 26 MHz clock), bit 4 `CONN_DDR_EN`, bit 15
`CONN_APSRC_REQ`, bit 13 `CONN_STATE`. Sampling it through `/dev/mem` every
10 ms (read only) showed the connectivity chip holding the 26 MHz clock
(and with Bluetooth, DRAM) 100 % of the time, so the SoC never reached its
deep sleep state. The chip only lets go when every function on it sleeps:
- **Bluetooth never slept.** MediaTek's HAL sends vendor command `0xFC7A`
  (sleep parameters, 7 bytes) from the factory record
  `APCFG/APRDEB/BT_Addr` (bytes 18-24; MediaTek's default is
  `03 40 1f 40 1f 00 04`); `titan-btbridge` only set the address. HCI_Reset
  clears the setting, and BlueZ resets the controller every time it powers
  it on, so the bridge now sends it at start and again after every
  successful HCI_Reset (before passing the Reset's Command Complete on).
  Clock hold: 100 % → 0-3 %. See
  [drivers/connectivity.md](drivers/connectivity.md#sleep).
- **Wi-Fi lost power save at every connection.** The firmware starts each
  connection in CAM (always awake). Android's framework sends the power
  mode again after each connect; cfg80211 doesn't (`iw ... power_save` is
  unchanged, so nothing is sent), and `iw` kept reporting power save "on".
  Fix (`nic/nic.c`, `nicPmIndicateBssConnected`): re-send the power-save
  profile held for the BSS when it connects. Clock hold: 100 % → 3-30 %
  while awake, lower once idle.

**Real use is not there yet.** Overnight (5.3 h unplugged, screen off,
asleep 98 % of the time) the average was 68 mA, but not evenly: 15-17 mA for
the first 35 minutes, then 70-83 mA from about 02:10 until morning, with no
event in the logs at the change (no Wi-Fi reconnect, no gauge resync; the
voltage fell faster too, so it was real). `tools/power/connlog.py` logs, at
every wake, the gauge, the clock-hold share, the Wi-Fi link and power-save
state and Bluetooth, to catch the moment it changes.

`titan-wlan-cmd [-i wlan0] CMD ...` sends any gen3 driver private command
(ioctl `SIOCDEVPRIVATE+1`, `struct priv_driver_cmd_s`).

## Charging

Not characterized yet (the limit and LED below work). What's known (2026-10-20):
- The MT6370 charger runs under MediaTek's charger framework
  (`/sys/devices/platform/charger`: `ADC_Charger_Voltage` = VBUS in mV,
  `pe20`, `pe40`, `pdc_max_watt`, `sw_jeita`; `chg1_current` and
  `input_current` read unset, 4294967295, so the framework chooses).
- **It plans with the wrong battery data:** it reads MediaTek's own
  `battery` supply, a gauge this board doesn't use (capacity −1, 2946 mAh,
  "Charging" while full), not the OZ8806 (`oz8806-battery`: 99 %, 4250 mAh,
  "Not charging" at the same moment). The same stale value blocked the flash
  ([camera.md](camera.md)).
- On a computer's USB port the input is a standard downstream port, 500 mA.

**`titan-charge`** (Rust, `userspace/titan-charge/`, `titan-charge.service`;
it was `titan-chargelog`) does three things:
- **Log** to `/var/log/titan-charge/YYYY-MM.csv`: a line when the percentage
  changes, on plugging in and out, every minute while plugged in, and at
  `hold`/`resume` (the limit); it polls every 10 s and nothing runs while
  asleep. Columns: local time, event, plugged, the gauge's status, percent,
  mV, mA (positive = charging), battery °C, mAh, VBUS mV, USB input limit
  mA, MediaTek's status and percent, charger and SoC °C, screen.
- **Charge limit** (`titan-charge limit [50-100]`, kept in
  `/var/lib/titan-charge/limit`; 100 = none): at the limit charging stops and
  the phone runs from USB (verified: "Not charging", −10 mA at 96 % with a
  90 % limit); it resumes 5 % below, or when unplugged. **It only holds
  above 4.0 V** (~75 % on this battery): on 2026-10-25 the gauge booted at
  "95 %" on an empty cell and the limit kept it from charging for 15 min.
  (The gauge was fixed in kernel #35, [drivers/oz8806.md](drivers/oz8806.md).) The switch is the
  charger manager's `cmd_discharging` (`/proc/mtk_battery_cmd/current_cmd`:
  `0 1` stops, `0 0` charges). MediaTek's own smart-charging limit
  (`sc_tuisoc`, `enable_sc`) would use its battery percentage, −1 here.
- **Indicator LED** (below).
`titan-charge status` prints a reading, the limit and whether charging is
held.

## Indicator LED

The MT6370's RGB LED (`/sys/class/leds/mt6370_pmu_led1`-`3`: red, green,
blue, as the stock lights HAL maps them; `led4` is a fourth, "misc"
channel). Brightness 0-6 (led4 0-3). The trigger selects the chip's mode:
`cc_mode` (steady), `pwm_mode` (hardware blink: `pwm_dim_freq` from a list,
0.1 Hz to 1 kHz, written as its index; `pwm_duty` 0-31) and `breath_mode`
(`tr1`/`tr2`/`ton`/`tf1`/`tf2`/`toff`). Blinking in the chip costs no
wake-ups. The kernel also offers the gauge's triggers
(`oz8806-battery-charging`, `-full`, `-charging-blink-full-solid`).
titan-charge uses: **red** while charging, **green** when full or held at
the limit, a short **red flash every 2 s** (pwm 0.5 Hz, duty 3/31) below
15 % on battery, off otherwise; brightness 2.

Plan: charge from ~20 % to full on a wall charger, a computer's port and a
USB-PD charger; from the logs, the constant-current level, the CV voltage
(must not pass 4.40 V), termination current, time to 80 % and full, peak
temperatures, and whether MediaTek's view tracks the gauge. Then point the
charger framework at the OZ8806.

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

**Screen off and on** (`panel()`), in order:
- off: the `/run/titan-screen.off` flag (Weston stops drawing), the panel
  blanked (fb0 blank 4), the Wi-Fi driver's suspend mode on
  (`SETSUSPENDMODE 1`), systemd-timesyncd stopped; then suspend;
- on: the flag removed, the panel unblanked, **the backlight first**, then
  `SETSUSPENDMODE 0` and timesyncd started in the background. With the
  Wi-Fi and time-sync steps before the backlight, the panel stayed black for
  about a second, and a second press in that gap turned the screen off
  again (2026-10-20, 03:00).
- **The backlight after an unblank:** the display driver powers the
  backlight PWM off at blank (`disp_pwm_backlight_status: backlight is off,
  power:(0)`), and the LED driver skips a write of the value it has cached,
  so restoring the same level left the panel dark. (titan-brightness reads
  the PWM register back to catch a lost write, but that register keeps its
  value while the PWM is unpowered, so its check passed on a dark screen.)
- **After deep suspend the PWM is off as well (2026-10-26).** Once the SoC
  reached real deep sleep, the display power domain lost power and the PWM
  came back with its enable bit clear (`DISP_PWM_EN` at `0x1100e000` = 0)
  while the level register held the right value. `ddp_pwm.c` keeps the
  enable state in `g_pwm_en` and skips the register write when the cache
  already says enabled, so every wake set a level on a disabled PWM and the
  screen stayed dark. titan-powerkey now writes **0** to the LED first (the
  driver then clears its enable cache) and then runs `titan-brightness
  apply`, which enables the PWM in hardware again. A kernel fix would check
  the register instead of the cache.

**Test tools** (`tools/power/`, run on the phone): `sleeptest.py` (the
unattended sleep loop with a frame log, see above), `sleepstats.sh SINCE
[UNTIL]` (suspend cycles, wake sources, sleep lengths, Wi-Fi drops from the
journal), `wlan-rx-count.py` (what arrives on `wlan0`, by kind and
protocol), `sleepfloor.py` (sleep current per radio setup, see
[the sleep floor](#the-sleep-floor-2026-10-2526)), `drainlog.sh [SECONDS]`
(gauge, current, voltage, plug and screen state every 5 s while awake) and
`connlog.py [SECONDS]` (the same at each wake, plus the connectivity chip's
26 MHz clock hold and the Wi-Fi/Bluetooth state). Run the long ones with
`systemd-run --unit=NAME --collect`, so they survive the SSH session.

Long presses are ignored; holding the key for ~10 s makes the PMIC reset
the phone. systemd-logind would otherwise act on the key (the device is
tagged `power-switch`, and logind's default is **poweroff**), so
`/etc/systemd/logind.conf.d/titan-powerkey.conf` sets `HandlePowerKey=ignore`.

## Not done

- The overnight drain step: 15-17 mA asleep for the first half hour, then
  70-83 mA for the rest of the night with nothing in the logs
  ([the sleep floor](#the-sleep-floor-2026-10-2526)). Next: a night with
  `connlog.py` running.
- A kernel fix for the backlight PWM enable cache (`ddp_pwm.c`
  `disp_pwm_set_enabled`), so the userspace workaround isn't needed.
- Wake on keyboard/touch (only the power key, RTC and cable wake it now).
- The resume hang (CPU1 coming online, no pstore record) and which power
  domain refused suspend, once kernel #33 logs it.
- DPMS in the KMS driver (titan-powerkey blanks through mtkfb; Weston idle
  ([core] idle-time) locks and asks titan-powerkey for the screen off, and
  stays offscreen while it's off).
- cpuidle/hotplug tuning, thermal limits.
