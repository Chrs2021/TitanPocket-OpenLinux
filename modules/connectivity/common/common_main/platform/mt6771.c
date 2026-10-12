/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */
/*! \file
*    \brief  MT6771 CONSYS platform ops
*
* MediaTek did not publish this file. It was reconstructed for the Titan
* Pocket project from the stock wmt_drv.ko (code at .text 0x3910-0x58e0, ops
* table consys_ic_ops at .data 0x1f8), with mt6765.c as the template. Hooks
* the stock table leaves NULL are left out here too. The comments name the
* stock code offset each function was rebuilt from.
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

#include <connectivity_build_in_adapter.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/memblock.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include "osal_typedef.h"
#include "mt6771.h"
#include "mtk_wcn_consys_hw.h"
#include "wmt_ic.h"
#include "wmt_lib.h"
#include "wmt_plat.h"
#include "stp_dbg.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
#include <mtk-clkbuf-bridge.h>
#else
#include <mtk_clkbuf_ctl.h>
#endif

#ifdef CONFIG_MTK_EMI
#include <mt_emi_api.h>
#endif

#if CONSYS_PMIC_CTRL_ENABLE
#include <linux/regulator/consumer.h>
#include <upmu_common.h>
#endif

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
static INT32 consys_clock_buffer_ctrl(MTK_WCN_BOOL enable);
static VOID consys_hw_reset_bit_set(MTK_WCN_BOOL enable);
static VOID consys_hw_spm_clk_gating_enable(VOID);
static INT32 consys_hw_power_ctrl(MTK_WCN_BOOL enable);
static INT32 consys_ahb_clock_ctrl(MTK_WCN_BOOL enable);
static INT32 polling_consys_chipid(VOID);
static VOID update_consys_rom_desel_value(VOID);
static VOID consys_hang_debug(VOID);
static VOID consys_acr_reg_setting(VOID);
static VOID consys_afe_reg_setting(VOID);
static INT32 consys_hw_vcn18_ctrl(MTK_WCN_BOOL enable);
static VOID consys_vcn28_hw_mode_ctrl(UINT32 enable);
static INT32 consys_hw_vcn28_ctrl(UINT32 enable);
static INT32 consys_hw_wifi_vcn33_ctrl(UINT32 enable);
static INT32 consys_hw_bt_vcn33_ctrl(UINT32 enable);
static UINT32 consys_soc_chipid_get(VOID);
static INT32 consys_emi_mpu_set_region_protection(VOID);
static UINT32 consys_emi_set_remapping_reg(VOID);
static INT32 bt_wifi_share_v33_spin_lock_init(VOID);
static INT32 consys_clk_get_from_dts(struct platform_device *pdev);
static INT32 consys_pmic_get_from_dts(struct platform_device *pdev);
static INT32 consys_read_irq_info_from_dts(struct platform_device *pdev, PINT32 irq_num, PUINT32 irq_flag);
static INT32 consys_read_reg_from_dts(struct platform_device *pdev);
static UINT32 consys_read_cpupcr(VOID);
static VOID force_trigger_assert_debug_pin(VOID);
static INT32 consys_co_clock_type(VOID);
static P_CONSYS_EMI_ADDR_INFO consys_soc_get_emi_phy_add(VOID);
static VOID consys_set_if_pinmux(MTK_WCN_BOOL enable);
static INT32 consys_emi_coredump_remapping(UINT8 __iomem **addr, UINT32 enable);
static INT32 consys_reset_emi_coredump(UINT8 __iomem *addr);
static INT32 consys_dump_osc_state(P_CONSYS_STATE state);
static UINT64 consys_get_options(VOID);

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/
#if CONSYS_BT_WIFI_SHARE_V33
struct bt_wifi_v33_status gBtWifiV33;
#endif

/* CCF part */
struct clk *clk_scp_conn_main;	/*ctrl conn_power_on/off */

/* PMIC part */
#if CONSYS_PMIC_CTRL_ENABLE
static struct regulator *reg_VCN18;
static struct regulator *reg_VCN28;
static struct regulator *reg_VCN33_BT;
static struct regulator *reg_VCN33_WIFI;
#endif

