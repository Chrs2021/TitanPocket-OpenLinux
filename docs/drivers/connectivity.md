# Wi-Fi and Bluetooth (MediaTek CONNSYS: WMT + gen3 WLAN + bt_drv)

Status: **working** (2026-10-11). `wlan0` comes up at boot, 2.4 + 5 GHz
scanning, factory MAC and TX power calibration from the phone's NVRAM.
Bluetooth: `hci0` (BlueZ) starts at boot through `titan-bt.service` and
uses the factory address from NVRAM. Classic and LE scanning work, and
LE connections reach BlueZ (fixed 2026-10-15, see "LE commands" below). A
BlueZ GATT peripheral (a phone app connecting, subscribing to notifications
and exchanging encrypted data) works end to end; classic (BR/EDR) pairing
and audio are untested. FM radio works ([fm.md](fm.md)). GPS shares the same chip and
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

#### Wi-Fi RAM code layout (2026-10-16)

`WIFI_RAM_CODE_6759` (480,704 bytes) is in MediaTek's "divided download"
format (`FIRMWARE_DIVIDED_DOWNLOAD_T`, `wlan_lib.h`). The header is the magic
`MTKE`, a CRC32 of everything after the first 8 bytes, a count of 4 and a
section table of `{offset, key index, encrypted, length, destination}`:

| Section | Offset | Length | Loaded at | Encrypted | Content |
|---|---|---|---|---|---|
| 0 | 0x58 | 0x6d80 | 0x020a1000 | yes, key 0 | one 16-byte block repeated 1,536 times: a block cipher in ECB mode over padding |
| 1 | 0x6dd8 | 0x83f0 | 0x000a5800 | yes, key 0 | entropy 7.99, opaque |
| 2 | 0xf1c8 | 0x538f0 | 0xf0000000 | no | entropy 6.6: probably code |
| 3 | 0x62ab8 | 0x12af0 | 0xf0055000 | no | ~1,600 readable debug/log strings |

- **No cryptographic signature is visible.** The only check is the CRC32,
  which can be recomputed.
- **Encrypted sections are decrypted inside the chip.** The driver sets
  `DOWNLOAD_CONFIG_ENCRYPTION_MODE` and the key index per section
  (`CFG_ENABLE_FW_ENCRYPTION`, `wlanImageSectionConfig`). The key is in the
  chip, so sections 0 and 1 can't be read or changed.
- **About 400 KB (85 %) is plaintext.**
- **Untested:** whether the chip's ROM verifies anything else (a hash of the
  plaintext held in an encrypted section, for instance). A cheap, reversible
  test: change one character of a log string in section 3, fix the CRC,
  load it and see whether Wi-Fi comes up. Restore the original file
  afterwards.
- **CPU:** the plaintext code is for an Andes N9 core (NDS32 instruction
  set: 1,132 `ret5 $lp` = `0xdd9e`, almost no ARM, RISC-V or Xtensa
  return patterns). GNU objdump has an NDS32 target; Ghidra doesn't out of
  the box.
