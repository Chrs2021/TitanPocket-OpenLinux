# NFC (STMicro ST21NFC)

Status (2026-10-12): the controller initializes and reports its identity;
NDEF read works for NFC Forum Type 2 / Type 4 / Type 5 tags, NDEF write for
Type 2 (NTAG / MIFARE Ultralight), and dumps MIFARE Classic cards with a key
dictionary (`classic-dump`). A GTK app (`titan-nfc-gui`, in the app drawer)
and a terminal UI (`titan-nfc tui`) drive it. **Needs on-device testing with
real tags.**

## Hardware and driver

An **STMicro ST21NFC** controller on I2C, driven by ST's character-device
driver already built into the BSP kernel: `drivers/misc/mediatek/nfc/
st21nfc/`, `CONFIG_NFC_CHIP_SUPPORT=y`, device node `/dev/st21nfc`. The
driver is a thin pipe to the chip's NCI interface:

| Operation | Behaviour |
|---|---|
| `write()` | sends one complete NCI packet over I2C |
| `read()` | when the controller's IRQ line is high, returns an NCI packet (3-byte header then its announced payload); with no IRQ pending it returns `0x7E` filler bytes |
| `poll()` | readable while the IRQ line is high |
| `ioctl PULSE_RESET` (`0xEA02`) | double low pulse on the reset line |
| `ioctl SET_POLARITY_HIGH` (`0xEA05`) | level-triggered IRQ (what ST's HAL selects) |

The chip keeps its factory **RF configuration** (antenna tuning, RF
parameters): `titan-nfc info` shows config CRC `A5 5E`, matching the value
in the vendor file `st21nfc_conf.txt`, so the Android-written tuning is
intact and we don't reconfigure the analog front end.

## titan-nfc

`userspace/titan-nfc/` (Rust, one dependency: `crossterm` for the UI),
static aarch64-musl binary built by `userspace/titan-nfc/build.sh`. Modules:

| File | Role |
|---|---|
| `nci.rs` | NCI transport: reset-with-retry, packet framing, segmentation/reassembly, command→response matching |
| `rf.rs` | RF discovery map + poll, activation parsing (NFC-A/B/F/V), data exchange on the static RF connection |
| `tags.rs` | NDEF over Type 2 (READ 0x30 / WRITE 0xA2), Type 4 (ISO 7816 APDUs), Type 5 (ISO 15693 read) |
| `ndef.rs` | NDEF record parse (URI, Text, Smart Poster, MIME) and build (URI, Text) |
| `tui.rs` | interactive terminal UI (worker thread owns the controller; UI thread renders + reads keys) |

### Commands

```sh
titan-nfc info            # reset + init, print NCI version, manufacturer, config CRC
titan-nfc poll [secs]     # wait for a tag: type, UID, SAK, NDEF
titan-nfc read [secs]     # wait for a tag and print its NDEF records
titan-nfc dump [secs]     # hex dump a Type 2 tag's pages
titan-nfc write-url URL   # tap a tag to write a URL NDEF record
titan-nfc write-text TEXT # tap a tag to write a text NDEF record
titan-nfc tui             # interactive UI (needs a terminal)
```

`TITAN_NFC_TRACE=1` prints every NCI packet (`> ` sent, `< ` received).

`classic-dump` authenticates each sector of a MIFARE Classic card and dumps
its blocks. Keys come from the `keys` argument, else `$TITAN_NFC_KEYS`, else
the card's factory key. The image ships the proxmark/Flipper default-key list
(2475 keys) at `/usr/local/share/titan-nfc/mf_classic_dict.nfc`;
`/etc/profile.d/titan-nfc.sh` points `TITAN_NFC_KEYS` at it for login shells,
and the GTK app sets it explicitly. Card size comes from the SAK
(`TITAN_NFC_CARD=mini|1k|2k|4k` overrides). MIFARE Classic is kept **out** of
the general polling discovery map (the ST21NFC rejects proto `0x80` there,
see below); `classic-dump` reaches the card through its own path.

### The apps

**NFC** in the app drawer launches `titan-nfc-gui` (GTK 4 with `titan_ui4`, same toolkit as
the other apps). It runs `titan-nfc watch` and shows tapped tags live (type,
UID, NDEF). The mode buttons: **Read**, **Write URL**, **Write Text**, and
**MIFARE** — which stops scanning, and on **Dump card** runs `classic-dump`
(with the key dictionary) and streams the sector dump into a scrollable
view. Writing and dumping own the controller, so they pause scanning.

### The terminal UI

`titan-nfc tui` is the terminal front end. A worker thread
keeps the controller in discovery; pick a mode, tap a card, watch the
result:

- `r` read NDEF · `d` dump pages · `u` write a URL · `t` write text · `q` quit
- the chosen mode runs on the next tap (as phone NFC does: choose write, then tap)
- top: controller status; middle: the tag (type, UID, SAK) and the result;
  bottom: an event log and the key legend

Needs a real terminal (raw mode); over SSH use `ssh -tt`.

### Init sequence (`info`)

`PULSE_RESET` ioctl → wait for the controller to boot (it NAKs I2C until
ready) → `CORE_RESET_CMD` (keep config) → `CORE_INIT_CMD`. NCI 2.0 puts the
version in `CORE_RESET_NTF` and the max control-packet payload in
`CORE_INIT_RSP`; NCI 1.0 puts both in the responses. This phone reports
**NCI 2.0**, manufacturer STMicroelectronics.

### Reader flow (`poll` / `read` / `tui`)

`RF_DISCOVER_MAP_CMD` maps T2T/T3T/T5T to the Frame interface and ISO-DEP to
the ISO-DEP interface; `RF_DISCOVER_CMD` polls NFC-A/B/F/V. On
`RF_INTF_ACTIVATED_NTF` the tag's protocol, tech, UID and (NFC-A) SAK are
parsed. Frames go out as NCI data on connection 0; with the Frame interface
the controller appends a status byte, checked and stripped. After handling a
tag the UI sends `RF_DEACTIVATE` (discovery) so the next tap is detected.

## Card emulation (Type 4 tag)

`titan-nfc emulate-url URL [SECONDS]` makes the phone answer as an NFC Forum
**Type 4 tag** holding one URL NDEF record, so a reader (including an iPhone's
background tag reading, XS and later) reads it like any tag. Implemented in
`src/emulate.rs`:
- **Listen setup** (`start_listen`): `CORE_SET_CONFIG` with NFC-A listen
  params (`LA_BIT_FRAME_SDD`, `LA_PLATFORM_CONFIG` = not NFC-DEP,
  `LA_SEL_INFO` = `0x20` to advertise ISO-DEP, a 4-byte `LA_NFCID1`) and
  `RF_FIELD_INFO` on; **`RF_SET_LISTEN_MODE_ROUTING`** routing NFC-A
  technology and the ISO-DEP protocol to the host (NFCEE `0x00`) — without
  the routing table the controller never answered the reader's select;
  `RF_DISCOVER_MAP` ISO-DEP in listen mode to the ISO-DEP interface;
  `RF_DISCOVER` listening on NFC-A passive (`0x80`).
- **The tag** (`Type4`): answers SELECT (by the NDEF AID
  `D2760000850101`, then the CC file `E103` and NDEF file `E104`) and READ
  BINARY. The files are read-only (UPDATE BINARY returns a security-status
  error). The capability container advertises MLe `0x0080` and no write
  access.
- The loop logs `RF_FIELD_INFO_NTF` (field on/off) and
  `RF_INTF_ACTIVATED_NTF`, then relays each command APDU on connection 0 to
  the tag and sends the response APDU back.

**Verified (2026-10-26):** an iPhone on the home screen, held to the top of
the phone, ran the full Type 4 read 14 times in a minute (SELECT AID → CC →
NDEF file → read length → read the 22-byte message). The field-info trace
shows its field coming and going as it was moved. Whether iOS then shows the
URL as a notification banner is iOS's decision (needs the screen on and
unlocked); the NFC exchange itself completes.

