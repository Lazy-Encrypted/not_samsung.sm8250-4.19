/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ZRAM_DRV_INTERNAL_H_
#define _ZRAM_DRV_INTERNAL_H_

#include "zram_drv.h"

#define dev_to_zram(dev) ((struct zram *)dev_to_disk(dev)->private_data)
#define init_done(zram) ((zram)->disksize)
#define zram_test_flag(zram, index, flag) \
	((zram)->table[index].flags & BIT(flag))
#define zram_set_flag(zram, index, flag) \
	((zram)->table[index].flags |= BIT(flag))
#define zram_clear_flag(zram, index, flag) \
	((zram)->table[index].flags &= ~BIT(flag))
#define zram_get_handle(zram, index) ((zram)->table[index].handle)
#define zram_set_handle(zram, index, value) \
	((zram)->table[index].handle = (value))
#define zram_get_obj_size(zram, index) \
	((zram)->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1))
#define zram_set_obj_size(zram, index, value) do { \
	unsigned long __flags = (zram)->table[index].flags >> ZRAM_FLAG_SHIFT; \
	(zram)->table[index].flags = (__flags << ZRAM_FLAG_SHIFT) | (value); \
} while (0)

#ifdef CONFIG_HYBRIDSWAP_ASYNC_COMPRESS
extern int async_compress_page(struct zram *zram, struct page *page);
extern void update_zram_index(struct zram *zram, u32 index,
		unsigned long page);
#endif

#endif /* _ZRAM_DRV_INTERNAL_H_ */
