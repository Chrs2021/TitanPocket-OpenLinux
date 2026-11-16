# Reverse-engineering vendor drivers

Unihertz/Agold never published source for the board-specific drivers. The
stock firmware is unusually helpful, though, and the method below recovered
the fuel gauge, keymap and amp in hours rather than weeks.

## Inputs

| File | What |
|---|---|
| `kernel` | stock raw arm64 `Image` (decompressed), load address `0xffffff8008080000` |
| `vmlinux.elf` | the same with kallsyms symbols restored (vmlinux-to-elf); **function** symbols only |
| `stock_config.txt` | stock kernel `.config` |
| `stock_dt/dtbo0.dts` | decompiled dtbo (board devices, pins) |
| `extracted_boot/device_tree.dts` | decompiled base dtb |
| `Firmware/super_ext/vendor_ext/` | extracted vendor partition: HAL libraries, firmware, configs |
| `Firmware/super_ext/system_ext/` | extracted system partition (keylayouts etc.) |

## Tools (`tools/re/`; more below, under "Emulating vendor code" and "Firmware disassemblers")

```sh
# disassemble stock kernel functions by name
container exec titan_build_ubuntu bash /workspace/tools/re/disasm.sh oz8806_probe afe_read_current
# hex dump initialized kernel data (tables, config structs)
container exec titan_build_ubuntu bash /workspace/tools/re/dumpdata.sh ffffff80099ddf70 96
# disassemble an unstripped vendor .so
container exec titan_build_ubuntu bash /workspace/tools/re/disasm_so.sh \
    /workspace/Firmware/super_ext/vendor_ext/lib64/libfs1860.so fs16xx_set_preset
# build symbol + relocation cache, then read structs with pointers resolved
python3 tools/re/reloc.py
python3 tools/re/dq.py ffffff80099ddca0 30
```

### Relocations matter

The stock kernel is relocatable: every pointer in `.data` is **zero** in the
Image and filled at boot from 183,088 `R_AARCH64_RELATIVE` entries (the RELA
table starts at file offset `0x1421658`). Dumping an `i2c_driver`, an
`of_device_id` table or a `miscdevice` naively shows zeros. `reloc.py` finds
the RELA table by scanning and `dq.py` applies it, then resolves targets to
`<symbol+off>` and C strings. This is how we found that the gauge binds
`mediatek,coulomb`, the amp misc device is `/dev/smartpa_i2c`, etc.

## Method

1. **Find the driver by symbols and strings.** `nm` on `vmlinux.elf`
   (`tools/re/.cache/nm.txt`), `strings -n 4 kernel`. Printk format strings
   name registers and units; source file paths (e.g.
   `drivers/misc/mediatek/agold/touchpad/synaptics_1403/...`) reveal the
   driver family.
2. **Find how it binds.** Locate the driver struct via relocations pointing at
   its probe function (`.probe` is at +0x10 in a 4.14 `i2c_driver`; name at
   +0x40, `of_match_table` at +0x68, `id_table` at +0xb8), then read the
   compatible string at +64 in the `of_device_id`. Cross-reference with the
   dtbo to get bus/address.
3. **Read the small accessors first.** Register read/write helpers
   (`afe_read_cell_volt`, `fs16xx_set_mute`...) are short and give register
   numbers and scaling directly (e.g. `mul`, `0x51eb851f` = divide by 100).
4. **Dump the data tables** the code references (`adrp`+`add` targets), and
   verify transcriptions programmatically against the Image — one OZ8806 NTC
   entry was mistyped by hand and caught this way.
5. **Prefer unstripped userspace.** When the kernel only provides a
   pass-through (the FS1860), the HAL library has the logic with full symbol
   names.
6. **Validate on the running phone.** Boot stock Android (rooted) and read the
   hardware: the FS1860 register snapshot confirmed 26 of 27 decoded preset
   mappings and supplied the unit-specific calibration values. Comparing
   "idle" vs "playing" snapshots isolated the speaker-on sequence.

## Struct layouts recovered

- OZ8806 config (`0xffffff80099ddf70`): `{rsense, ntc_pullup, ntc_ref_mV,
  car_lsb, curr_lsb, volt_lsb, design_mAh, cv_mV, eoc_mA, eod_mV, ..., debug}`
  (int32 each). OCV table 65 x `{mV, %}` at `0xffffff80099de0d8`; NTC table
  28 x `{ohm, degC}` at `0xffffff80099dde90`.
- AW9523 keymap (`0xffffff80099e1740`): 35 x `{char name[12]; int code;
  int val; int row; int col;}` (28 bytes).
- FS1860 preset file: 468-byte header (u16 version `0x0703` at 0, coefficient
  count at 42, 81 x u32 DSP RAM words at 144, register values at 64–146 and
  76–116 for the two preset sets) + 512 u16 coefficients.

## Emulating vendor code

When a vendor library computes something we need (register values, a
tuning table, a firmware command), running the stock function is often
quicker and more reliable than reading it. These load stock Android
libraries into Unicorn with just enough of their environment stubbed, call
the function, and log register writes or dump memory:

