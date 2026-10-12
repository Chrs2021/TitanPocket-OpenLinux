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
 * \brief  MT6771 (Helio P60/P70) CONSYS platform definitions
 *
 * MediaTek did not publish this file. It was reconstructed for the Titan
 * Pocket project from the stock wmt_drv.ko (Unihertz Android 11 vendor
 * image, built from alps .../common_main/platform/mt6771.c), using mt6765.h
 * as the template. Every value below was read from the stock binary; see
 * docs/drivers/connectivity.md in the project.
 */

#ifndef _MTK_MT6771_H_
#define _MTK_MT6771_H_

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

#define CONSYS_BT_WIFI_SHARE_V33	0
#define CONSYS_PMIC_CTRL_ENABLE		1
#define CONSYS_AHB_CLK_MAGEMENT		1
#define CONSYS_USE_PLATFORM_WRITE	1
#define CONSYS_PWR_ON_OFF_API_AVAILABLE	1
#define CONSYS_AFE_REG_SETTING		1

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

#define PLATFORM_SOC_CHIP 0x6771

/* value of mcu_base + CONSYS_CHIP_ID_OFFSET once CONNSYS is powered */
#define CONSYS_CHIP_ID_OFFSET		0x00000008
#define CONSYS_CHIP_ID			0x0788

/* A-Die interface pinmux base */
#define CONSYS_IF_PINMUX_REG_BASE	0x10005000
#define CONSYS_IF_PINMUX_01_OFFSET	0x000003F0
#define CONSYS_IF_PINMUX_01_MASK	0xFFFFFF00
#define CONSYS_IF_PINMUX_01_VALUE	0x00000011
#define CONSYS_IF_PINMUX_02_OFFSET	0x000003E0
#define CONSYS_IF_PINMUX_02_MASK	0x0000000F
#define CONSYS_IF_PINMUX_02_VALUE	0x11111110

/* infracfg_ao ("topckgen" in conn_reg) */
#define CONSYS_AP2CONN_OSC_EN_OFFSET	0x00000f00
#define CONSYS_EMI_MAPPING_OFFSET	0x00000380
#define CONSYS_EMI_MAPPING_ENABLE	(0x1 << 13)
/* ap_rgu */
#define CONSYS_CPU_SW_RST_OFFSET	0x00000018
/* spm (only dumped on a failed power on) */
#define CONSYS_TOP1_PWR_CTRL_OFFSET	0x0000032C
#define CONSYS_PWR_CONN_ACK_OFFSET	0x00000180
#define CONSYS_PWR_CONN_ACK_S_OFFSET	0x00000184
/* mcu */
#define CONSYS_MCU_CFG_ACR_OFFSET	0x00000140
#define CONSYS_CPUPCR_OFFSET		0x00000160
#define CONSYS_MCU_ROM_DELSEL_OFFSET	0x00000600
#define CONSYS_MCU_ROM_DELSEL_VALUE	0x001702A4
#define CONSYS_DBG_ADDR1_OFFSET		0x00000400
#define CONSYS_DBG_ADDR2_OFFSET		0x00000404
#define CONSYS_DBG_ADDR3_OFFSET		0x0000040C

#define CONSYS_AFE_REG_BASE			(0x180B6000)
#define CONSYS_AFE_RG_WBG_PLL_03_OFFSET		(0x00000038)
#define CONSYS_AFE_RG_WBG_PLL_03_VALUE		(0x000C1DF0)
#define CONSYS_AFE_RG_WBG_GPS_02_OFFSET		(0x00000054)
#define CONSYS_AFE_RG_WBG_GPS_02_VALUE		(0x110A2000)

/*CONSYS_CPU_SW_RST_REG*/
#define CONSYS_CPU_SW_RST_BIT		(0x1 << 12)
#define CONSYS_CPU_SW_RST_CTRL_KEY	(0x88 << 24)

/*CONSYS_MCU_CFG_ACR_REG*/
#define CONSYS_MCU_CFG_ACR_MBIST_BIT	(0x1 << 0 | 0x1 << 1)

/*control app2cnn_osc_en*/
#define CONSYS_AP2CONN_WAKEUP_BIT	(0x1 << 9)

/* EMI part mapping & ctrl */
#define CONSYS_EMI_FW_PHY_BASE		(0xf0080000)
#define CONSYS_EMI_COREDUMP_OFFSET	(0x80000)
/* CONSYS_EMI_MEM_SIZE (96 KB = 0x18000, as stock) is in mtk_wcn_consys_hw.h */
#define CONSYS_EMI_PAGED_TRACE_OFFSET	(0x400)
#define CONSYS_EMI_PAGED_DUMP_OFFSET	(0x8400)
#define CONSYS_EMI_FULL_DUMP_OFFSET	(0x10400)
/* EMI MPU: region 26, from this offset to the end of the CONNSYS memory */
#define CONSYS_EMI_MPU_REGION		26
#define CONSYS_EMI_MPU_OFFSET		(0x70000)
#define CONSYS_EMI_MPU_APC0		0x80b6da28
#define CONSYS_EMI_MPU_APC1		0x00b6db6d

/* EMI_CTRL_STATE_OFFSET: the EXP_APMEM_* enum in mtk_wcn_consys_hw.h
 * matches the stock values exactly.
 */

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

#ifdef CONSYS_BT_WIFI_SHARE_V33
struct bt_wifi_v33_status {
	UINT32 counter;
	UINT32 flags;
	spinlock_t lock;
};

extern struct bt_wifi_v33_status gBtWifiV33;
#endif

#endif /* _MTK_MT6771_H_ */