EMI_CTRL_STATE_OFFSET mtk_wcn_emi_state_off = {
	.emi_apmem_ctrl_state = EXP_APMEM_CTRL_STATE,
	.emi_apmem_ctrl_host_sync_state = EXP_APMEM_CTRL_HOST_SYNC_STATE,
	.emi_apmem_ctrl_host_sync_num = EXP_APMEM_CTRL_HOST_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_state = EXP_APMEM_CTRL_CHIP_SYNC_STATE,
	.emi_apmem_ctrl_chip_sync_num = EXP_APMEM_CTRL_CHIP_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_addr = EXP_APMEM_CTRL_CHIP_SYNC_ADDR,
	.emi_apmem_ctrl_chip_sync_len = EXP_APMEM_CTRL_CHIP_SYNC_LEN,
	.emi_apmem_ctrl_chip_print_buff_start = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_START,
	.emi_apmem_ctrl_chip_print_buff_len = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_LEN,
	.emi_apmem_ctrl_chip_print_buff_idx = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX,
	.emi_apmem_ctrl_chip_int_status = EXP_APMEM_CTRL_CHIP_INT_STATUS,
	.emi_apmem_ctrl_chip_paded_dump_end = EXP_APMEM_CTRL_CHIP_PAGED_DUMP_END,
	.emi_apmem_ctrl_host_outband_assert_w1 = EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1,
	.emi_apmem_ctrl_chip_page_dump_num = EXP_APMEM_CTRL_CHIP_PAGE_DUMP_NUM,
	.emi_apmem_ctrl_assert_flag = EXP_APMEM_CTRL_ASSERT_FLAG,
};

CONSYS_EMI_ADDR_INFO mtk_wcn_emi_addr_info = {
	.emi_phy_addr = CONSYS_EMI_FW_PHY_BASE,
	.paged_trace_off = CONSYS_EMI_PAGED_TRACE_OFFSET,
	.paged_dump_off = CONSYS_EMI_PAGED_DUMP_OFFSET,
	.full_dump_off = CONSYS_EMI_FULL_DUMP_OFFSET,
	.emi_remap_offset = CONSYS_EMI_MAPPING_OFFSET,
	.p_ecso = &mtk_wcn_emi_state_off,
	.emi_core_dump_offset = CONSYS_EMI_COREDUMP_OFFSET,
};

WMT_CONSYS_IC_OPS consys_ic_ops = {
	.consys_ic_clock_buffer_ctrl = consys_clock_buffer_ctrl,
	.consys_ic_hw_reset_bit_set = consys_hw_reset_bit_set,
	.consys_ic_hw_spm_clk_gating_enable = consys_hw_spm_clk_gating_enable,
	.consys_ic_hw_power_ctrl = consys_hw_power_ctrl,
	.consys_ic_ahb_clock_ctrl = consys_ahb_clock_ctrl,
	.polling_consys_ic_chipid = polling_consys_chipid,
	.update_consys_rom_desel_value = update_consys_rom_desel_value,
	.consys_hang_debug = consys_hang_debug,
	.consys_ic_acr_reg_setting = consys_acr_reg_setting,
	.consys_ic_afe_reg_setting = consys_afe_reg_setting,
	.consys_ic_hw_vcn18_ctrl = consys_hw_vcn18_ctrl,
	.consys_ic_vcn28_hw_mode_ctrl = consys_vcn28_hw_mode_ctrl,
	.consys_ic_hw_vcn28_ctrl = consys_hw_vcn28_ctrl,
	.consys_ic_hw_wifi_vcn33_ctrl = consys_hw_wifi_vcn33_ctrl,
	.consys_ic_hw_bt_vcn33_ctrl = consys_hw_bt_vcn33_ctrl,
	.consys_ic_soc_chipid_get = consys_soc_chipid_get,
	.consys_ic_emi_mpu_set_region_protection = consys_emi_mpu_set_region_protection,
	.consys_ic_emi_set_remapping_reg = consys_emi_set_remapping_reg,
	.ic_bt_wifi_share_v33_spin_lock_init = bt_wifi_share_v33_spin_lock_init,
	.consys_ic_clk_get_from_dts = consys_clk_get_from_dts,
	.consys_ic_pmic_get_from_dts = consys_pmic_get_from_dts,
	.consys_ic_read_irq_info_from_dts = consys_read_irq_info_from_dts,
	.consys_ic_read_reg_from_dts = consys_read_reg_from_dts,
	.consys_ic_read_cpupcr = consys_read_cpupcr,
	.ic_force_trigger_assert_debug_pin = force_trigger_assert_debug_pin,
	.consys_ic_co_clock_type = consys_co_clock_type,
	.consys_ic_soc_get_emi_phy_add = consys_soc_get_emi_phy_add,
	.consys_ic_set_if_pinmux = consys_set_if_pinmux,
	.consys_ic_emi_coredump_remapping = consys_emi_coredump_remapping,
	.consys_ic_reset_emi_coredump = consys_reset_emi_coredump,
	.consys_ic_dump_osc_state = consys_dump_osc_state,
	.consys_ic_get_options = consys_get_options,
};

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/
UINT32 gJtagCtrl;

