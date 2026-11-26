// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#include <linux/sched/rt.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/vmstat.h>
#include <linux/mmzone.h>
#include "dynamic_readhead.h"

int dynamic_readahead_enable = 1;
EXPORT_SYMBOL(dynamic_readahead_enable);

static bool is_key_task(struct task_struct *tsk)
{
	return (
#ifdef CONFIG_OPLUS_FEATURE_SCHED_ASSIST
		test_task_ux(tsk) ||
		(sysctl_sched_assist_scene & SA_LAUNCH) ||
#endif
		rt_task(tsk));
}

bool is_lowmem(void)
{
	struct zone *zone = NULL;

	for_each_zone(zone) {
		if (!managed_zone(zone))
			continue;
		if (zone_watermark_ok(zone, 0, low_wmark_pages(zone), 0, 0))
			return false;
	}
	return true;
}
EXPORT_SYMBOL(is_lowmem);

void adjust_readaround(struct file_ra_state *ra, pgoff_t offset)
{
	unsigned int ra_pages = ra->ra_pages;

	if (is_key_task(current)) {
		ra->start = max_t(long, 0, offset - ra->ra_pages / 2);
		ra->size = ra->ra_pages;
		ra->async_size = ra->ra_pages / 4;
		return;
	}

	if (is_lowmem())
		ra_pages = ra->ra_pages / 2;

	ra->start = max_t(long, 0, offset - ra_pages / 2);
	ra->size = ra_pages;
	ra->async_size = ra_pages >> 2;
}
EXPORT_SYMBOL(adjust_readaround);

unsigned long adjust_readahead(struct file_ra_state *ra, unsigned long max_pages)
{
	if (is_key_task(current))
		return max_pages;

	if (is_lowmem())
		max_pages = min_t(long, max_pages, ra->ra_pages / 2);

	return max_pages;
}
EXPORT_SYMBOL(adjust_readahead);
