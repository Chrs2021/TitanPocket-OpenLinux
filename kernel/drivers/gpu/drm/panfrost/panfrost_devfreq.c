// SPDX-License-Identifier: GPL-2.0
/*
 * 4.14 backport: devfreq stub. The MT6771 device tree has no OPP table and
 * GPU frequency/voltage belong to MediaTek's gpufreq driver, so Panfrost runs
 * at the gpufreq boot OPP for now. Busy/idle accounting is kept so a
 * gpufreq-based governor can be added later.
 */
#include <linux/spinlock.h>

#include "panfrost_device.h"
#include "panfrost_devfreq.h"

int panfrost_devfreq_init(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	spin_lock_init(&pfdevfreq->lock);
	pfdevfreq->time_last_update = ktime_get();
	return 0;
}

void panfrost_devfreq_fini(struct panfrost_device *pfdev)
{
}

void panfrost_devfreq_resume(struct panfrost_device *pfdev)
{
}

void panfrost_devfreq_suspend(struct panfrost_device *pfdev)
{
}

void panfrost_devfreq_record_busy(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);
	pfdevfreq->busy_count++;
	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}

void panfrost_devfreq_record_idle(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);
	WARN_ON(--pfdevfreq->busy_count < 0);
	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}