INT32 mtk_wcn_consys_jtag_set_for_mcu(VOID)
{
	if (gJtagCtrl)
		WMT_PLAT_PR_INFO("unsupported options!\n");
	return 0;
}

UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en)
{
	WMT_PLAT_PR_INFO("%s jtag set for MCU\n", en ? "enable" : "disable");
	gJtagCtrl = en;
	return 0;
}

/* stock 0x48a0 */
static INT32 consys_clk_get_from_dts(struct platform_device *pdev)
{
	clk_scp_conn_main = devm_clk_get(&pdev->dev, "conn");
	if (IS_ERR(clk_scp_conn_main)) {
		WMT_PLAT_PR_ERR("[CCF]cannot get clk_scp_conn_main clock.\n");
		return PTR_ERR(clk_scp_conn_main);
	}
	WMT_PLAT_PR_DBG("[CCF]clk_scp_conn_main=%p\n", clk_scp_conn_main);

	return 0;
}

/* stock 0x496c */
static INT32 consys_pmic_get_from_dts(struct platform_device *pdev)
{
#if CONSYS_PMIC_CTRL_ENABLE
	reg_VCN18 = regulator_get(&pdev->dev, "vcn18");
	if (!reg_VCN18)
		WMT_PLAT_PR_ERR("Regulator_get VCN_1V8 fail\n");
	reg_VCN28 = regulator_get(&pdev->dev, "vcn28");
	if (!reg_VCN28)
		WMT_PLAT_PR_ERR("Regulator_get VCN_2V8 fail\n");
	reg_VCN33_BT = regulator_get(&pdev->dev, "vcn33_bt");
	if (!reg_VCN33_BT)
		WMT_PLAT_PR_ERR("Regulator_get VCN33_BT fail\n");
	reg_VCN33_WIFI = regulator_get(&pdev->dev, "vcn33_wifi");
	if (!reg_VCN33_WIFI)
		WMT_PLAT_PR_ERR("Regulator_get VCN33_WIFI fail\n");
#endif
	return 0;
}

/* stock 0x4fd0 */
static INT32 consys_co_clock_type(VOID)
{
	return 0;
}

/* stock 0x3910 */
static INT32 consys_clock_buffer_ctrl(MTK_WCN_BOOL enable)
{
	KERNEL_clk_buf_ctrl(CLK_BUF_CONN, enable ? true : false);	/* XO_WCN */
	return 0;
}

/* stock 0x4fec */
static VOID consys_set_if_pinmux(MTK_WCN_BOOL enable)
{
	UINT8 *consys_if_pinmux_reg_base = NULL;

	/* Switch D die pinmux for connecting A die */
	consys_if_pinmux_reg_base = ioremap_nocache(CONSYS_IF_PINMUX_REG_BASE, 0x1000);
	if (!consys_if_pinmux_reg_base) {
		WMT_PLAT_PR_ERR("consys_if_pinmux_reg_base(%x) ioremap fail\n", CONSYS_IF_PINMUX_REG_BASE);
		return;
	}

	if (enable) {
		CONSYS_REG_WRITE(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_01_OFFSET,
				(CONSYS_REG_READ(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_01_OFFSET) &
				CONSYS_IF_PINMUX_01_MASK) | CONSYS_IF_PINMUX_01_VALUE);
		CONSYS_REG_WRITE(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_02_OFFSET,
				(CONSYS_REG_READ(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_02_OFFSET) &
				CONSYS_IF_PINMUX_02_MASK) | CONSYS_IF_PINMUX_02_VALUE);
	} else {
		CONSYS_REG_WRITE(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_01_OFFSET,
				CONSYS_REG_READ(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_01_OFFSET) &
				CONSYS_IF_PINMUX_01_MASK);
		CONSYS_REG_WRITE(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_02_OFFSET,
				CONSYS_REG_READ(consys_if_pinmux_reg_base + CONSYS_IF_PINMUX_02_OFFSET) &
				CONSYS_IF_PINMUX_02_MASK);
	}

	iounmap(consys_if_pinmux_reg_base);
}

