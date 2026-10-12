/* SPDX-License-Identifier: MIT */
/* 4.14 backport of the parts of drm_utils.h used by backported drivers */
#ifndef __DRM_UTILS_H__
#define __DRM_UTILS_H__

#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>

/* from drm_syncobj.c / drm_utils (Linux 5.x) */
static inline unsigned long drm_timeout_abs_to_jiffies(int64_t timeout_nsec)
{
	ktime_t abs_timeout, now;
	u64 timeout_ns, timeout_jiffies64;

	/* make 0 timeout means poll - absolute 0 doesn't seem valid */
	if (timeout_nsec == 0)
		return 0;

	abs_timeout = ns_to_ktime(timeout_nsec);
	now = ktime_get();

	if (!ktime_after(abs_timeout, now))
		return 0;

	timeout_ns = ktime_to_ns(ktime_sub(abs_timeout, now));

	timeout_jiffies64 = nsecs_to_jiffies64(timeout_ns);
	/*  clamp timeout to avoid infinite timeout */
	if (timeout_jiffies64 >= MAX_SCHEDULE_TIMEOUT - 1)
		return MAX_SCHEDULE_TIMEOUT - 1;

	return timeout_jiffies64 + 1;
}

#endif
