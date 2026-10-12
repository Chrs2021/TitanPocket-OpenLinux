# Wi-Fi and Bluetooth (MediaTek CONNSYS: WMT + gen3 WLAN + bt_drv)

Status: **working** (2026-10-11). `wlan0` comes up at boot, 2.4 + 5 GHz
scanning, factory MAC and TX power calibration from the phone's NVRAM.
Bluetooth: `hci0` (BlueZ) starts at boot through `titan-bt.service` and
uses the factory address from NVRAM. Scanning works; pairing hasn't been
tested yet. FM radio works ([fm.md](fm.md)). GPS shares the same chip and
core driver but isn't brought up yet.

Everything is built **from source** (no stock `.ko`), so it can move to a
newer kernel later. Two MT6771-specific files that MediaTek never published
were reconstructed from the stock modules.

## Hardware

The MT6771 has an on-die connectivity subsystem ("CONNSYS", chip id
`0x788`, an ARM MCU + MAC) paired with the **MT6631** RF chip (A-die). The AP
talks to it through:

- **BTIF**: a UART-like link carrying STP, the framed protocol for WMT
  commands, patch download, BT/GPS/FM data (`1100c000.btif`).
- **AHB/AXI HIF** at `0x180f0000` for Wi-Fi data (PDMA rings).
- **EMI**: 2 MB of DRAM shared with the chip (`consys-reserve-memory`,
  allocated at boot; `0xbfc00000` on this unit), guarded by the EMI MPU.
- Power: SCPSYS `conn` power domain (CCF clock `conn`), PMIC LDOs VCN18,
  VCN28 and VCN33 (MT6358), a clock buffer (XO_WCN).

Device tree node (from the stock dtb, untouched):
`consys@18070000`, `compatible = "mediatek,mt6771-consys"`, 4 `reg`
ranges (mcu `0x18070000`, RGU `0x10007000`, INFRACFG_AO `0x10001000`,
SPM `0x10006000`), 2 interrupts, clock `conn`.

## Software stack

| Piece | Where | Origin |
|---|---|---|
| `wmt_drv.ko`: WMT core, STP, BTIF glue, chip power, patch download | `modules/connectivity/common/` | Motorola's `vendor-mediatek-kernel_modules-connectivity-common` (`android-11-release-rrq`) |
| `common_main/platform/mt6771.{c,h}`: chip power/clock/EMI ops | same | **reconstructed** from stock `wmt_drv.ko` |
| `wmt_chrdev_wifi.ko`: `/dev/wmtWifi` on/off switch, NVRAM hand-over | `modules/connectivity/wlan_adaptor/` | `mediatek-opensource/...-wlan-adaptor` (`alps-mp-r0.mp1`) |
| `wlan_drv_gen3.ko`: the Wi-Fi driver (cfg80211, `wlan0`) | `modules/connectivity/wlan_gen3/` | Motorola's `...-wlan-core-gen3` (`android-15-release-vvob35`) |
| `os/linux/plat/mt6771/plat_priv.c`: EMI MPU region for Wi-Fi, CPU/EMI boost | same | **reconstructed** from stock `wlan_drv_gen3.ko` |
| `titan-wmt`: replaces Android's `wmt_loader` + `wmt_launcher` | `userspace/src/titan-wmt.c` | written from the reverse-engineered protocol |
| `bt_drv.ko`: `/dev/stpbt`, raw H4 HCI over STP | `modules/connectivity/bt/legacy/` | Motorola's `...-connectivity-bt-mt66xx` (`android-11-release-rrk`) |
| `titan-btbridge`: `/dev/stpbt` ↔ `/dev/vhci` relay, makes `hci0` | `userspace/src/titan-btbridge.c` | new |
| `titan-wmt.service`, `titan-wifi.service`, `titan-bt.service`, `titan-wifi-on`, `titan-bt-start`, `titan-nvram-copy` | `userspace/debian/overlay/` | new |

The in-kernel half (`drivers/misc/mediatek/connectivity/common/*build_in_adapter*`,
BTIF, EMI MPU, CCF, PMIC) was already in the BSP; `CONFIG_MTK_COMBO_CHIP="CONSYS_6771"`
is set from the stock config.

Coverage check: 97 % of the stock `wmt_drv.ko` functions and every source
file compiled into the stock `wlan_drv_gen3.ko` exist in the published
trees; the rest is release drift. Only the platform files were missing.

### Changes to the vendor sources

