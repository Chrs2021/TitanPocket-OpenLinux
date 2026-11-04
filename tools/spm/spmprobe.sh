# spmprobe.sh: the SPM firmware's live state (docs/spm-firmware.md) from its register mirrors,
# to check the static reading against the running PCM. Needs the firmware running (boot with
# mt6771_spm.deep=1). Usage: sh spmprobe.sh [SAMPLES]  (r15, the PC, sampled SAMPLES times)
S=0x10006000
r() { busybox devmem $((S + $1)) 32; }
for n in "0x080 PCM_EVENT_VECTOR_EN" "0x0b0 SPM_CPU_WAKEUP_EVENT" "0x600 SPM_SW_FLAG" \
	 "0x610 SPM_SW_RSV_2" "0x604 SPM_SW_DEBUG" "0x61c SPM_SW_RSV_5" "0x608 SPM_SW_RSV_0" \
	 "0x028 PCM_REG_DATA_INI" "0x780 WDT_LATCH_SPARE0_FIX" "0x6b0 SPM_DFS_LEVEL" \
	 "0x6b4 SPM_DVS_LEVEL" "0x140 PCM_REG12_MASK_B_STA" "0x148 PCM_REG12_EXT_MASK_B_STA"; do
	set -- $n; echo "$2 $(r $1)"
done
i=0; for o in 0x100 0x104 0x108 0x10c 0x110 0x114 0x118 0x11c 0x120 0x124 0x128 0x12c 0x130 0x134 0x138; do
	printf "r%d %s  " $i $(r $o); i=$((i + 1)); [ $((i % 4)) = 0 ] && echo
done; echo
echo "r15 (pc) samples: $(for i in $(seq ${1:-20}); do r 0x13c; done | sort | uniq -c | tr '\n' ' ')"
