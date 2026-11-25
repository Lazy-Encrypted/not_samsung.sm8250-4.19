/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _UX_PAGE_POOL_H
#define _UX_PAGE_POOL_H

#include <linux/kref.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/sizes.h>

#define UXMEM_POOL_MAX_PAGES 2
#define HIGHATOMIC_MIN_RESERVED_PAGES (SZ_16M >> PAGE_SHIFT)

enum UX_POOL_MIGRATETYPE {
	UX_POOL_MIGRATETYPE_UNMOVABLE,
	UX_POOL_MIGRATETYPE_MOVABLE,
	UX_POOL_MIGRATETYPE_TYPES_SIZE
};

struct ux_page_pool {
	int low[UX_POOL_MIGRATETYPE_TYPES_SIZE];
	int high[UX_POOL_MIGRATETYPE_TYPES_SIZE];
	int count[UX_POOL_MIGRATETYPE_TYPES_SIZE];
	struct list_head items[UX_POOL_MIGRATETYPE_TYPES_SIZE];
	spinlock_t lock;
	unsigned int order;
	gfp_t gfp_mask;
};

#ifdef CONFIG_OPLUS_UXMEM_OPT
extern int ux_page_pool_enable;
void ux_page_pool_free_prepared(struct page *page, unsigned int order);
struct page *ux_page_pool_alloc_pages(unsigned int order, int migratetype, bool may_retry);
int ux_page_pool_refill(struct page *page, unsigned int order, int migratetype);
bool is_critical_zeroslowpath_task(struct task_struct *tsk);
bool free_unref_page_prepare2(struct page *page, unsigned int order, unsigned long pfn);
#else
static inline void ux_page_pool_free_prepared(struct page *page, unsigned int order) { }
static inline struct page *ux_page_pool_alloc_pages(unsigned int order, int migratetype, bool may_retry) { return NULL; }
static inline int ux_page_pool_refill(struct page *page, unsigned int order, int migratetype) { return 0; }
static inline bool is_critical_zeroslowpath_task(struct task_struct *tsk) { return false; }
static inline bool free_unref_page_prepare2(struct page *page, unsigned int order, unsigned long pfn) { return false; }
#endif

#endif /* _UX_PAGE_POOL_H */