- **Frame injection didn't need a firmware patch after all**: it is done in
  the driver ([monitor mode and frame injection](#monitor-mode-and-frame-injection)).

### Power and sleep

How the driver lets the SoC sleep with Wi-Fi connected (WoWLAN, suspend mode
at screen-off, power save, the firmware log, multicast in suspend) is in
[../power.md](../power.md#what-wakes-it-from-sleep) and
[../power.md](../power.md#the-sleep-floor-2026-10-2526). Driver changes for
it:
- `mgmt/rlm.c` (`rlmFillSyncCmdParam`): short slot time always on for 5 GHz.
  Access points that leave the capability bit clear made the firmware log
  `TX_DIAG: 5G Force Short Slot` to the host on every beacon in suspend
  mode, waking the SoC ~10 times a second.
- `nic/nic.c` (`nicPmIndicateBssConnected`): the power-save profile is sent
  again on every connection. The firmware starts each connection in CAM and
  cfg80211 doesn't re-send an unchanged `power_save`, so the radio stayed
  awake (and held the SoC's 26 MHz clock) while `iw` said power save was on.
- `os/linux/gl_init.c` (firmware log level OFF at probe and at each switch
  to suspend mode), `include/config.h` (`CFG_SUPPORT_DROP_ALL_MC_PACKET 1`),
  `common/debug.c` (firmware text log printed only at RX INFO) and
  `nic/nic_rx.c` (each received item's type and first bytes at RX LOUD, for
  debugging).

**Checking it:** `iw dev wlan0 get power_save` only shows what cfg80211
believes. The SPM register `PCM_REG13` (bit 14, `CONN_SRCCLKENA`) shows
whether the chip actually sleeps; `tools/power/connlog.py` samples it.

**Reloading the driver** (stop `wpa_supplicant@wlan0`, `echo 0 >
/dev/wmtWifi`, `rmmod wlan_drv_gen3`, `systemctl restart titan-wifi`, start
`wpa_supplicant@wlan0`) works, with two side effects:
- `/proc/net/wlan/` (and `dbgLevel` in it) isn't recreated until the next
  boot, so the log level can't be changed and `titan-wifi-on` can't set it;
  reboot after installing a new `wlan_drv_gen3.ko` instead.
- The radio comes back as a new phy (`phy1`, `phy2`, ...), so `iw phy phy0`
  fails; `iw dev` shows the current one. WoWLAN is re-applied by
  wpa_supplicant (`wowlan_triggers=any` in its config).

The installed module is stripped (`aarch64-linux-gnu-strip --strip-debug`,
3 MB; the build's is 52 MB with debug info). Check its md5 on the phone
before replacing the installed one: once, a stripped module read from the
build container's output too early arrived empty, and the phone booted
without Wi-Fi (USB SSH still worked, so it was fixed from there).

### Monitor mode and frame injection

The firmware has a sniffer mode, reached with a driver command:

```sh
systemctl stop wpa_supplicant@wlan0          # leaves the access point
titan-wlan-cmd MONITOR 1 149 80 0            # enable, channel, width in MHz (20/40/80/160), ext
ip link set radiotap0 up                     # radiotap headers, every frame on the channel
titan-wlan-cmd MONITOR 0 149 80 0            # back to normal
systemctl start wpa_supplicant@wlan0
```

It receives everything: data, management and control frames (RTS, CTS,
ACKs). Transmitting needed driver work (2026-10-26):
- `os/linux/gl_init.c`: `radiotap0` has an `ndo_start_xmit`
  (`wlanMonHardStartXmit`). It takes a radiotap header plus an 802.11
  frame without FCS (what libpcap's `pcap_inject` sends), strips the
  radiotap header and posts the frame to the main thread
  (`MID_MNY_AIS_MON_TX`). The interface comes up with carrier on (without
  it the stack drops everything sent) and **wlan0's MAC address**.
- `mgmt/ais_fsm.c` (`aisFsmRunEventMonTx`): the frame goes out through the
  management-frame path at 6 Mbit/s OFDM with its own sequence number.
- `common/wlan_oid.c` (`wlanoidSetMonitor`): the AIS BSS is activated in the
  firmware while sniffing (and deactivated after). The firmware drops
  management frames for an inactive BSS: before this, injected frames went
  to the chip and nothing went on air.
- **Data frames** go to an LMAC queue instead (best effort, `TC1`). The
  management path (`TC4`) ends at the MCU port, and the firmware there
  silently drops data frames; the LMAC queues go straight to the MAC
  hardware. `nic/nic_tx.c` (`nicTxProcessMngPacket`) now keeps a queue
  chosen with `MSDU_OPT_MANUAL_TX_QUE` instead of forcing `TC4`.

Checked against our own access point (`tools/wifi/inject-ack-test.py`, 10
frames each): probe requests, null-data and data frames are all ACKed (the
data frames also bring a deauthentication, as they should from a station
that isn't associated). The radio **ACKs unicast frames sent to wlan0's
address** while sniffing (`tools/wifi/inject-test.py`: 1.1-1.2 copies per
probe response, i.e. almost no retries), so it works as an "active"
monitor interface. Only test-inject towards your own equipment.

Known issue: once in a while, after a monitor session in which an AWDL
peer was active, scans return nothing until the Wi-Fi function is powered
off and on (`echo 0 > /dev/wmtWifi`, `systemctl restart titan-wifi`).

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

### LE commands

The controller's reply to **Read Local Supported Commands** leaves octets
25-28 (all the Bluetooth 4.0 LE commands) at zero, although it implements
them. The 4.14 kernel builds the **LE event mask** from that list
(`hci_init3_req` in `net/bluetooth/hci_core.c`: LE Create Connection
enables LE Connection Complete, LE Set Scan Enable enables LE Advertising
Report, ...), so it set the mask to `0x10` (LE Long Term Key Request only).
- Symptom: LE advertising worked, but when a phone connected, BlueZ never
  got LE Connection Complete. The client's first ATT packet arrived on an
  unknown ACL handle and was dropped, and the connection timed out. LE scans
  returned nothing.
- Fix: `titan-btbridge` ORs the 4.0 LE command bits (`f7 ff ff 7f` into
  octets 25-28) into that reply on its way to the host, and logs "added the
  LE commands missing from Read Local Supported Commands". The kernel then
  sets the LE event mask to `0x1f`, and every LE command BlueZ sends at init
  succeeds.

### Address

Without help the controller reports a MediaTek default of
`00:00:46:xx:xx:xx`. The stock HAL (`libbluetooth_mtk.so`,
`GORMcmd_HCC_Set_Local_BD_Addr`) reads the factory NVRAM record
`APCFG/APRDEB/BT_Addr` and sends vendor command **0xFC1A** with the 6
address bytes, least significant first.

- The record is 440 bytes. Its first 6 bytes are the address in display
  order. Bytes 18-24 are the controller's sleep parameters (see
  [Sleep](#sleep)); the rest are radio settings the controller doesn't
  need.
- `titan-bt-start` copies the record once to `/var/lib/titan-bt/bt_addr.bin`
  (mode 600, per phone, don't share it) and runs `titan-btbridge -a <addr>`.
- The bridge sends HCI_Reset and then 0xFC1A before it opens vhci.
- The address stays set through BlueZ's own HCI_Reset (checked with
  `titan-btbridge -a ... -t`).

### Sleep

Without its sleep parameters the controller never sleeps, and an awake
Bluetooth function keeps the whole connectivity chip awake: it held the
SoC's 26 MHz clock and DRAM 100 % of the time, about 30 mA in suspend
([../power.md](../power.md#the-sleep-floor-2026-10-2526)). MediaTek's HAL
sends vendor command **0xFC7A** with 7 bytes from the NVRAM record (bytes
18-24; MediaTek's default is `03 40 1f 40 1f 00 04`).
- `titan-bt-start` reads them from the record and passes them as
  `titan-btbridge -s <14 hex digits>`; without them the bridge uses
  MediaTek's default.
- **HCI_Reset clears the setting**, and BlueZ resets the controller every
  time it powers it on (at boot, `btmgmt power on`). The bridge watches for
  a successful HCI_Reset Command Complete, sends 0xFC7A, waits for its
  completion and only then passes the Reset's completion to BlueZ, so BlueZ
  never sees the extra command.
- Result: clock hold 100 % → 0-3 % with Bluetooth idle; a command wakes the
  controller for about 2 s.

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
- Why scans sometimes find nothing after a monitor session with an AWDL
  peer (worked around by power-cycling Wi-Fi).
- Concurrent monitor and station mode (AWDL while connected to an access
  point).
- `WMT_STEP.cfg`, coredump/firmware log daemons (`stp_dump`, `wifi_dump`).