/* stock 0x3934 */
static VOID consys_hw_reset_bit_set(MTK_WCN_BOOL enable)
{
	if (enable) {
		/* assert CONNSYS CPU SW reset 0x10007018 [12]=1 [31:24]=0x88 (key) */
		CONSYS_REG_WRITE(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET,
				CONSYS_REG_READ(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET) |
				CONSYS_CPU_SW_RST_BIT | CONSYS_CPU_SW_RST_CTRL_KEY);
	} else {
		/* deassert CONNSYS CPU SW reset 0x10007018 [12]=0 [31:24]=0x88 (key) */
		CONSYS_REG_WRITE(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET,
				(CONSYS_REG_READ(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET) &
				 ~(CONSYS_CPU_SW_RST_BIT | CONSYS_CPU_SW_RST_CTRL_KEY)) |
				CONSYS_CPU_SW_RST_CTRL_KEY);
	}
}

/* stock 0x3990 */
static VOID consys_hw_spm_clk_gating_enable(VOID)
{
}

/* stock 0x3994: the "conn" clock is the SCPSYS CONN power domain */
static INT32 consys_hw_power_ctrl(MTK_WCN_BOOL enable)
{
	INT32 iRet = -1;

	if (enable) {
		iRet = clk_prepare_enable(clk_scp_conn_main);
		if (iRet)
			WMT_PLAT_PR_ERR("clk_prepare_enable(clk_scp_conn_main) fail(%d)\n", iRet);
		WMT_PLAT_PR_DBG("clk_prepare_enable(clk_scp_conn_main) ok\n");
	} else {
		clk_disable_unprepare(clk_scp_conn_main);
		WMT_PLAT_PR_DBG("clk_disable_unprepare(clk_scp_conn_main) calling\n");
	}

	return iRet;
}

/* stock 0x3ad8 */
static INT32 consys_ahb_clock_ctrl(MTK_WCN_BOOL enable)
{
	return 0;
}

/* stock 0x3ae0 */
static INT32 polling_consys_chipid(VOID)
{
	INT32 retry = 10;
	UINT32 consys_ver_id = 0;

	/* poll CONNSYS CHIP ID until chipid is returned 0x18070008 */
	while (retry-- > 0) {
		consys_ver_id = CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_CHIP_ID_OFFSET);
		if (consys_ver_id == CONSYS_CHIP_ID) {
			WMT_PLAT_PR_INFO("retry(%d)consys chipId(0x%08x)\n", retry, consys_ver_id);
			break;
		}
		msleep(20);
	}

	/* as in the stock driver, only a match on the last try is reported */
	if (retry == 0 || consys_ver_id == 0) {
		WMT_PLAT_PR_ERR("Maybe has a consys power on issue,(0x%08x)\n", consys_ver_id);
		WMT_PLAT_PR_INFO("reg dump:CONSYS_CPU_SW_RST_REG(0x%x)\n",
				CONSYS_REG_READ(conn_reg.ap_rgu_base + CONSYS_CPU_SW_RST_OFFSET));
		WMT_PLAT_PR_INFO("reg dump:CONSYS_PWR_CONN_ACK_REG(0x%x)\n",
				CONSYS_REG_READ(conn_reg.spm_base + CONSYS_PWR_CONN_ACK_OFFSET));
		WMT_PLAT_PR_INFO("reg dump:CONSYS_PWR_CONN_ACK_S_REG(0x%x)\n",
				CONSYS_REG_READ(conn_reg.spm_base + CONSYS_PWR_CONN_ACK_S_OFFSET));
		WMT_PLAT_PR_INFO("reg dump:CONSYS_TOP1_PWR_CTRL_REG(0x%x)\n",
				CONSYS_REG_READ(conn_reg.spm_base + CONSYS_TOP1_PWR_CTRL_OFFSET));
	}

	return 0;
}

