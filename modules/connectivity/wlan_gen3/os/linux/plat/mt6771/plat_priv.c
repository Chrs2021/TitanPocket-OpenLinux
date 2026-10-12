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
/*
 * MT6771 platform hooks of the gen3 Wi-Fi driver.
 *
 * MediaTek did not publish this file. It was reconstructed for the Titan
 * Pocket project from the stock wlan_drv_gen3.ko (.text 0x0-0x1d8, built
 * from os/linux/plat/mt6771/plat_priv.c); the Kbuild picks it up when present.
 */
#include <linux/pm_qos.h>
#include <linux/mutex.h>
#include <cpu_ctrl.h>
#include <mt_emi_api.h>

#include "gl_os.h"

/* EMI MPU region the Wi-Fi firmware memory (start of the CONNSYS EMI) uses */
#define WIFI_EMI_MPU_REGION	25
/*
 * Per-domain access, 3 bits per domain (0 = no protection, 5 = forbidden):
 * CONNSYS (domain 2) always has access, the AP (domain 0) only while the
 * protection is off. Same as the stock values.
 */
#define WIFI_EMI_APC0_OPEN	0x00b6da28
#define WIFI_EMI_APC0_LOCKED	(WIFI_EMI_APC0_OPEN | 0x5)
#define WIFI_EMI_APC1		0x00b6db6d

/* boost: CPU frequency floor on both clusters, EMI (DRAM) at its top OPP */
#define WIFI_BOOST_CPU_KHZ	2340000
#define WIFI_BOOST_CLUSTERS	2
#define WIFI_EMI_OPP_HIGHEST	0
#define WIFI_EMI_OPP_UNREQ	15

static DEFINE_MUTEX(boost_mutex);
static bool boost_emi_requested;
static struct pm_qos_request boost_emi_req;

INT_32 kalBoostCpu(UINT_32 core_num)
{
	struct ppm_limit_data freq[WIFI_BOOST_CLUSTERS];
	int i;

	for (i = 0; i < WIFI_BOOST_CLUSTERS; i++) {
		freq[i].min = core_num ? WIFI_BOOST_CPU_KHZ : -1;
		freq[i].max = -1;
	}
	update_userlimit_cpu_freq(CPU_KIR_WIFI, WIFI_BOOST_CLUSTERS, freq);

	mutex_lock(&boost_mutex);
	if (core_num) {
		if (!boost_emi_requested) {
			pm_qos_add_request(&boost_emi_req, PM_QOS_EMI_OPP, WIFI_EMI_OPP_HIGHEST);
			boost_emi_requested = true;
		}
		pm_qos_update_request(&boost_emi_req, WIFI_EMI_OPP_HIGHEST);
	} else if (boost_emi_requested) {
		pm_qos_update_request(&boost_emi_req, WIFI_EMI_OPP_UNREQ);
		pm_qos_remove_request(&boost_emi_req);
		boost_emi_requested = false;
	}
	mutex_unlock(&boost_mutex);

	return 0;
}

VOID kalSetEmiMpuProtection(phys_addr_t emiPhyBase, UINT_32 size, BOOLEAN enable)
{
	struct emi_region_info_t region_info;

	region_info.start = emiPhyBase;
	region_info.end = emiPhyBase + size - 1;
	region_info.region = WIFI_EMI_MPU_REGION;
	region_info.apc[0] = enable ? WIFI_EMI_APC0_LOCKED : WIFI_EMI_APC0_OPEN;
	region_info.apc[1] = WIFI_EMI_APC1;
	emi_mpu_set_protection(&region_info);
}
