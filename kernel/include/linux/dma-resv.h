/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 4.14 backport: struct dma_resv was called struct reservation_object until
 * Linux 5.4. Map the names used by backported code.
 */
#ifndef _LINUX_DMA_RESV_COMPAT_H
#define _LINUX_DMA_RESV_COMPAT_H

#include <linux/reservation.h>

#define dma_resv			reservation_object
#define dma_resv_init			reservation_object_init
#define dma_resv_fini			reservation_object_fini
#define dma_resv_lock			reservation_object_lock
#define dma_resv_unlock			reservation_object_unlock
#define dma_resv_held			reservation_object_held
#define dma_resv_assert_held		reservation_object_assert_held
#define dma_resv_add_excl_fence		reservation_object_add_excl_fence
#define dma_resv_add_shared_fence	reservation_object_add_shared_fence
#define dma_resv_reserve_shared(obj, n)	reservation_object_reserve_shared(obj)
#define dma_resv_get_excl		reservation_object_get_excl
#define dma_resv_get_excl_rcu		reservation_object_get_excl_rcu
#define dma_resv_get_fences_rcu		reservation_object_get_fences_rcu
#define dma_resv_wait_timeout_rcu	reservation_object_wait_timeout_rcu
#define dma_resv_test_signaled_rcu	reservation_object_test_signaled_rcu

#endif