/* stock 0x3ddc */
static VOID update_consys_rom_desel_value(VOID)
{
	CONSYS_REG_WRITE(conn_reg.mcu_base + CONSYS_MCU_ROM_DELSEL_OFFSET, CONSYS_MCU_ROM_DELSEL_VALUE);
	WMT_PLAT_PR_INFO("Update consys rom desel value(0x%x)\n",
			CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_MCU_ROM_DELSEL_OFFSET));
}

/*
 * stock 0x3e60: steer the CONNSYS debug mux (addr1/addr3 select, addr2 reads
 * back) through the AHB/APB probe points and log what it sees.
 */
static VOID consys_hang_debug_probe(SIZE_T addr1, SIZE_T addr2, SIZE_T addr3,
		UINT32 sel, UINT32 sel_mask, UINT32 step, UINT32 count,
		UINT32 probe_a, UINT32 probe_b)
{
	UINT32 i, v3, v_a, v_b;

	for (i = 0; i < count; i++) {
		v3 = (i * step) | sel_mask;
		CONSYS_REG_WRITE(addr3, v3);
		CONSYS_REG_WRITE(addr1, sel | probe_a);
		v_a = CONSYS_REG_READ(addr2);
		CONSYS_REG_WRITE(addr1, sel | probe_b);
		v_b = CONSYS_REG_READ(addr2);
		WMT_PLAT_PR_INFO("0x%x->addr3,0x%x->addr1,addr2:0x%x/0x%x->addr1,addr2:0x%x\n",
				v3, sel | probe_a, v_a, sel | probe_b, v_b);
	}
}

static VOID consys_hang_debug(VOID)
{
	SIZE_T addr1 = conn_reg.mcu_base + CONSYS_DBG_ADDR1_OFFSET;
	SIZE_T addr2 = conn_reg.mcu_base + CONSYS_DBG_ADDR2_OFFSET;
	SIZE_T addr3 = conn_reg.mcu_base + CONSYS_DBG_ADDR3_OFFSET;
	UINT32 v1 = CONSYS_REG_READ(addr1);
	UINT32 v3 = CONSYS_REG_READ(addr3);
	UINT32 sel = v1 & 0xff0000ff;
	UINT32 r1, r2;

	WMT_PLAT_PR_INFO("addr1:%zx:0x%x/addr2:%zx/addr3:%zx:0x%x\n", addr1, v1, addr2, addr3, v3);

	CONSYS_REG_WRITE(addr1, sel | 0x20100);
	r1 = CONSYS_REG_READ(addr2);
	CONSYS_REG_WRITE(addr1, sel | 0x40300);
	r2 = CONSYS_REG_READ(addr2);
	WMT_PLAT_PR_INFO("0x%x->addr1, addr2:0x%x/0x%x->addr1, addr2:0x%x\n",
			sel | 0x20100, r1, sel | 0x40300, r2);

	consys_hang_debug_probe(addr1, addr2, addr3, sel, v3 & 0xffffff0f, 0x10, 9,
			0x525100, 0x545300);
	WMT_PLAT_PR_INFO("apb0_dbg_prob\n");
	consys_hang_debug_probe(addr1, addr2, addr3, sel, v3 & 0xffff0fff, 0x1000, 10,
			0x565500, 0x585700);
	WMT_PLAT_PR_INFO("apb1_dbg_prob\n");
	consys_hang_debug_probe(addr1, addr2, addr3, sel, v3 & 0xff0fffff, 0x100000, 10,
			0x5a5900, 0x5c5b00);
}

/* stock 0x4198 */
static VOID consys_acr_reg_setting(VOID)
{
	CONSYS_SET_BIT(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET, CONSYS_MCU_CFG_ACR_MBIST_BIT);
}

