# AW9523 keyboard

`drivers/misc/mediatek/aw9523/aw9523_key.c` — from the ubuntu-touch Titan
port (`halium-11.0` branch, "rework the keyboard driver based on the orig
kernel binary"), with fixes for this project. Config: `CONFIG_MTK_AW9523=y`.
Binds `i2c4/aw9523@58` (`awinic,aw9523-key`); registers input device
`aw9523-key`.

## How it scans

The AW9523 is a GPIO expander used as a key matrix. On interrupt (EINT 0,
falling edge) the driver scans columns 1–6 (mask `0x7E`): for each column it
writes `mask ^ 0x7E` to P1 config (reg `0x05`) and reads P0 (reg `0x00`). A
0 bit in P0 is a pressed key; bit index = row, column index = col. Changes
are reported as `EV_KEY` events. An hrtimer polls while keys are held.

## Changes made here

### 1. Keyboard active at boot

The upstream probe ended with `agold_key_suspend()` (IRQ disabled) and only
resumed from an `FB_BLANK_UNBLANK` notifier — an event Android's
SurfaceFlinger sends at boot but a plain console never does. Result: zero
keyboard interrupts. Now the probe leaves the keyboard active, and
suspend/resume track `pdata->irq_enabled` so a later blank/unblank cannot
double-enable the IRQ.

### 2. Keymap from the stock kernel

The upstream table (inherited from the Astro Slide) did not match this
keyboard. The stock kernel's table was extracted from the binary at
`0xffffff80099e1740` (35 entries of a 28-byte struct: `char name[12]; int
code; int val; int row; int col;`) and replaced the old one. The scan
convention (bit = row, column index = col) was verified identical by
disassembling stock `aw9523_key_work`.

| Special key | Code |
|---|---|
| Alt | `KEY_RIGHTALT` (100) — console AltGr |
| Sym | `KEY_COMPOSE` (127) |
| Shift | `KEY_LEFTSHIFT` |
| Back | `KEY_BACK` (158) |
| Menu | `KEY_APPSELECT` (580) |
| Fn-like key | `KEY_F13` (183) |

## Alt symbols (keycap legends)

The digits/punctuation printed on the keys are **not** a kernel matter. On
Android they come from `/system/usr/keychars/aw9523-key.kcm`. Here
`userspace/src/titan-keymap.c` loads the same layer into the console AltGr
table with `KDSKBENT` at boot (Alt+Q = 0, Alt+W = 1, ..., Alt+A = @ ...).
It also forces keycode 100 (the Alt key, `KEY_RIGHTALT`) to the **AltGr**
modifier: the kernel default keymap already does, but Debian's
console-setup "us" keymap makes it a plain Alt, which silently disabled the
whole symbol layer. In Debian it runs as `titan-keymap.service` after
`console-setup.service`. Check with `dumpkeys | grep '^keycode 100'`.
For Wayland/X11 the same table is an XKB layout,
`/usr/share/X11/xkb/symbols/titanpocket`: `us` letters, keycap symbols on
level 3, and the Alt key (`<RALT>`) as the level-3 switch
(`level3(ralt_switch)`). Weston uses it through `/etc/xdg/weston/weston.ini`
(`[keyboard] keymap_layout=titanpocket`). Check it with
`xkbcli compile-keymap --layout titanpocket`.

udev's `input_id` builtin tags the device only `ID_INPUT_KEY`, because it has
no Esc/number row. `/etc/udev/rules.d/60-titanpocket-keyboard.rules` adds
`ID_INPUT_KEYBOARD=1`, and `/etc/libinput/local-overrides.quirks` marks it
`AttrKeyboardIntegration=internal`. With both, libinput pairs it with the
keyboard touchpad for disable-while-typing (see [touchpad.md](touchpad.md)).

## Verifying

```sh
grep aw9523 /proc/interrupts      # count rises as you type
cat /proc/bus/input/devices       # aw9523-key, Handlers=kbd eventN
```