| Tool | What |
|---|---|
| `emu_so.py` | one function of an AArch64 shared library, memory dumped afterwards |
| `emu_hal.py`, `emu32.py` | several AArch64 (or 32-bit ARM) libraries in one address space, for HAL code that calls across them |
| `emu_dip_regs.py`, `emu_dip_config.py`, `emu_tile.py` | the camera HAL's pass-2 tuning managers and tile engine: the DIP (hardware ISP) register values for a frame ([camera.md](camera.md)) |
| `emu_sensor.py` | a stock-kernel imgsensor function: the sensor register writes it makes |
| `emu_venc.py` | the H.264 encoder's vendor driver: every VENC register access ([video-encoder.md](video-encoder.md)) |

Reading decompiled HAL code: `hal_func.sh`, `hal_view.sh`, `hal_regseq.py`,
`ghidra_regops.py` (register sequences out of Ghidra's output),
`ispmgr_engines.sh`; `elf_strrefs.py` finds where a PIE library points at
a string. Kernel modules: `kodis.py` and `kosimp.py` annotate and simplify
a module's disassembly, `modcheck.py` says whether a prebuilt `.ko` would
load into our kernel.

## Firmware disassemblers

The SoC's coprocessors run firmware from their own partitions. Each has a
tool that parses the container and disassembles the program with
annotations:

| Firmware | Tool | Doc |
|---|---|---|
| SPM (the PCM sleep controller) | `tools/spm/spmfw.py` | [spm-firmware.md](spm-firmware.md) |
| SSPM (MD32 power coprocessor) | `tools/sspm/sspmdis.py` | sspm-firmware.md |
| SCP (Cortex-M4 sensor hub) | `tools/scp/scpdis.py` | scp-firmware.md |
| VPU (Xtensa Vision P6) | `tools/vpu/vpuimg.py`, `xtdis.py`; `xtasm.py` assembles our own | apu.md |

## What has been reverse engineered, and where it is written down

| Subject | How | Doc |
|---|---|---|
| Fuel gauge, keymap, speaker amp, touchpad | stock kernel and HAL, this method | [drivers/](drivers/) |
| Cameras: sensor init, the hardware ISP (DIP) registers and tuning, AF statistics | the camera HAL run in the emulator; register traces | [camera.md](camera.md) |
| SPM sleep firmware: the instruction set, every routine, the sleep ladder, the `debug` word | `spmfw.py` | [spm-firmware.md](spm-firmware.md) |
| SSPM firmware: what suspend and resume do to the PMIC, the RTC's clocks and the rails | `sspmdis.py` | sspm-firmware.md |
| SCP firmware: its IPC and the sensor hub | `scpdis.py` | scp-firmware.md |
| PLL sleep control: `AP_PLL_CON3/4` hand-over, `SPM_PLL_CON`, the CPU PLLs' hardware mode, the vendor's suspend golden tables; every MT6358 rail's sleep mode | the vendor's clock and hotplug code, `goldcheck.py`, `rails.py`, `pllctl.py` on the phone | power-rails.md |
| Why deep sleep reset the phone: hung wakes, the RTC breadcrumb and the preloader's watchdog record, the CPU PLLs across the sleep | `sleepcrash.py`, `longsleep.py`, LK's crash dump in `expdb` | power-testing.md, power-rails.md |
| MT6358 `TOP_RST_MISC`: the MT6331's layout, not the MT6397's | the vendor's PMIC register header against the phone | power-testing.md |
| Modem: LK's hand-off, CCCI, the boot and exception handshakes, the NVRAM file service, CLDMA, the AT mux, the uplink queue mapping the modem asks for; the IMS side (SIP, IPsec, VoLTE, SMS, USSD) | the vendor's eccci driver, AT and packet traces under Android | [modem.md](modem.md) |
| GNSS chip: its protocol and measurements (replacing libmnl), the receiver's registers and gain, the camera clocks as a desense source | libmnl traces, register reads through the WMT debug file | [gnss-open.md](gnss-open.md), gnss-chip.md, gnss-research.md |
| H.264 encoder (VENC) | `emu_venc.py` | [video-encoder.md](video-encoder.md) |
| VPU / APU: firmware format, the NN runtime, writing our own DSP programs | `vpuimg.py`, `xtdis.py`, `xtasm.py` | apu.md |
| Connectivity firmware (Wi-Fi/BT/FM/GPS): the MTKE container, NDS32 sections; monitor mode and injection for AWDL | parsing the firmware files | [drivers/connectivity.md](drivers/connectivity.md), airdrop.md |
| Fingerprint sensor: chip, wiring and ID reads from the stock `sf_ctl` driver | `vmlinux.elf` symbols and disassembly | fingerprint.md |
| IR blaster: the PWM block's memory mode | the vendor IRTX driver | [ir.md](ir.md) |
| NFC (ST21NFC): the driver's NCI transport and the chip's init | the stock driver and the NCI specification | [nfc.md](nfc.md) |

Confidential reference material (`reference/mediatek/`) was read for some
of these; nothing from it is quoted in the docs.

## Still to recover

- **Fingerprint:** the capture protocol (in the stock HAL, not the kernel
  driver) and a matcher (fingerprint.md).
- **Modem power:** what keeps the modem's domain at ~12 mA asleep
  ([power.md](power.md)).