/* stock 0x41c8 */
static VOID consys_afe_reg_setting(VOID)
{
#if CONSYS_AFE_REG_SETTING
	UINT8 i = 0;
	UINT8 *consys_afe_reg_base = NULL;

	/* update ANA_WBG(AFE) CR */
	consys_afe_reg_base = ioremap_nocache(CONSYS_AFE_REG_BASE, 0x100);
	if (consys_afe_reg_base) {
		CONSYS_REG_WRITE(consys_afe_reg_base + CONSYS_AFE_RG_WBG_PLL_03_OFFSET,
				CONSYS_AFE_RG_WBG_PLL_03_VALUE);
		CONSYS_REG_WRITE(consys_afe_reg_base + CONSYS_AFE_RG_WBG_GPS_02_OFFSET,
				CONSYS_AFE_RG_WBG_GPS_02_VALUE);

		WMT_PLAT_PR_DBG("Dump AFE register\n");
		for (i = 0; i < 64; i++) {
			WMT_PLAT_PR_DBG("reg:0x%08x|val:0x%08x\n",
					CONSYS_AFE_REG_BASE + 4*i,
					CONSYS_REG_READ(consys_afe_reg_base + 4*i));
		}
		iounmap(consys_afe_reg_base);
	} else
		WMT_PLAT_PR_ERR("AFE base(0x%x) ioremap fail!\n", CONSYS_AFE_REG_BASE);
#endif
}

/* stock 0x4394 */
static INT32 consys_hw_vcn18_ctrl(MTK_WCN_BOOL enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	if (enable) {
		/* VCN18 under SW control */
		KERNEL_upmu_set_reg_value(MT6358_LDO_VCN18_OP_EN, 1);
		if (reg_VCN18) {
			regulator_set_voltage(reg_VCN18, 1800000, 1800000);
			if (regulator_enable(reg_VCN18))
				WMT_PLAT_PR_ERR("enable VCN18 fail\n");
			else
				WMT_PLAT_PR_DBG("enable VCN18 ok\n");
		}

		/* VCN33 (BT/Wi-Fi PA) at 3.5 V, under SW control */
		KERNEL_upmu_set_reg_value(MT6358_LDO_VCN33_OP_EN, 1);
		if (reg_VCN33_BT) {
			regulator_set_voltage(reg_VCN33_BT, 3500000, 3500000);
			if (regulator_enable(reg_VCN33_BT))
				WMT_PLAT_PR_ERR("WMT do BT PMIC on fail!\n");
		}
	} else {
		if (reg_VCN33_BT)
			regulator_disable(reg_VCN33_BT);

		if (reg_VCN18) {
			if (regulator_disable(reg_VCN18))
				WMT_PLAT_PR_ERR("disable VCN_1V8 fail!\n");
			else
				WMT_PLAT_PR_DBG("disable VCN_1V8 ok\n");
		}
	}
#endif
	return 0;
}

/* stock 0x4598 */
static VOID consys_vcn28_hw_mode_ctrl(UINT32 enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	KERNEL_pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_EN, enable ? 1 : 0);
	KERNEL_pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_CFG, 0);
#endif
}

/* stock 0x45c4 */
static INT32 consys_hw_vcn28_ctrl(UINT32 enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	if (enable) {
		/*in co-clock mode,need to turn on vcn28 when fm on */
		if (reg_VCN28) {
			regulator_set_voltage(reg_VCN28, 2800000, 2800000);
			if (regulator_enable(reg_VCN28))
				WMT_PLAT_PR_ERR("WMT do VCN28 PMIC on fail!\n");
		}
		WMT_PLAT_PR_INFO("turn on vcn28 for fm/gps usage in co-clock mode\n");
	} else {
		/*in co-clock mode,need to turn off vcn28 when fm off */
		if (reg_VCN28)
			regulator_disable(reg_VCN28);
		WMT_PLAT_PR_INFO("turn off vcn28 for fm/gps usage in co-clock mode\n");
	}
#endif
	return 0;
}

/* stock 0x46f0: VCN33 is switched together with VCN18 */
static INT32 consys_hw_bt_vcn33_ctrl(UINT32 enable)
{
	return 0;
}

/* stock 0x46e8 */
static INT32 consys_hw_wifi_vcn33_ctrl(UINT32 enable)
{
	return 0;
}

