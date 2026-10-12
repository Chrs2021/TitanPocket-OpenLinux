# NFC (STMicro ST21NFC)

Status (2026-10-12): the controller initializes and reports its identity;
NDEF read works for NFC Forum Type 2 / Type 4 / Type 5 tags, NDEF write for
Type 2 (NTAG / MIFARE Ultralight). An interactive terminal UI
(`titan-nfc tui`) drives it. **Needs on-device testing with real tags.**
MIFARE Classic is not done yet.

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

### The terminal UI

In the dock as **NFC** (launches `foot titan-nfc tui`). A worker thread
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

## Not done

- **MIFARE Classic.** The ST21NFC runs the CRYPTO1 cipher in its own
  firmware (unlike a Flipper Zero, whose ST25R front end does raw RF, so its
  open CRYPTO1 code does not port here). Authenticated read/write therefore
  goes through a controller-specific NCI command whose exact format must be
  determined from a real card (does the chip activate it as protocol `0x80`,
  and what does the Frame interface accept for `AUTH` with a key?). Command
  bytes and sector layout are known (`AUTH_A 0x60`, `AUTH_B 0x61`,
  `READ 0x30`, `WRITE 0xA0`; 1K = 16 sectors × 4 blocks, trailer = last block
  of each sector); default-key dictionary still to add.
- NDEF **write** for Type 4 and Type 5.
- Tag **emulation** (card emulation / listen mode) and peer-to-peer.
- A graphical (GTK) app — the TUI is the testing front end for now.