This is tag emulation for **reading by** another device, not peer-to-peer
(NFC-DEP), which iOS has never supported. It gives "tap the phone to an
iPhone to offer a link" — e.g. a download URL for a file the phone serves
over Wi-Fi — without AWDL or Apple certificates (see
airdrop.md).

### NameDrop is not reachable this way

NameDrop (iOS 17+) is NFC-*initiated*, but by Apple's proprietary
**Enhanced Contactless Polling (ECP)** and the **`OSE.VAS.01`** applet (the
same VAS mechanism as Apple Wallet): the tap passes a small encrypted
proximity token, then hands off to AWDL (keyed on SHA-256 hashes of the
users' Apple IDs / phone numbers), TLS, and a biometric Share/Receive UI.
Measured here (`emulate-url` with `TITAN_NFC_TRACE=1`, iPhone brought in for
NameDrop): the iPhone selected **only** the standard NDEF application
(`D2760000850101`) and read us as an ordinary tag — it never selected
`OSE.VAS.01`, i.e. never escalated to VAS. The iPhone decides whether to
escalate during ECP polling, before the applet select.

ECP (Apple's Enhanced Contactless Polling, reverse-engineered by kormax,
github.com/kormax/apple-enhanced-contactless-polling) is a custom frame the
**reader** emits during polling (header `6A`, version, payload, CRC). The
NameDrop trigger is an ECP frame type `05` subtype `00`, TCI `01 00 01`,
carrying the initiator's 6-byte BLE MAC; the peer reads that raw frame to
learn where to bring up the BLE/AWDL link. So triggering or observing
NameDrop would need either to **emit** a custom frame in the polling loop
(reader) or to **observe** raw frames before selection (listener, the
Android-15 "observe mode"). The ST21NFC driver
(`drivers/misc/mediatek/nfc/st21nfc/`) is a raw NCI pipe with only
power-control ioctls (reset, IRQ polarity, wakeup, eSE, recovery); NCI hides
the RF layer, and this controller exposes no observe mode or raw-poll-frame
command (its one proprietary NCI group is a power monitor). kormax's PoCs are
on bare reader chips (PN532, PN5180, ST25R3916, MFRC522) with direct RF
access, which a sealed NFCC like this one doesn't give. And the NFC frame is
only the doorbell: the transfer is still AWDL + Apple-ID identity + the
Apple-only UI. The reachable equivalent is the URL→vCard tap above.

## Not done

- **MIFARE Classic on-device test.** `classic-dump` is in the active binary
  and the GTK app, but has not yet been run against a real card.
  **Finding:** the ST21NFC rejects protocol `0x80` (MIFARE Classic) in
  `RF_DISCOVER_MAP` (`RF_DISCOVER_MAP_RSP` status `0x01`) — an early build
  that put `0x80` in the *general* polling map broke all NFC ("controller
  stopped"), so MIFARE must stay out of that map and be reached on its own
  path. Still open: MIFARE **write**, and reading sectors whose keys are not
  in the dictionary.
- NDEF **write** for Type 4 and Type 5.
- Card emulation **write** (a reader updating our NDEF) and emulating Type 2
  or other tag types; peer-to-peer (NFC-DEP; iOS never supported it).
- A "tap to share" UI on top of `emulate-url` (serve a file over Wi-Fi and
  emulate its URL).
- A graphical (GTK) app — the TUI is the testing front end for now.