- `common/common_main/core/wmt_conf.c`: removed Motorola's per-product
  `WMT_SOC_<device>.cfg` selection (read `androidboot.*` from
  `saved_command_line`, which this kernel doesn't export).
- `wlan_gen3`: dropped `(const struct proc_ops *)` casts (the structs are
  already version-guarded); guarded the P2P `update_mgmt_frame_registrations`
  op for kernels < 5.8; always search `/lib/firmware/` for the RAM code (it
  was only searched on non-Android kernel configs); `-I.../mach` for
  `plat_priv.c`.

## Reconstructing `mt6771.c`

Stock `wmt_drv.ko` keeps global symbols but not static ones. The ops table
`consys_ic_ops` (`.data` `0x1f8`, 64 pointers) maps each hook to code in
`.text` `0x3910`–`0x58e0`; the field order matches Motorola's
`WMT_CONSYS_IC_OPS`. 31 hooks are implemented, the rest NULL. Each function
in `mt6771.c` carries a `/* stock 0x.... */` comment. Highlights:

| Hook | MT6771 behaviour |
|---|---|
| power | `clk_prepare_enable("conn")` (SCPSYS), not raw SPM pokes |
| chip id | poll `mcu + 0x08` for `0x788` (10 × 20 ms) |
| ROM delsel | `mcu + 0x600 = 0x001702a4` |
| ACR | `mcu + 0x140 |= 3` |
| AFE | `0x180b6000 + 0x38 = 0xc1df0`, `+ 0x54 = 0x110a2000` |
| LDOs | VCN18 1.8 V, VCN33 3.5 V (with VCN18), VCN28 2.8 V; `MT6358_LDO_VCN{18,33}_OP_EN = 1`; VCN28 HW0 op mode |
| EMI remap | `INFRACFG_AO + 0x380 |= (base >> 21) | 0x2000` |
| EMI MPU | region 26, `base + 0x70000` .. end, apc `0x80b6da28 / 0x00b6db6d` |
| pin mux | `0x10005000 + 0x3f0`, `+ 0x3e0` (A-die interface) |
| options | `0x8436b5` (incl. `OPT_NORMAL_PATCH_DWN_2`, ROMv4 patches) |

`plat_priv.c` (stock `wlan_drv_gen3.ko` `.text 0x0–0x1d8`) adds MPU
**region 25** over the first 512 KB of the CONNSYS memory (Wi-Fi firmware
area): CONNSYS always allowed, AP allowed only while unprotected. Without it
the Wi-Fi firmware's first reads hit the AP-only region 31 and flood the log
with EMI MPU violations.

Tools: `tools/re/kodis.py` + `kosimp.py` (module disassembly with
relocations and folded constants), `tools/re/elfstr.py` (Android binaries),
`tools/re/modcheck.py` (symbol CRC check of prebuilt modules).

## Bring-up sequence

1. `modprobe wmt_drv` → `/dev/wmtdetect`.
2. `titan-wmt`, loader part (`/dev/wmtdetect` ioctls, magic `'w'`):
   `EXT_CHIP_PWR_ON` (SoC: refused) → `GET_SOC_CHIP_ID` (0x6771) →
   `SET_CHIP_ID` → `MODULE_CLEANUP` → `DO_MODULE_INIT` → `/dev/stpwmt`.
3. `titan-wmt`, launcher part (`/dev/stpwmt`, magic `0xa0`):
   `WMT_CFG_NAME "WMT_SOC.cfg"`, `SET_STP_MODE 0x23` (BTIF full, FM over the
   common interface), `SET_LAUNCHER_KILL 0`, then it serves kernel requests
   (poll/read a command string, answer with ioctls, write `ok`):
   - `srh_patch`: every `ROMv4_be_patch*` file in `/lib/firmware`; header
     byte `0x18` = count (high nibble) / sequence (low nibble), bytes
     `0x19..0x1b` = load address → `SET_PATCH_NUM`, `SET_PATCH_INFO`.
`titan-wmt.service` covers steps 1–3. It is ready once `/run/titan-wmt.ready`
exists, and it keeps running to serve patch requests. `titan-wifi.service`
and `titan-bt.service` both `Require` it.

4. `titan-wifi-on`: `modprobe wmt_chrdev_wifi wlan_drv_gen3`, write
   `WR-BUF:NVRAM` + the 1024-byte NVRAM record to `/dev/wmtWifi`, then `1`.
   The kernel powers CONNSYS on, downloads both ROM patches over STP,
   calibrates, loads `WIFI_RAM_CODE_6759`, creates `wlan0`.
5. `wpa_supplicant@wlan0` + `systemd-networkd` (`30-wlan0.network`, DHCP).

### Firmware (from the vendor partition, not redistributable)

`/lib/firmware/`: `WMT_SOC.cfg`, `ROMv4_be_patch_1_0_hdr.bin`,
`ROMv4_be_patch_1_1_hdr.bin`, `WIFI_RAM_CODE_6759` (MT6771 uses the
MT6759 RAM code). The rootfs build copies them from
`Firmware/super_ext/vendor_ext/firmware/`.

### NVRAM (per phone)

The factory record `APCFG/APRDEB/WIFI` lives in the Android `nvdata`
partition (`/dev/sdc10`, ext4). `titan-wifi-on` copies its first 1024 bytes
once to `/var/lib/titan-wifi/nvram.bin` (mounting `nvdata` read-only with
`noload`, so it is never modified; the helper is `titan-nvram-copy`). It holds the MAC address
(`f0:df:ec:...`), TX power calibration and the **5 GHz enable flags**:
without it the driver runs 2.4 GHz only with a random `00:08:22:...` MAC.
Don't share this file; it is unit-specific.

## Using it

From a shell:

```sh
# add a network (type the passphrase when prompted), then reconnect
wpa_passphrase "MyNetwork" >> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
systemctl restart wpa_supplicant@wlan0
networkctl status wlan0        # DHCP via systemd-networkd
iw dev wlan0 link
iw dev wlan0 scan | grep -E 'SSID|freq'
```

`wpa_passphrase` also prints the plaintext passphrase as a comment line;
delete that line from the file afterwards.

## Bluetooth

`bt_drv.ko` creates `/dev/stpbt`. Opening it switches the BT function on
through WMT. The ROM patches downloaded for Wi-Fi already cover BT, so
there is no separate BT firmware; `BT_FW.cfg` is installed but optional.
The device carries plain HCI with H4 framing: one type byte, then a
command, ACL, SCO or event packet. The kernel has no HCI driver for it, so
`titan-btbridge` relays packets between `/dev/stpbt` and `/dev/vhci`.
Writing `ff 00` to vhci creates a primary controller, which shows up as
`hci0` in BlueZ.

The relay buffers reads from `stpbt` and splits them on H4 packet
boundaries, because vhci expects one packet per write. Anything BlueZ
writes goes to the controller unchanged.

Controller: HCI version 0x0a (5.1), manufacturer 0x0046 (MediaTek).

### Address

Without help the controller reports a MediaTek default of
`00:00:46:xx:xx:xx`. The stock HAL (`libbluetooth_mtk.so`,
`GORMcmd_HCC_Set_Local_BD_Addr`) reads the factory NVRAM record
`APCFG/APRDEB/BT_Addr` and sends vendor command **0xFC1A** with the 6
address bytes, least significant first.

- The record is 440 bytes. Its first 6 bytes are the address in display
  order; the rest are radio settings that this controller doesn't need.
- `titan-bt-start` copies the record once to `/var/lib/titan-bt/bt_addr.bin`
  (mode 600, per phone, don't share it) and runs `titan-btbridge -a <addr>`.
- The bridge sends HCI_Reset and then 0xFC1A before it opens vhci.
- The address stays set through BlueZ's own HCI_Reset (checked with
  `titan-btbridge -a ... -t`).

### Using it

```sh
systemctl status titan-bt bluetooth
btmgmt info                     # hci0, address, settings
bluetoothctl                    # power on / scan on / pair <addr> / connect <addr>
titan-btbridge -t               # stop titan-bt first: raw HCI self-test
```

## Routing and DNS

Wi-Fi is the preferred uplink; the USB gadget network (`usb0`,
172.16.42.1/24) is for management only.

- **Routes**: `30-wlan0.network` installs the IPv4 (DHCP) and IPv6 (RA)
  default routes with metric 600. `50-usb0.network` has no gateway, ignores
  router advertisements and is never a DNS route. A later uplink (e.g.
  cellular) should use a higher metric.
- **DNS**: systemd-resolved, `/etc/resolv.conf` → its stub
  (`127.0.0.53`). Wi-Fi is the DNS default route (`Domains=~.`). For now it
  uses **8.8.8.8** (`DNS=` in `30-wlan0.network`) and ignores the DNS servers
  DHCP/RA offer (`UseDNS=no`). Set `UseDNS=yes` and drop `DNS=` to use the
  network's resolver instead.
- Check: `ip route`, `ip -6 route`, `cat /run/systemd/resolve/resolv.conf`.
  `resolvectl` needs D-Bus (not running on this minimal system).

Before this, `/etc/resolv.conf` was the build container's file
(`nameserver 192.168.64.1`, copied in by debootstrap), so no names resolved
on the phone.

## Debugging

- `journalctl -u titan-wifi` (titan-wmt log), `dmesg | grep -E 'WMT|wlan|CONSYS'`.
- `[MPU] EMI MPU violation ... MT6771_M7_AXI_MST_CONNSYS`: the chip touched
  memory outside regions 25/26; check `/sys/bus/platform/drivers/emi_ctrl/mpu_config`.
- `WMT turn on WIFI fail!` immediately: WMT is stuck after an earlier
  failure; reboot.
- WMT only asks for patches once per boot (the info is cached).

## Not done

- Bluetooth audio (A2DP needs PipeWire/PulseAudio; SCO over HCI is untested).
- GPS. FM is in [fm.md](fm.md).
- Suspend/resume with Wi-Fi on; Wi-Fi power save tuning.
- `WMT_STEP.cfg`, coredump/firmware log daemons (`stp_dump`, `wifi_dump`).
