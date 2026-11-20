# Userspace the hardware needs

The few programs and settings that the Titan Pocket's hardware needs on
the 6.12 kernel, beyond what a distribution brings: the connectivity
chip (Wi-Fi, Bluetooth, FM) and the sound card. They replace the parts of
Android's vendor services that these drivers depend on. Everything else
(desktop, apps, the IMS client for calls and SMS) belongs to a separate
project and is not here.

Written for and tested on Debian 12 (systemd, PipeWire run as root) on
the phone; another distribution may want other paths or a different
audio setup. Install paths follow the units: programs in
`/usr/local/bin` and `/usr/local/sbin`, units in `/etc/systemd/system`.

## Contents

| Path | What |
|---|---|
| `connectivity/titan-wmt.c` | powers the CONNSYS chip up through MediaTek's WMT driver (`/dev/stpwmt`) and serves its patch downloads from `/lib/firmware`; replaces Android's `wmt_loader` and `wmt_launcher`. `titan-wmt.service` runs it |
| `connectivity/titan-wifi-on` | Wi-Fi on: loads the drivers, hands the driver this phone's factory NVRAM record (MAC address, TX power calibration, 5 GHz), powers Wi-Fi up, waits for `wlan0` and turns 802.11 power save on. `titan-wifi.service` |
| `connectivity/titan-btbridge.c` | Bluetooth: relays the controller's raw HCI (`/dev/stpbt`) to `/dev/vhci`, so BlueZ sees `hci0`, with the factory address and the controller's sleep parameters. `titan-bt-start` reads those from NVRAM and starts it; `titan-bt.service` |
| `connectivity/titan-nvram-copy` | copies one factory NVRAM record from the phone's `nvdata` partition (mounted read-only, never written) to a mode-600 file, once |
| `connectivity/titan-fm.c` | FM tuner for `/dev/fm`: scan and tune (the radio's RDS is decoded in the driver) |
| `audio/91-titan-audio.rules` | switches the sound card's speaker route (DL1 to I2S3, the amplifier) and the FM capture route on when the card appears; the kernel's DAPM then powers the amplifier while something plays |
| `audio/pipewire/`, `audio/wireplumber/` | the speaker sink (the card has ~40 PCMs and no UCM profile, so WirePlumber's automatic ALSA setup is off and the sink is defined by hand) and a limiter (`swh-plugins`' `fast_lookahead_limiter`, LADSPA) |
| `audio/titan-headphones/` | wired headphones: watches the jack and moves the sound to it and back, with its own volume (Rust, standard library only). `titan-headphones.service` |
| `audio/debian-root-session/` | Debian's PipeWire user units skip root; these drop-ins let them run in root's session, which the rest assumes |

The firmware and per-phone data these need are not here; the top-level
README lists them and where they come from.

## Building

```sh
make                 # on an aarch64 machine: the C programs static, titan-headphones
                     # against the system's C library
make CARGO_TARGET=aarch64-unknown-linux-musl     # titan-headphones static too
make CC=aarch64-linux-gnu-gcc CARGO_TARGET=aarch64-unknown-linux-musl  # cross
sudo make install    # PREFIX=/usr/local, DESTDIR for packaging
```

The C programs need only the C library. `titan-headphones` needs Rust
1.70 or later and no crates. At run time: `amixer` (alsa-utils),
`pactl` and `pw-cli` (PipeWire), `iw`, BlueZ, and the kernel's
out-of-tree drivers from `modules/connectivity/`.

## License

GPL-2.0-or-later: the GNU General Public License, version 2 or (at your
option) any later version; the text is in `COPYING`, and each file says
so in its `SPDX-License-Identifier` line. This folder's license differs
from the repository's top-level `LICENSE` (GPL-3.0), which covers what
has no license of its own; code under GPL-2.0-or-later may be used under
GPL-3.0 as well.
