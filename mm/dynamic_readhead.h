/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#ifndef _DYNAMIC_READHEAD_H
#define _DYNAMIC_READHEAD_H

#include <linux/fs.h>
#include <linux/sched.h>

#ifdef CONFIG_OPLUS_FEATURE_SCHED_ASSIST
#include <linux/sched_assist.h>
#endif

extern int dynamic_readahead_enable;

void adjust_readaround(struct file_ra_state *ra, pgoff_t offset);
unsigned long adjust_readahead(struct file_ra_state *ra, unsigned long max_pages);
bool is_lowmem(void);

#endif /* _DYNAMIC_READHEAD_H */
