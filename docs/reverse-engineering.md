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

## Tools (`tools/re/`)

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

## Still to recover

(The touchpad was recovered with this method; see [drivers/touchpad.md](drivers/touchpad.md).)


- **Cameras** (OV16880, OV8865): `OV16880_MIPI_RAW_SensorInit` etc.; an
  earlier session extracted init tables (`ov16880_sensor_init.c`,
  `extracted_blobs/`). Of limited value without MediaTek's camera HAL.
