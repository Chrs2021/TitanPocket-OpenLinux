# SPM firmware (MT6771 PCM code)

The SPM (System Power Manager, `0x10006000`) runs a small program on its
own sequencer, the PCM, which does the low-power work: during suspend it
powers down the CPU top, sends DRAM to self-refresh, turns off the 26 MHz
clock, and reverses all of it on wake; while the system is awake it serves
the Vcore and DRAM frequency requests. MediaTek ships the program as a
binary (the `spmfw` partition); ATF loads it, and the kernel only passes
flags (`mt6771-spm`, see [mainline.md](mainline.md), "Suspend").

This page is the whole program, read statically with `tools/spm/spmfw.py`:
every instruction decodes, every routine is named, and every branch's
condition is given in the vendor's flag and status names. Nothing here
was taken from confidential material: the register, flag and bit names
come from the vendor kernel's GPL headers (`spm_v4/mtk_spm_reg_mt6771.h`,
`pcm_def_mt6771.h`, `sleep_def_mt6771.h`, `dramc/mt6771/mtk_dramc.h`) and
its device tree (block addresses), and the encoding was worked out from
the code. Where a meaning is inferred rather than forced by the code,
the text says so. The firmware, and listings made from it, are vendor
material: they stay out of the repository.

```
tools/spm/spmfw.py Firmware/spmfw-verified.img --annotate   # the program
tools/spm/spmfw.py IMAGE --routines   # each routine: calls, writes, tests
tools/spm/spmfw.py IMAGE --flags      # every bit a branch tests, and where
tools/spm/spmfw.py IMAGE --effects    # each flag-gated skip and what it skips
tools/spm/spmfw.py IMAGE --debug      # where each SPM_SW_DEBUG bit is set
tools/spm/spmfw.py IMAGE --dis 800 860
sh spmprobe.sh 40     # on the phone (tools/spm/): the live state
```

`--build N` picks the build (default 2, the one this phone runs).
Addresses below are word indices in hex, for that build.

## Container

The partition starts with a MediaTek partition header (name `spmfw`),
then four records, each tagged `2MPS`:

| Build | Name | Words | Instructions |
|---|---|---|---|
| 0 | `pcm_allinone_lp3_1866` | 2984 | 2373 |
| 1 | `pcm_allinone_lp4_3200` | 2988 | 2284 |
| 2 | `pcm_allinone_lp4_3733` | 2988 | 2286 |
| 3 | `pcm_allinone_lp4_2400` | 2988 | 2295 |

A record holds the name at +8, the code size in 32-bit words (u16) at
+0x42, and the code from +0x44. After the code comes the `pcm_desc`: size
(u16), session (u8), replace (u8), addr_2nd (u16), reserved, then 16 event
vectors. The version string follows, `pcm_allinone_v1.18_20190221` in all
four. A vector is `pc << 16 | imm << 6 | resume << 5 | event`.

