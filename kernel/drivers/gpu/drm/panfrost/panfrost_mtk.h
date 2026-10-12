/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PANFROST_MTK_H__
#define __PANFROST_MTK_H__

struct panfrost_device;

int panfrost_mtk_init(struct panfrost_device *pfdev);
void panfrost_mtk_fini(struct panfrost_device *pfdev);
void panfrost_mtk_power_on(struct panfrost_device *pfdev);
void panfrost_mtk_power_off(struct panfrost_device *pfdev);
void panfrost_mtk_suspend(struct panfrost_device *pfdev);
void panfrost_mtk_quirks(struct panfrost_device *pfdev);

#endif