/* stock 0x4700 */
static INT32 consys_emi_mpu_set_region_protection(VOID)
{
#ifdef CONFIG_MTK_EMI
	struct emi_region_info_t region_info;

	/*set MPU for EMI share Memory */
	WMT_PLAT_PR_INFO("setting MPU for EMI share memory\n");

	region_info.start = gConEmiPhyBase + CONSYS_EMI_MPU_OFFSET;
	region_info.end = gConEmiPhyBase + gConEmiSize - 1;
	region_info.region = CONSYS_EMI_MPU_REGION;
	/* per-domain permissions as in the stock driver (AP and CONN full access) */
	region_info.apc[0] = CONSYS_EMI_MPU_APC0;
	region_info.apc[1] = CONSYS_EMI_MPU_APC1;
	emi_mpu_set_protection(&region_info);
#endif
	return 0;
}

/* stock 0x47f4 */
static UINT32 consys_emi_set_remapping_reg(VOID)
{
	/*
	 * Not in the stock function, but the ROM patch code of this (newer)
	 * common driver reads these; every other platform file sets them here.
	 */
	mtk_wcn_emi_addr_info.emi_ap_phy_addr = gConEmiPhyBase;
	mtk_wcn_emi_addr_info.emi_size = gConEmiSize;

	/* consys to ap emi remapping register: 0x10001380, 2 MB granularity */
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET,
			CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET) |
			((gConEmiPhyBase >> 21) & 0x1fff) | CONSYS_EMI_MAPPING_ENABLE);
	WMT_PLAT_PR_INFO("CONSYS_EMI_MAPPING dump in restore cb(0x%08x)\n",
			CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET));

	return 0;
}

/* stock 0x4898 */
static INT32 bt_wifi_share_v33_spin_lock_init(VOID)
{
#if CONSYS_BT_WIFI_SHARE_V33
	gBtWifiV33.counter = 0;
	spin_lock_init(&gBtWifiV33.lock);
#endif
	return 0;
}

/* stock 0x4b28 */
static INT32 consys_read_irq_info_from_dts(struct platform_device *pdev, PINT32 irq_num, PUINT32 irq_flag)
{
	struct device_node *node;
	UINT32 irq_info[3] = {0, 0, 0};

	node = pdev->dev.of_node;
	if (node) {
		*irq_num = irq_of_parse_and_map(node, 0);
		if (of_property_read_u32_array(node, "interrupts", irq_info, ARRAY_SIZE(irq_info))) {
			WMT_PLAT_PR_ERR("get irq flags from DTS fail!!\n");
			return -1;
		}
		*irq_flag = irq_info[2];
		WMT_PLAT_PR_INFO("get irq id(%d) and irq trigger flag(%d) from DT\n", *irq_num,
				   *irq_flag);
	} else {
		WMT_PLAT_PR_ERR("[%s] can't find CONSYS compatible node\n", __func__);
		return -1;
	}

	return 0;
}

/* stock 0x4cbc: the MT6771 node has 4 reg entries */
static INT32 consys_read_reg_from_dts(struct platform_device *pdev)
{
	struct device_node *node = NULL;

	node = pdev->dev.of_node;
	if (node) {
		conn_reg.mcu_base = (SIZE_T) of_iomap(node, MCU_BASE_INDEX);
		WMT_PLAT_PR_DBG("Get mcu register base(0x%zx)\n", conn_reg.mcu_base);
		conn_reg.ap_rgu_base = (SIZE_T) of_iomap(node, TOP_RGU_BASE_INDEX);
		WMT_PLAT_PR_DBG("Get ap_rgu register base(0x%zx)\n", conn_reg.ap_rgu_base);
		conn_reg.topckgen_base = (SIZE_T) of_iomap(node, INFRACFG_AO_BASE_INDEX);
		WMT_PLAT_PR_DBG("Get topckgen register base(0x%zx)\n", conn_reg.topckgen_base);
		conn_reg.spm_base = (SIZE_T) of_iomap(node, SPM_BASE_INDEX);
		WMT_PLAT_PR_DBG("Get spm register base(0x%zx)\n", conn_reg.spm_base);
	} else {
		WMT_PLAT_PR_ERR("[%s] can't find CONSYS compatible node\n", __func__);
		return -1;
	}

	return 0;
}