The vendor kernel's 4.14 log on this phone reports `__spmfw_idx 0x0
(type:3 freq:3600)`: LPDDR4X at 3600, the vendor enum's index 0 (index 0
is LP4X_2CH_3733, 1 LP4X_2CH_3200, 2 LP3_1CH_1866, 3 LP4_2CH_2400). The
container's order differs from the enum, so ATF matches by name; the
3733 build is container record 2. Our driver passes the same index
(`SPMFW_IDX 0`).

All four builds have the same 47 routines in the same order. Compared
routine by routine (flow targets by routine), they differ only in DRAM
code: the 3200 build in the APSRC sleep handler and the frequency-change
path (`ev8_dvfs`, `dfs_target`, `dvfs_requests`, `dfs_shuffle`), the
LPDDR3 build also in the DRAM helpers and the main program.

## Instruction set

32-bit words. Every jump, call and branch has one delay slot (the next
instruction runs before the transfer). Fields:

```
op 31:28 | imm 27 | rd 26:22 | inv 21 | left 20 | sh 19:15 | rs2 14:10 | rs3 9:5 | rs1 4:0
```

Data operations compute `rd <- rs1 OP operand2`, where operand2 is the
next word (imm set) or `rs2` shifted by `sh` (left or right). r1 holds 1
(set at entry), r31 reads as 0.

| Op | Operation | Confidence |
|---|---|---|
| 0 | add; with inv, subtract (the timeout counters: `r4 <- r4 - r1`) | high |
| 1 | add / move. With imm: `rd <- #word`, with left `rd <- [word]` (a load), with inv `rd <- [rs1]` (no word) | high: loads feed tests of status registers, moves feed stores |
| 3 | XOR | medium: both uses compare two level bits |
| 4 | a magnitude compare (direction not settled) | low |
| 6 | `rd <- (rs1 == operand2)` | high: the bus-protect and ack polling loops |
| 7 | `rd <- (rs1 != operand2)` | high: the `SPM_DRS_CON` poll |
| 8 | AND; with inv, AND NOT (clear bits) | high |
| 9 | XOR (toggles a bit; inv set in both uses) | medium |
| 0xa | OR | high |
| 0xb | `rd <- rs3 \| bit sh of rs2` (inv: the bit's complement): OR-ing flag tests | high: "deep idle or `DIS_INFRA_PDN`" |
| 0xc | call (`0xc0`); with imm, call if rs1 is set (inv: clear) | high; conditional sense as the branch's |
| 0xd | jump (`0xd0`); with imm, branch if rs1 is set (inv: clear) | high: measured on the `SPM_ACK_CHK` fail loop |
| 0xe | store `[rd] <- rs1`, or with inv `[rd] <- #imm10`; with imm (`0xe8`) `[word1] <- #word2` | high |
| 0xf | return | high |

Common forms: `r7 <- r7 | r1 << 24` sets bit 24, `r7 <- r7 & ~(r1 << 24)`
clears it, `r5 <- r1 & (r6 >> 20)` tests bit 20 of r6. `0x17c07c1f`
(`r31 <- r31 + r31`) is the no-op that fills delay slots. Flow targets
sit in bits 20:5, so bit 16 of a branch is bit 11 of its target.

### Registers

| Register | Use |
|---|---|
| r0 | output: clock and DRAM PHY controls (`R0_*`: `SC_26M_CK_OFF`, `SC_CKSQ0_OFF`, `SC_AXI_CK_OFF`, `SC_MEM_CK_OFF`, the DLL, PHY PLL and shuffle bits; bits 31:30 the DRAM shuffle level) |
| r1 | 1 |
| r6 | the flags: `SPM_SW_FLAG` at entry, plus software state: 23 DFS direction, 24 the active DRAM PHY PLL, 27-31 resource state (26M, infra, APSRC, VRF18, DDREN; 1 = awake) |
| r7 | output: handshakes and controls (`R7_*`: `SRCCLKENO_0`, `SRCVOLTEN`, `PWRAP_SLEEP_REQ`, `EMI_CLK_OFF_REQ`, `TOP_MAS_PAU_REQ`, `FORCE_*_WAKE`, `PCM_CK_SEL0/1`, `WDT_KICK_P`, `SPM2RC_DVS_DONE`) |
| r12 | multiplexed by r7's `WAKEUP_EXT_W_SEL` and `WAKEUP_EXT_R_SEL`: a write arms the events taken next (or clears the wake latches), a read returns the live wake status (`R12_*`) |
| r13 | read only: resource requests and acknowledges (`R13_*`: `EMI_CLK_OFF_ACK_ALL`, `PWRAP_SLEEP_ACK`, `EMI_IDLE`, `SW_DMDRAMCSHU_ACK_ALL`, `C2K_VRF18_REQ`) |
| r14 | the PCM timer: a write waits. Top nibble 2: a fixed delay (48 uses); `0xa`: before polling loops (31); `0x9` and `0xd`: the event waits (inferred) |
| r15 | the program counter (never written; the vendor reads its copy as `PCM_REG15_DATA`) |
| r2-r5, r8-r11 | scratch; r10 carries arguments (the debug bit, the DVFS command, the target level) |

## Structure

| Words | What |
|---|---|
| `000`-`2ed` | the 11 event handlers (vectors) |
| `2ee`-`7ff` | 34 helpers, then no-ops to `7ff` |
| `800`-`b4d` | the main program: sleep entry, wait, wake, resume |
| `b4e`-`ba2` | the common scenario: the awake loop |
| `ba3`-`bab` | one helper |

Three kinds of code run: the **main program** when the kernel (through
ATF) kicks the PCM, the **event handlers** when a hardware event fires
during the main program's waits, and the **common scenario**, the loop
the main program ends in, which waits for the next kick while serving
Vcore/DRAM frequency requests (event 8) and, by default, DRAM
self-refresh while the system is awake.

### The resource ladder (event handlers)

Each handler fires on its event (a resource's request from all masters
dropping or rising, gated by the kernel's source masks), sets one
`SPM_SW_DEBUG` bit, updates its resource bit in r6, and before returning
writes r12 with the events it allows next (event-numbered):

| Vector | Event | Entry | Step (debug bit) | Arms next |
|---|---|---|---|---|
| 13 | 13 | `2ae` | DDREN sleep (9) | 5 APSRC sleep, 8, 12 DDREN wake (SODI CG mode: 8, 12) |
| 5 | 5 | `139` | APSRC sleep (5) | 4 APSRC wake, 7 VRF18 sleep |
| 11 | 7 | `28b` | VRF18 sleep (7) | 3 infra sleep, 4, 6 VRF18 wake (SODI without SODI3: 4, 6) |
| 3 | 3 | `0db` | infra sleep (3) | 1 26M sleep, 2 infra wake |
| 1 | 1 | `059` | 26M sleep (1) | 0 26M wake |
| 0 | 0 | `000` | 26M wake (0) | 1, 2 |
| 2 | 2 | `0c0` | infra wake (2) | 3, 4, 6 |
| 10 | 6 | `270` | VRF18 wake (6) | 4, 7 |
| 4 | 4 | `0f4` | APSRC wake (4) | 5, 8, 13 |
| 6 | 12 | `2a3` | DDREN wake (8) | `ddren_rearm`: 5, 8, 13 (SODI CG: 8, 13), or none and a CPU wake if one is pending |
| 8 | 8 | `1af` | Vcore/DRAM DVFS request | events 8 plus what was armed |

`PCM_EVENT_VECTOR_EN` is numbered by vector (the suspend value `0x2d7f`
is exactly the 11 vectors present); the r12 masks by event.

Read top to bottom the table is the descent: DRAM request (DDREN), AP
source (APSRC), the 1.8 V RF rail (VRF18), infra, and the 26 MHz clock
last. Every handler arms exactly the next step down and its own undo.
DDREN and APSRC are two ways in: the main program arms events 5 and 13
together. When APSRC drops first, its sleep takes DRAM down itself and
disarms DDREN; when DDREN drops first, APSRC follows (both seen, below).

**What each step does:**

- **DDREN sleep** (`2ae`): `dramc_22c_clr`; abort if the C2K modem needs
  VRF18 (`R13_C2K_VRF18_REQ`). Saves the PC to `PCM_REG_DATA_INI` and
  calls `emi_clk_off` (DRAM self-refresh, 21 tries); on a timeout restores
  and returns. Rechecks C2K, then `dram_phy_sleep` (DLLs, VREF and memory
  clock off). Clears `ST_DDREN`.
- **APSRC sleep** (`139`): pause masks, `MD_APSRC_ACK` low, DRAM PHY
  awake if DDREN had slept. **If the SCP asks for the high Vcore**
  (`SCP_VCORE_LEVEL` bit 1) while the DVS level is low, it forces a DVFS
  request (`DVFSRC_EVENT_FORCE_ON = 4`) and detours through `ev8_dvfs`
  first (`SPM_SW_RSV_5` bits 16-17 mark the detour). Then waits for
  `R13_EMI_IDLE`, `dqsosc_save`, `dramc_22c_clr`, `emi_clk_off`; on a
  timeout it re-acknowledges and returns. Otherwise `dramc_13fc_set`,
  `dram_phy_sleep`, `ddrphy_off` (the DDR PHY's MPLLs, and `DPY_PWR_CON`
  unless `DIS_DDRPHY_PDN`), `SCP_CLK_CON = 6`.
- **VRF18 sleep** (`28b`): `SC_26M_CK_SEL`, `SC_AXI_DCM_DIS`,
  `FREQH_PAUSE_MAIN`; `SPM_PLL_CON` `3 -> 0x303 -> 0x333` (the main PLLs
  off).
- **Infra sleep** (`0db`): unless `DIS_INFRA_PDN` or deep idle, all bus
  masters paused and infra powered off (`pwr_con_off` on `IFR_PWR_CON`).
  `SPM_SRC_RDY_STA = 0`. SODI sets `DIS_INFRA_PDN` itself (`836`).
- **26M sleep** (`059`): returns at once if any wake event is pending
  (r12), or unless bit 1 of `PCM_REG12_EXT_MASK_B_STA` is set (its own
  event still enabled, inferred). Sends PMIC-wrapper commands `0x15` (0
  with `SPM_FLAG1_VCORE_LP_0P7V`) and `0x14`, each waiting for the ack.
  Unless deep idle: `ulposc_on`, `pwrap_sleep` (the wrapper quiesced; on
  a timeout, `ulposc_off` and return), `pwrap_clk_sel` (`ULPOSC_CON` bit
  3 = 1), `ulposc_off`, then `PWRAP_SLEEP_REQ` released. In deep idle,
  command 7 instead. Then `SC_AXI_CK_OFF` (unless `DIS_BUS_CLOCK_OFF`),
  `SC_MD26M_CK_OFF`. Unless deep idle: `CPU_SYS_TIMER_CLK_SEL`,
  `PCM_CK_SEL0` (the PCM off the 26 MHz clock), `IM_SLEEP_ENABLE`,
  `SC_26M_CK_OFF`, `SC_CKSQ0_OFF`, and **`SRCCLKENO_0` and `SRCVOLTEN`
  low**: the 26 MHz request to the MT6358 drops, which is what puts it in
  low power.
- **The wakes** reverse their step: 26M wake raises `SRCVOLTEN` and
  `SRCCLKENO_0` first, moves the PMIC wrapper's clock back and waits for
  `SPM_WAKEUP_MISC` bit 30, then restores Vcore through command
  `SPM_DVS_LEVEL[1]` (slot 0 or 1: our `VCORE_PMIC_LOW`/`HIGH`) and Vmodem
  through `0x10 | SPM_DVS_LEVEL[17]`. Infra wake runs `pwr_con_on`. APSRC
  wake: VRF18 first if it is still down, `ddrphy_on`, `dram_phy_wake`,
  `dramc_22c_set`, `dram_mr4_read` (LPDDR4 MR4, the refresh rate, on both
  channels), `dramc_13fc_clr`, `SCP_CLK_CON = 0`. DDREN wake:
  `dram_phy_wake`, `dramc_22c_set`, `ddren_rearm`.

### The main program (`800`)

1. Entry: `r1 = 1`, `r7 = 0x15a00`, bus-pause masks cleared, PCM timer
   on; `r6 = SPM_SW_FLAG | 0xf8000000` (all resources awake), bit 24 from
   `SPM_SW_RSV_5[31]`; `SPM_CPU_WAKEUP_EVENT`, `SPM_SW_DEBUG`,
   `WDT_LATCH_SPARE0_FIX` and `SPM_SW_RSV_0` cleared;
   `DRAMC_DPY_CLK_SW_CON_SEL = 0xffffff`, `_SEL2 = 0x3ff7f`.
2. `831` `RUN_COMMON_SCENARIO`: straight to the common scenario. This is
   how the kernel starts the firmware at boot (`VCOREFS_FLAGS 0x8418`).
3. `834` SODI: `DIS_INFRA_PDN` set in r6; unless `ENABLE_SODI3`,
   `FORCE_INFRA_WAKE`.
4. Only vector 8 enabled. `845` unless `DIS_ATF_ABORT`: a pending wake is
   saved to `SPM_SW_RSV_0` and the sleep abandoned (to `b23`).
5. Bus masters paused (`bus_pause`, then `BUS_PROTECT2_RDY` polled for
   `0x3c0000`); TX tracking off.
6. `875` unless `DIS_CPU_PDN`, the CPU top goes down: `CSYSPWRUPACK` low
   (unless `KEEP_CSYSPWRUPACK_HIGH`); the last core (`ROOT_CORE_ADDR`, set
   by ATF) off; bus protection (`0xc3cfc00`); the cluster
   (`ROOT_CPUTOP_ADDR`) off; with `BIG_BUCK_OFF_ENABLE` and without
   `ENABLE_CPU_SLEEP_VOLT`, the A73 buck off through MCUCFG `+0x2008`;
   bus protection again with EMI `+0x400` bit 3; then MCUSYS
   (`MCU_PWR_CON`): isolation, its SRAM down (or, with
   `ENABLE_CPU_DORMANT`, into retention, waiting for bit 28), reset,
   clock off, power off; `CPU_EXT_BUCK_ISO = 7`. Debug `CPU_PDN`.
7. `920` unless `DIS_CPU_PDN`: wake latches cleared; a wake already
   pending is saved to `SPM_SW_RSV_0` and the CPU top restored at once
   (`a70`).
8. `93b` SYSRAM to sleep (unless `DIS_SYSRAM_SLEEP`). `945` unless
   `DIS_VPROC_VSRAM_DVS`: VPROC and VSRAM to their sleep voltages, four
   PMIC-wrapper commands chosen by `BIG_BUCK_OFF_ENABLE` and
   `ENABLE_CPU_SLEEP_VOLT` (`0xa`/`0x4`, `0x16`/`0xe`, `0x8`/`0x2`,
   `0x14`/`0xc`). `armpll_clk_off`.
9. Debug `SSPM_WFI` if `SUBSYS_IDLE_STA` bit 11 (the SSPM in WFI); SSPM
   SRAM to sleep unless `DIS_SSPM_SRAM_SLEEP` or the SSPM was not in WFI,
   then debug `SSPM_SRAM_SLP`. `drs_hold`.
10. `9a2` arm the ladder by scenario, and wait:

    | Scenario | `PCM_EVENT_VECTOR_EN` | first events |
    |---|---|---|
    | suspend, deep idle, SODI3 | `0x2d7f` (all) | 5, 8, 13 |
    | SODI | `0x2d70` (no 26M or infra) | 5, 8, 13 |
    | SODI, `SODI_CG_MODE` | `0x2140` | 8, 13 |
    | SODI, `SODI_NO_EVENT` | 0 | none |

11. `9d0` wake: the wake handler of each resource still down, in the
    order 26M, infra, VRF18, DDREN, APSRC; event 8 if
    `SPM_WAKEUP_EXT_STA` bit 8; the wake status to `SPM_SW_RSV_0`;
    `drs_release`; vector 8 again.
12. SYSRAM back; SSPM SRAM back if debug bit 20 says it slept; ARMPLL
    clock steps back (`0x1ff`, `0x3f`, `7`, 0); VPROC and VSRAM back
    (commands `0x17`/`0xf`, `0xb`/`0x5`, `0x15`/`0xd`, `0x9`/`0x3`).
13. `a71` unless `DIS_CPU_PDN`, the CPU top back in reverse:
    `CPU_EXT_BUCK_ISO = 3`, MCUSYS power on (waiting for `PWR_STATUS` and
    `PWR_STATUS_2ND` bit 14), its SRAM, isolation off; if the root
    cluster is not MP0 (`ROOT_CPUTOP_ADDR` is not `MP0_CPUTOP_PWR_CON`) and
    `BIG_BUCK_ON_ENABLE` (without `ENABLE_CPU_SLEEP_VOLT`), the A73 buck
    on, MCUCFG `+0x2208` bits 19:16 set, and **`SW2SPM_MAILBOX_3` copied to
    MCUCFG `+0x2290`, `+0x2298`, `+0x22c0`, `+0x22c8`** (values ATF leaves
    for the A73 cluster); the cluster and the last core on; for MP1,
    (`MP1_CPUTOP_PWR_CON`) MCUCFG `+0x2728`; TX tracking on.
14. `b23` resume: the PCM watchdog value (unless SODI or deep idle),
    timer off, `CSYSPWRUPACK`, bus-pause masks cleared, watchdog kick;
    the wake status to `SPM_BSI_D0_SR` (`SPM_WAKEUP_MISC`), `D1`
    (`PCM_TIMER_OUT`), `D2` (`PCM_EVENT_REG_STA`); `r11 = 0x55aa55aa`;
    `SPM_SWINT_SET = 1` (the interrupt to the CPU).
15. `b4e` unless `DIS_COMMON_SCENARIO`: the common scenario.

### The common scenario (`b50`)

The PCM back on its 26 MHz clock (`PCM_CK_SEL0/1` cleared), r6 bit 31 set
and bit 24 from `SPM_SW_RSV_5[31]`, debug `VCORE_DVFS_START`, vector 8
(Vcore DVFS) enabled, `PCM_CON1 |= 0x0b160000`. **Unless
`SPM_FLAG1_DISABLE_MCDSR`** (which ATF always sets here), it also forces
APSRC awake and arms DDREN sleep and wake (`PCM_EVENT_VECTOR_EN
0x2140`): with the system awake,
DRAM goes to self-refresh whenever every master's DRAM request drops
(MCDSR). It then loops on `SPM_CPU_WAKEUP_EVENT` (the next kick),
recording `SPM_WAKEUP_MISC` bits 18-20 into `SPM_SW_RSV_5` bits 28-30
meanwhile. On the kick it releases `FORCE_APSRC_WAKE`, wakes DDREN if it
is down, sets `r11 = 0x77887788` and, unless `DISABLE_NO_RESUME`, jumps
back to `802` (the main program) without stopping.

### Vcore and DRAM frequency (event 8, `1af`)

Event 8 is a DVFS request (from DVFSRC, `SPM_DVFS_EVENT_STA`):

1. The DRAM PHY awake if DDREN slept; the PCM on 26 MHz.
2. `dvfs_requests`: the requested level (priority masks `0x888`, `0x666`,
   `0x111` over the request nibbles) against the last served
   (`SPM_SW_RSV_5[15:0]`); the comparison into r6 bit 23 (the direction).
3. `dfs_target` (unless `DIS_VCORE_DFS`): the DRAM shuffle level wanted.
   When it differs from `SPM_DFS_LEVEL` the DRAM frequency is switched
   either before the voltage change (`1c3`) or after it (`250`).
4. Vcore (unless `DIS_VCORE_DVS`): target from the masks `0xffffccc` /
   `0x333`. Unless `DISABLE_MMSYS_DVFS`, MM clocks to the low setting
   first (`SC_MM_CK_SEL_CON = 0x13`). Dummy reads on, `dvfs_cmd` with
   slot 0 or 1, settle (`r14 0x330`), dummy reads off, MM clocks back;
   `SPM_DVS_LEVEL[7:0]` updated.
5. Vmodem (unless `DIS_VMODEM_DVS`): target from four nibble masks,
   `dvfs_cmd` slot `0x10`-`0x13` with ack-check timer 4; `SPM_DVS_LEVEL`
   bits 16 and up updated; `SPM2RC_DVS_DONE` per `dvfsrc_ack_chk`.
6. The DRAM frequency switch, when it comes after: `drs_hold`,
   `dfs_prepare`, `dfs_shuffle`, the system-timer count to
   `SPM_SW_RSV_9` (a timestamp), dummy reads off.
7. `dvfs_done`: DRS and MM released, `SPM_DVFS_LEVEL` published, the
   served requests to `SPM_SW_RSV_5`, `SPM_SWINT_SET` bit 7 with
   `EN_MET_DEBUG_USAGE`; event 8 re-armed with what was armed before.
8. A detour from the APSRC handlers (`SPM_SW_RSV_5` bit 16) returns into
   them: bit 17 to the APSRC sleep's continuation, bit 18 to its abort
   path, otherwise to the APSRC wake's.

The DRAM frequency switch (`dfs_prepare`, `dfs_shuffle`) holds DRAM
requests (`SPM_DRS_CON = 3`, polled for `0x303`). It also tells MM
(`SPM2MM_CON = 0x1f`), writes the target to r0[31:30], and sets DRAMC
`SPCMDCTRL` bit 29. It takes the hardware semaphore `SPM_SPM_SEMA`
(timer 2: on a timeout `SPM_SWINT_SET = 3` and DVFSRC `+0x4f0` to
`SPM2SW_MAILBOX_3`). It then alternates the DRAM PHY PLL (r6 bit 24,
`PHYPLL`/`PHYPLL2`), saves the tracking results, and parks DDRPHY
`+0x274`/`+0x27c` in `SPM_PASR_DPD_0-3`. It pauses the bus masters and
polls `BUS_PROTECT_RDY` for `0x600006` (timer 3). Last it requests the
shuffle (`SC_DR_SHU_EN_PCM`), waits for `R13_SW_DMDRAMCSHU_ACK_ALL`, and
restores.

## Routines

Names are ours (`NAMES` in the tool), from what each routine does.

| Entry | Name | What it does |
|---|---|---|
| `2ee` | `ddren_rearm` | the events after a DDREN step; a pending wake (`PCM_REG12_MASK_B_STA`) sets `SPM_CPU_WAKEUP_EVENT` and arms nothing |
| `305` | `pwr_con_on` | power-on sequence on the `PWR_CON` in r3 (`0x316` ... `0x00d`), delay r8 |
| `318` | `pwr_con_off` | power-off sequence (`0x30d` ... `0x312`) |
| `31e` | `ddrphy_off` | DDR PHY MPLLs off; `DPY_PWR_CON` off unless `DIS_DDRPHY_PDN` |
| `333` | `ddrphy_on` | the reverse, with the shuffle restore pulse |
| `357` | `dram_phy_sleep` | DLLs, VREF and memory clock off, frequency hopping paused |
| `362` | `dram_phy_wake` | the reverse; `EMI_CLK_OFF_REQ` released and its ack awaited; PHY precal |
| `37e` | `emi_clk_off` | `EMI_CLK_OFF_REQ` with 21 tries; on a timeout `SPM_CPU_WAKEUP_EVENT = 1`, `WDT_LATCH_SPARE0_FIX` bit 0, r3 = 1 |
| `39a` | `bus_pause` | `TOP_MAS_PAU_REQ` |
| `39f` | `ulposc_on` | the ULPOSC on (records whether it was, `SPM_SW_RSV_2` bit 10) |
| `3be` | `ulposc_off` | off, unless it was on or `DISABLE_ULPOSC_OFF` |
| `3d5` | `pwrap_clk_sel` | `ULPOSC_CON` bit 3 = r5, unless `DISABLE_PWRAP_CLK_SWITCH` |
| `3e5` | `pwrap_sleep` | `PWRAP_SLEEP_REQ` with ack-check timer; on a timeout `SPM_CPU_WAKEUP_EVENT = 1`, `WDT_LATCH_SPARE0_FIX` bit 1 |
| `408` | `debug_or` | `SPM_SW_DEBUG \|= r10` |
| `410` | `dvfs_ack_wait` | wait for `SPM_DVFS_CON1_STA` bit 9; a failed `SPM_ACK_CHK_STA2` into `WDT_LATCH_SPARE0_FIX` bit 2 |
| `420` / `540` | `drs_release` / `drs_hold` | `SPM_DRS_CON` 0 / 3 (polled for `0x303`) |
| `425` / `428` | `tx_tracking_off` / `_on` | `SC_TX_TRACKING_DIS` |
| `42f` | `dram_mr4_read` | per channel: `SPCMDCTRL`, `MRS` = MR4, `SPCMD` MRR, wait for the response (NAO `+0x088` bit 1) |
| `489` / `49b` | `dramc_13fc_set` / `_clr` | DRAMC NAO `+0x13fc` bit 0, both channels |
| `4ad` / `4bb` | `dramc_22c_clr` / `_set` | DRAMC AO A `+0x22c` bit 31, B `+0x04c` bit 2 (set only at shuffle level 0) |
| `4cf` | `dqsosc_save` | at shuffle level 0: NAO `+0x468` and `+0x460` read three times; when two agree and are non-zero, copied to AO `+0x8a8`/`+0x8ac` and `+0x8b0`/`+0x8b4` of both channels |
| `52c` | `dfs_target` | the DRAM level wanted (one-hot r10); r4 set if `SPM_DFS_LEVEL` is already there |
| `54d` | `dvfs_requests` | see event 8 |
| `571` | `dvfs_done` | see event 8 |
| `595` | `dfs_prepare` | see event 8 |
| `5d1` | `dfs_shuffle` | the DRAM frequency switch (412 words) |
| `76d` / `77f` | `dummy_read_on` / `_off` | `DRAMC_DMYRD_CON` bits 0-2, 8-10 |
| `790` | `dvfs_cmd` | PMIC-wrapper command r10 through `SPM_DVFS_CON1`, wait for the ack |
| `79a` | `dvfsrc_ack_chk` | r10 = (`SPM_DVFS_MISC[9:8]` == 1) |
| `ba3` | `armpll_clk_off` | `ARMPLL_CLK_CON` `7`, `0x3f`, `0x1ff`, `0x3ff` |

The PMIC-wrapper commands are slots of a table ATF fills (not in the
image); our driver sets slots 0 and 1 (`VCORE_PMIC_LOW`/`HIGH`).

## Software state the program keeps

| Register | Content |
|---|---|
| `SPM_SW_FLAG` | the flags (copied to r6) |
| `SPM_SW_RSV_2` | the second flag word; bit 10 "the ULPOSC was on" |
| `SPM_SW_DEBUG` | the path of the last sleep (below) |
| `WDT_LATCH_SPARE0_FIX` | handshake timeouts (`SPM_DBG1_*`: bit 0 EMI clock off, 1 PMIC wrapper sleep, 2 PMIC wrapper DVFS) |
| `PCM_REG_DATA_INI` | breadcrumbs: the PC before the EMI and PMIC-wrapper handshakes (0 after); `0xa00000db \| ready bits` while a bus-protection poll waits. The vendor reads it as `assert_pc`; ours logs it as `pc` |
| `SPM_SW_RSV_0` | the wake status at an abort or the wake |
| `SPM_SW_RSV_5` | 15:0 the DVFS requests served; 16-19 a DVFS detour from the APSRC handlers; 28-30 `SPM_WAKEUP_MISC` 18-20 seen while awake; 31 the active DRAM PHY PLL |
| `SPM_SW_RSV_9` | system-timer count at the last DRAM frequency switch |
| `SPM_SW_RSV_10` | `DVFSRC_EVENT_FORCE_ON` saved during a detour |
| `SPM_BSI_D0/D1/D2_SR` | `SPM_WAKEUP_MISC`, `PCM_TIMER_OUT`, `PCM_EVENT_REG_STA` at resume |
| `SPM_DVS_LEVEL`, `SPM_DFS_LEVEL`, `SPM_DVFS_LEVEL` | current Vcore (7:0) and Vmodem (23:16) levels, DRAM level, the published level |
| `SPM_CPU_WAKEUP_EVENT` | the kick (kernel to PCM), and set by the PCM to wake the CPU on a failure |
| `SPM_SWINT_SET` | interrupts to the CPU: 1 resume, 3 semaphore timeout, bit 7 MET |
| `SW2SPM_MAILBOX_3` | from ATF: restored into MCUCFG after the A73 buck is back on |

## Reading `SPM_SW_DEBUG`

The main program clears it at entry; each step ORs in its bit (the
`debug_or` call's delay slot sets `r10 = 1 << n`). Values from our runs
(mainline.md, "Suspend"):

| `debug` | Path |
|---|---|
| `0x1910ff` | all the way: APSRC, VRF18, infra and 26 MHz down and back (bits 0-7); the best run, 29.9 mA |
| `0x1910fc` | down to infra; the 26 MHz step never fired (its request stayed up) |
| `0x191300` | DDREN only: DRAM self-refresh, APSRC never released, nothing below |

| `0x1913ff` | all the way, DDREN first then APSRC (2026-11-03, 14.2 s, RTC wake) |
| `0x11000` | CPU top down, then the second abort check (`920`) found a wake pending (`SPM_SW_RSV_0 = 0x800`, the SCP's): nothing else ran |

Bits 12 (`CPU_PDN`), 16 (`VCORE_DVFS_START`, the common scenario), 19
(`SSPM_WFI`) and 20 (`SSPM_SRAM_SLP`) are the main program's. Bits 8-9
are clear on a full path that APSRC led.

## Flags

`--effects` lists all 73 flag-gated skips. By flag:

| Flag | Where | Effect when set |
|---|---|---|
| `DIS_CPU_PDN` (0) | `875`, `920`, `a71` | no CPU-top power-down, no second abort check |
| `DIS_INFRA_PDN` (1) | infra handlers | infra stays powered (SODI sets it) |
| `DIS_DDRPHY_PDN` (2) | `ddrphy_off/on` | `DPY_PWR_CON` stays on |
| `DIS_VCORE_DVS` (3) | `1d9` | no Vcore change on DVFS requests |
| `DIS_VCORE_DFS` (4) | `dfs_target` | no DRAM frequency change |
| `DIS_COMMON_SCENARIO` (5) | `b4e` | stops after resume instead of looping awake |
| `DIS_BUS_CLOCK_OFF` (6) | 26M handlers | the AXI clock stays on |
| `DIS_ATF_ABORT` (7) | `845` | no early abort on a pending wake |
| `KEEP_CSYSPWRUPACK_HIGH` (8) | `878` | |
| `DIS_VPROC_VSRAM_DVS` (9) | `945`, `a2e` | no CPU sleep voltages |
| `RUN_COMMON_SCENARIO` (10) | `831` | the boot entry |
| `EN_MET_DEBUG_USAGE` (11) | `dvfs_done` | an interrupt per DVFS |
| `SODI_CG_MODE` (12), `SODI_NO_EVENT` (13), `ENABLE_SODI3` (14), `SODI_OPTION` (21) | main, ladder | the SODI scenarios |
| `DISABLE_MMSYS_DVFS` (15) | `1e3` | MM clocks untouched on DVFS |
| `DIS_SYSRAM_SLEEP` (16), `DIS_SSPM_SRAM_SLEEP` (17) | `93b`, `991` | keep those SRAMs awake |
| `DIS_VMODEM_DVS` (18) | `208` | no Vmodem change |
| `SUSPEND_OPTION` (19) | | **never tested**: suspend is the default |
| `DEEPIDLE_OPTION` (20) | the 26M and infra handlers | deep idle: SRCCLKEN0 stays up, infra stays powered, no PMIC-wrapper clock switch |
| bits 22-24, 31 (`DONT_TOUCH`) | DVFS code | firmware state: 23 direction, 24 PLL |
| `FLAG1_ENABLE_CPU_DORMANT` (1) | `8ec`, `a96` | MCUSYS SRAM retained instead of off |
| `FLAG1_ENABLE_CPU_SLEEP_VOLT` (2) | `945`ff | other VPROC/VSRAM commands; also stops the big-buck path |
| `FLAG1_DISABLE_PWRAP_CLK_SWITCH` (3) | `pwrap_clk_sel` | |
| `FLAG1_DISABLE_ULPOSC_OFF` (4) | `ulposc_off` | |
| `FLAG1_VCORE_LP_0P7V` (5) | 26M sleep | command 0 instead of `0x15` |
| `FLAG1_DISABLE_MCDSR` (6) | `b6d` | no DRAM self-refresh in the common scenario |
| `FLAG1_DISABLE_NO_RESUME` (7) | `b9a` | stop instead of re-entering at `802` |
| `FLAG1_BIG_BUCK_OFF/ON_ENABLE` (8, 9) | `8aa`, `adc` | A73 buck off at sleep, on at wake: **set both or neither** |

Our suspend flags, `0x80098`, skip the Vcore DVS and DFS steps and the
ATF abort; `SUSPEND_OPTION` is ignored. Everything else runs: CPU-top
power-down, DDR PHY power-down, the full ladder, SYSRAM and SSPM SRAM
sleep, CPU sleep voltages, and after resume the common scenario.
ATF adjusts both words on the way (measured, below): `SPM_SW_FLAG` reads
`0x88098` (bit 15 `DISABLE_MMSYS_DVFS` added) and `SPM_SW_RSV_2` reads our
`pcm_flags1 | 0x40`, so `DISABLE_MCDSR` is always set: MCDSR never runs
on this phone.

## Measured on the phone

`tools/spm/spmprobe.sh` reads the firmware's live state: the
SPM software registers and the PCM's register mirrors (`PCM_REG0_DATA`
to `PCM_REG15_DATA`, `0x10006100`), with the PC sampled. Run on build
#100 with `mt6771_spm.deep=1` (2026-11-03), at boot and after two deep
sleeps (`deep-test.sh`, radios off, 15 s RTC wake):

- **The PC sits at `b79` in every sample** (328 of 328, each time), the
  common scenario's wait right after `r14 <- 0x90100000`, and r14 reads
  `0x90100000`.
- **r6** reads `0xf9008418` at boot (`VCOREFS_FLAGS` with all five
  resources awake and bit 24 from `SPM_SW_RSV_5[31] = 1`), `0xf9088098`
  after a suspend. r3 and r4 hold the `PCM_CON1` update, r10 `1 << 16`
  (the last `debug_or`), r8 and r9 the `SPM_DVFS_CON1` and
  `WDT_LATCH_SPARE0_FIX` addresses `dvfs_cmd` and `dvfs_ack_wait` leave.
- **r7** after a resume has `PCM_TIMER_SET` cleared and `WDT_KICK_P` set,
  the resume steps at `b2a`-`b33`.
- **At boot** `SPM_SW_DEBUG = 0x10000` (only `VCORE_DVFS_START`: the boot
  entry goes straight to the common scenario) and `PCM_EVENT_VECTOR_EN =
  0x100`; `SPM_SW_RSV_2 = 0x40`.
- **Flag words:** we passed `pcm_flags1` 0 and then `0x10`; the firmware
  saw `0x40` and `0x50`. ATF ORs in `DISABLE_MCDSR`, and adds
  `DISABLE_MMSYS_DVFS` to the suspend flags.
- **The two sleeps:** with the SCP's wake unmasked the sleep ended at
  once (`debug 0x11000`, the `920` abort, `SPM_SW_RSV_0 = r9 = 0x800`);
  with `wake_mask 0xe87d5cf` it slept 14.2 s and woke on the RTC's EINT
  (`r12 0x40`), `debug 0x1913ff`. Both: `fail 0`, `pc 0`.
- **Not explained:** the r11 mirror reads `0xa` throughout, never the
  resume marker `0x55aa55aa` the code writes at `b48`, though r7 shows the
  resume path ran. Either `PCM_REG11_DATA` is not a plain mirror of r11
  or the register is special.

## What is still inferred

- Op 4's compare direction (one use: the DFS direction bit) and op 3/9
  as XOR.
- r14's wait forms beyond the plain delays.
- The r12 multiplexing is read from how r7's `W_SEL`/`R_SEL` bracket
  every r12 access; consistent everywhere, not measured.
- The PMIC-wrapper command slots other than 0 and 1 (ATF's table).
- DRAMC registers without a vendor name (`+0x22c`/`+0x04c`, NAO
  `+0x13fc`, `+0x460`/`+0x468`, AO `+0x864`, `+0x8a8`-`+0x8b4`).

## Leads for deep sleep on 6.12

- **Log `WDT_LATCH_SPARE0_FIX` with each suspend.** A set bit names the
  handshake that timed out. `PCM_REG_DATA_INI` (our `pc`) is then the
  breadcrumb: a PC inside `emi_clk_off`/`pwrap_sleep`, or `0xa00000db |
  bits` for a bus-protection wait that never completed.
- **The SCP steers the APSRC step.** When the SCP asks for the high
  Vcore, APSRC sleep first runs a DVFS. A misconfigured SCP (the #86-#90
  regression, mainline-handoff.md item 0b) shows up here.
- **The A73 buck.** `BIG_BUCK_ON_ENABLE` restores MCUCFG from
  `SW2SPM_MAILBOX_3`, which ATF must have filled. Use `pcm_flags1=0x300`
  only when ATF is known to do that on our boot path.
- A run that stops above 26 MHz (`0x1910fc`) means a 26 MHz requester
  stayed up. The 26M handler's other exits are a pending wake and a
  PMIC-wrapper sleep timeout (`WDT_LATCH_SPARE0_FIX` bit 1).