/* stock 0x4ed8 */
static VOID force_trigger_assert_debug_pin(VOID)
{
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_AP2CONN_OSC_EN_OFFSET,
			CONSYS_REG_READ(conn_reg.topckgen_base +
				CONSYS_AP2CONN_OSC_EN_OFFSET) & ~CONSYS_AP2CONN_WAKEUP_BIT);
	WMT_PLAT_PR_INFO("enable:dump CONSYS_AP2CONN_OSC_EN_REG(0x%x)\n",
			CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_AP2CONN_OSC_EN_OFFSET));
	usleep_range(64, 96);
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_AP2CONN_OSC_EN_OFFSET,
			CONSYS_REG_READ(conn_reg.topckgen_base +
				CONSYS_AP2CONN_OSC_EN_OFFSET) | CONSYS_AP2CONN_WAKEUP_BIT);
	WMT_PLAT_PR_INFO("disable:dump CONSYS_AP2CONN_OSC_EN_REG(0x%x)\n",
			CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_AP2CONN_OSC_EN_OFFSET));
}

/* stock 0x46f8 */
static UINT32 consys_soc_chipid_get(VOID)
{
	return PLATFORM_SOC_CHIP;
}

/* stock 0x4fd8 */
static P_CONSYS_EMI_ADDR_INFO consys_soc_get_emi_phy_add(VOID)
{
	return &mtk_wcn_emi_addr_info;
}

P_WMT_CONSYS_IC_OPS mtk_wcn_get_consys_ic_ops(VOID)
{
	return &consys_ic_ops;
}

/* stock 0x4e8c */
static UINT32 consys_read_cpupcr(VOID)
{
	if (conn_reg.mcu_base == 0)
		return 0;

	if (mtk_consys_check_reg_readable_by_addr(conn_reg.mcu_base + CONSYS_CPUPCR_OFFSET) == 0)
		return 0;

	return CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_CPUPCR_OFFSET);
}

/* stock 0x513c */
static INT32 consys_emi_coredump_remapping(UINT8 __iomem **addr, UINT32 enable)
{
	if (enable) {
		*addr = ioremap_nocache(gConEmiPhyBase + CONSYS_EMI_COREDUMP_OFFSET, CONSYS_EMI_MEM_SIZE);
		if (*addr) {
			WMT_PLAT_PR_INFO("COREDUMP EMI mapping OK virtual(0x%p) physical(0x%x)\n",
					   *addr, (UINT32) gConEmiPhyBase + CONSYS_EMI_COREDUMP_OFFSET);
			memset_io(*addr, 0, CONSYS_EMI_MEM_SIZE);
		} else {
			WMT_PLAT_PR_ERR("EMI mapping fail\n");
			return -1;
		}
	} else {
		if (*addr) {
			iounmap(*addr);
			*addr = NULL;
		}
	}
	return 0;
}

/* stock 0x52a0 */
static INT32 consys_reset_emi_coredump(UINT8 __iomem *addr)
{
	if (!addr) {
		WMT_PLAT_PR_ERR("get virtual address fail\n");
		return -1;
	}
	WMT_PLAT_PR_INFO("Reset EMI(0xF0080000 ~ 0xF0080400) and (0xF0088400 ~ 0xF0090400)\n");
	/* reset 0xF0080000 ~ 0xF0080400 (1K) */
	memset_io(addr, 0, 0x400);
	/* reset 0xF0088400 ~ 0xF0090400 (32K) */
	memset_io(addr + CONSYS_EMI_PAGED_DUMP_OFFSET, 0, 0x8000);
	return 0;
}

/* stock 0x535c */
static INT32 consys_dump_osc_state(P_CONSYS_STATE state)
{
	return MTK_WCN_BOOL_TRUE;
}

/* stock 0x5364: 0x8436b5 */
static UINT64 consys_get_options(VOID)
{
	ULONG options = OPT_POWER_ON_DLM_TABLE |
			OPT_SET_MCUCLK_TABLE_3_4 |
			OPT_SET_WIFI_EXT_COMPONENT |
			OPT_WIFI_LTE_COEX |
			OPT_BT_TSSI_FROM_WIFI_CONFIG_NEW_OPID |
			OPT_INIT_COEX_AFTER_RF_CALIBRATION |
			OPT_COEX_CONFIG_ADJUST |
			OPT_SET_OSC_TYPE |
			OPT_SET_COREDUMP_LEVEL |
			OPT_WIFI_LTE_COEX_TABLE_2 |
			OPT_NORMAL_PATCH_DWN_2;
	return options;
}
