// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#include <asm/page.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/cpufreq.h>
#include <linux/sched/rt.h>
#include <linux/sched_assist.h>
#include <linux/sysctl.h>
#include <linux/shrinker.h>
#include <linux/swap.h>

#include "ux_page_pool.h"

#define UX_PAGE_POOL_NAME "ux_page_pool_fillthread"
#define MAX_POOL_ALLOC_RETRIES (5)
#define K(x) ((x) << (PAGE_SHIFT - 10))

static const unsigned int orders[] = {0, 1};
/* 32M for order 0, 8M for order 1 default */
static const unsigned int page_pool_nr_blocks[] = {
	SZ_32M >> PAGE_SHIFT,
	SZ_8M >> (PAGE_SHIFT + 1),
};
#define NUM_ORDERS ARRAY_SIZE(orders)

static struct ux_page_pool *pools[NUM_ORDERS];
static struct task_struct *ux_page_pool_tsk = NULL;
static wait_queue_head_t kworkthread_waitq;
static atomic_t kworkthread_wait_flag = ATOMIC_INIT(0);
static bool ux_page_pool_enabled = false;
static bool fillthread_enabled = false;
static bool free_to_pool = false;
static atomic_long_t ux_pool_alloc_fail = ATOMIC_LONG_INIT(0);

#define PARA_BUF_LEN 512

static int page_pool_fill(struct ux_page_pool *pool, int migratetype);
static bool page_pool_needs_fill(struct ux_page_pool *pool, int migratetype);
static unsigned long ux_page_pool_count_objects(struct shrinker *shrinker,
						struct shrink_control *sc);
static unsigned long ux_page_pool_scan_objects(struct shrinker *shrinker,
						struct shrink_control *sc);

static struct shrinker ux_page_pool_shrinker = {
	.count_objects = ux_page_pool_count_objects,
	.scan_objects = ux_page_pool_scan_objects,
	.seeks = DEFAULT_SEEKS,
};

int ux_page_pool_enable = 1;

static bool get_critical_zeroslowpath_task_flag(struct task_struct *tsk)
{
	if (!tsk)
		return false;
#ifdef CONFIG_OPLUS_FEATURE_SCHED_ASSIST
	if ((tsk->ux_im_flag == IM_FLAG_SYSTEMSERVER_PID) ||
		(tsk->ux_im_flag == IM_FLAG_SURFACEFLINGER))
		return true;
#endif
	return false;
}

bool is_critical_zeroslowpath_task(struct task_struct *tsk)
{
	if (!tsk)
		tsk = current;
	if (unlikely(test_task_ux(tsk) || rt_task(tsk) ||
			get_critical_zeroslowpath_task_flag(tsk)))
		return true;
	else
		return false;
}
EXPORT_SYMBOL(is_critical_zeroslowpath_task);

static int order_to_index(unsigned int order)
{
	int i;
	for (i = 0; i < NUM_ORDERS; i++) {
		if (order == orders[i])
			return i;
	}
	return -1;
}

static void page_pool_wakeup_process(struct ux_page_pool *pool)
{
	if (unlikely(!READ_ONCE(ux_page_pool_enabled)))
		return;

	if (NULL == pool) {
		pr_err("%s: boost_pool is NULL!\n", __func__);
		return;
	}

	if (READ_ONCE(fillthread_enabled)) {
		atomic_set(&kworkthread_wait_flag, 1);
		wake_up_interruptible(&kworkthread_waitq);
	}
}

static bool set_ux_page_pool_fillthread_cpus(void)
{
	struct cpumask mask, max_cpus;
	pg_data_t *pgdat = NODE_DATA(0);
	unsigned int cpu, cpufreq_max_tmp = 0;
	bool found = false;

	cpumask_clear(&max_cpus);
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (policy->cpuinfo.max_freq > cpufreq_max_tmp) {
			cpufreq_max_tmp = policy->cpuinfo.max_freq;
			cpumask_copy(&max_cpus, policy->related_cpus);
			found = true;
		} else if (found && policy->cpuinfo.max_freq == cpufreq_max_tmp) {
			cpumask_or(&max_cpus, &max_cpus, policy->related_cpus);
		}
		cpufreq_cpu_put(policy);
	}

	if (!found)
		return false;

	cpumask_copy(&mask, cpumask_of_node(pgdat->node_id));
	cpumask_andnot(&mask, &mask, &max_cpus);
	if (!cpumask_empty(&mask))
		set_cpus_allowed_ptr(current, &mask);

	return true;
}

static int ux_page_pool_fillthread(void *p)
{
	struct ux_page_pool *pool;
	bool affinity_set = false;
	int i, j;
	int ret;

	if (unlikely(!READ_ONCE(ux_page_pool_enabled)))
		return -EINVAL;

	set_user_nice(current, 5);

	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(kworkthread_waitq,
						       atomic_read(&kworkthread_wait_flag) ||
						       kthread_should_stop());
		if (kthread_should_stop())
			break;
		if (ret < 0)
			continue;

		atomic_set(&kworkthread_wait_flag, 0);
		if (unlikely(!affinity_set))
			affinity_set = set_ux_page_pool_fillthread_cpus();

		for (i = 0; i < NUM_ORDERS; i++) {
			pool = pools[i];
			if (!pool)
				continue;
			for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++) {
				while (READ_ONCE(fillthread_enabled) &&
				       page_pool_needs_fill(pool, j)) {
					ret = page_pool_fill(pool, j);
					if (ret < 0)
						cond_resched();
					if (ret <= 0)
						break;
				}
			}
		}
	}
	return 0;
}

struct ux_page_pool *ux_page_pool_create(gfp_t gfp_mask, unsigned int order, unsigned int nr_blocks)
{
	struct ux_page_pool *pool;
	int i;

	pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->gfp_mask = gfp_mask;
	pool->order = order;
	for (i = 0; i < UX_POOL_MIGRATETYPE_TYPES_SIZE; i++) {
		pool->count[i] = 0;
		/* MIGRATETYPE: UNMOVABLE & MOVABLE */
		pool->high[i] = nr_blocks / UX_POOL_MIGRATETYPE_TYPES_SIZE;
		/* wakeup kthread on count < low, low = high/2 */
		pool->low[i]  = pool->high[i] / 2;
		INIT_LIST_HEAD(&pool->items[i]);

		pr_info("%s order:%d migratetype:%d low: %d high: %d count:%d.\n",
			__func__, pool->order, i, pool->low[i], pool->high[i], pool->count[i]);
	}

	spin_lock_init(&pool->lock);
	return pool;
}

static bool page_pool_try_add(struct ux_page_pool *pool, struct page *page,
			      int migratetype)
{
	unsigned long flags;
	bool added = false;

	spin_lock_irqsave(&pool->lock, flags);
	if (pool->count[migratetype] < pool->high[migratetype]) {
		list_add_tail(&page->lru, &pool->items[migratetype]);
		pool->count[migratetype]++;
		added = true;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	return added;
}

static struct page *page_pool_remove(struct ux_page_pool *pool, int migratetype,
				     bool may_drain_below_low)
{
	struct page *page;
	unsigned long flags;
	bool wakeup;

	spin_lock_irqsave(&pool->lock, flags);
	page = NULL;
	if (pool->count[migratetype] &&
	    (may_drain_below_low || pool->count[migratetype] > pool->low[migratetype]))
		page = list_first_entry(&pool->items[migratetype], struct page, lru);
	if (page) {
		pool->count[migratetype]--;
		list_del(&page->lru);
	}
	wakeup = pool->count[migratetype] < pool->low[migratetype];
	spin_unlock_irqrestore(&pool->lock, flags);

	/* wakeup kthread on count < low */
	if (wakeup)
		page_pool_wakeup_process(pool);

	return page;
}

static bool page_pool_needs_fill(struct ux_page_pool *pool, int migratetype)
{
	unsigned long flags;
	bool needs_fill;

	spin_lock_irqsave(&pool->lock, flags);
	needs_fill = pool->count[migratetype] < pool->high[migratetype];
	spin_unlock_irqrestore(&pool->lock, flags);
	return needs_fill;
}

static unsigned long ux_page_pool_cached_pages(void)
{
	unsigned long count = 0;
	unsigned long flags;
	int i, j;

	for (i = 0; i < NUM_ORDERS; i++) {
		struct ux_page_pool *pool = READ_ONCE(pools[i]);

		if (!pool)
			continue;
		spin_lock_irqsave(&pool->lock, flags);
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++)
			count += (unsigned long)pool->count[j] << pool->order;
		spin_unlock_irqrestore(&pool->lock, flags);
	}

	return count;
}

static unsigned long ux_page_pool_target_pages(void)
{
	unsigned long count = 0;
	unsigned long flags;
	int i, j;

	for (i = 0; i < NUM_ORDERS; i++) {
		struct ux_page_pool *pool = READ_ONCE(pools[i]);

		if (!pool)
			continue;
		spin_lock_irqsave(&pool->lock, flags);
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++)
			count += (unsigned long)pool->high[j] << pool->order;
		spin_unlock_irqrestore(&pool->lock, flags);
	}

	return count;
}

/* Preserve the high watermark and the remaining pool target during refill. */
static bool page_pool_refill_allowed(struct ux_page_pool *pool)
{
	struct zone *zone;
	unsigned long target_pages;
	unsigned long remaining_pages;
	unsigned long current_pages;
	unsigned long block_pages = 1UL << pool->order;
	unsigned long mark;
	int high_zoneidx = gfp_zone(pool->gfp_mask);

	target_pages = ux_page_pool_target_pages();
	current_pages = ux_page_pool_cached_pages();
	remaining_pages = current_pages < target_pages ?
		target_pages - current_pages : 0;
	if (remaining_pages > block_pages)
		remaining_pages -= block_pages;
	else
		remaining_pages = 0;

	for_each_populated_zone(zone) {
		if (zone_idx(zone) > high_zoneidx)
			continue;
		mark = zone->_watermark[WMARK_HIGH] + remaining_pages;
		if (zone_watermark_ok(zone, 0, mark, high_zoneidx, 0))
			return true;
	}

	return false;
}

static int page_pool_fill(struct ux_page_pool *pool, int migratetype)
{
	struct page *page;
	gfp_t gfp_refill;
	unsigned long pfn;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return -EINVAL;
	}
	if (!page_pool_refill_allowed(pool))
		return -EAGAIN;

	gfp_refill = pool->gfp_mask;
	page = alloc_pages(gfp_refill, pool->order);
	if (NULL == page)
		return -ENOMEM;
	if (WARN_ON_ONCE(!put_page_testzero(page))) {
		pr_err("ux_page_pool: fresh page has unexpected refcount\n");
		return -EINVAL;
	}
	pfn = page_to_pfn(page);
	if (!free_unref_page_prepare2(page, pool->order, pfn)) {
		/* The page failed allocator validation; do not decrement it again. */
		pr_err("ux_page_pool: free_unref_page_prepare2 failed\n");
		return -EINVAL;
	}

	if (!page_pool_try_add(pool, page, migratetype)) {
		ux_page_pool_free_prepared(page, pool->order);
		return 0;
	}

	return true;
}

static bool ux_page_pool_under_low_wmark(void)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (zone_idx(zone) != ZONE_NORMAL)
			continue;
		if (zone_watermark_ok(zone, 0, low_wmark_pages(zone), ZONE_NORMAL, 0))
			return false;
	}
	return true;
}

static unsigned long ux_page_pool_count_objects(struct shrinker *shrinker,
						struct shrink_control *sc)
{
	if (unlikely(!READ_ONCE(ux_page_pool_enabled)))
		return 0;

#ifdef CONFIG_OPLUS_FEATURE_SCHED_ASSIST
	if (!current_is_kswapd() && test_task_ux(current))
		return 0;
#endif

	if (!ux_page_pool_under_low_wmark())
		return 0;

	return ux_page_pool_cached_pages();
}

static unsigned long ux_page_pool_scan_objects(struct shrinker *shrinker,
						struct shrink_control *sc)
{
	unsigned long freed = 0;
	int i, j;

	for (i = 0; i < NUM_ORDERS && freed < sc->nr_to_scan; i++) {
		struct ux_page_pool *pool = READ_ONCE(pools[i]);

		if (!pool)
			continue;
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE &&
		     freed < sc->nr_to_scan; j++) {
			while (freed < sc->nr_to_scan) {
				struct page *page;
				unsigned long flags;

				spin_lock_irqsave(&pool->lock, flags);
				page = list_first_entry_or_null(&pool->items[j],
								struct page, lru);
				if (page) {
					list_del(&page->lru);
					pool->count[j]--;
				}
				spin_unlock_irqrestore(&pool->lock, flags);

				if (!page)
					break;

				ux_page_pool_free_prepared(page, pool->order);
				freed += 1UL << pool->order;
			}
		}
	}

	sc->nr_scanned = min(freed, sc->nr_to_scan);
	return freed ? freed : SHRINK_STOP;
}

/* fast path */
struct page *ux_page_pool_alloc_pages(unsigned int order, int migratetype, bool may_retry)
{
	struct page *page = NULL;
	int retries = 0;
	struct ux_page_pool *pool = NULL;
	int order_ind = order_to_index(order);

	if (unlikely(!READ_ONCE(ux_page_pool_enabled)) || order_ind == -1 ||
		migratetype < 0 || migratetype >= UX_POOL_MIGRATETYPE_TYPES_SIZE)
		return NULL;

	pool = READ_ONCE(pools[order_ind]);
	if (pool == NULL)
		return NULL;

retry:
	/* Fast-path: Get a page from cache */
	page = page_pool_remove(pool, migratetype,
				is_critical_zeroslowpath_task(current));
	if (!page && may_retry && retries < MAX_POOL_ALLOC_RETRIES) {
		retries++;
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(2));
		goto retry;
	}

	if (!page)
		atomic_long_inc(&ux_pool_alloc_fail);

	return page;
}
EXPORT_SYMBOL(ux_page_pool_alloc_pages);

int ux_page_pool_refill(struct page *page, unsigned int order, int migratetype)
{
	struct ux_page_pool *pool;
	int order_ind = order_to_index(order);

	if (unlikely(!READ_ONCE(ux_page_pool_enabled)) || !READ_ONCE(free_to_pool) ||
		order_ind == -1 || migratetype < 0 ||
		migratetype >= UX_POOL_MIGRATETYPE_TYPES_SIZE)
		return false;

	pool = READ_ONCE(pools[order_ind]);
	if (pool == NULL)
		return false;
	if (READ_ONCE(pool->count[migratetype]) >= READ_ONCE(pool->high[migratetype]))
		return false;
	if (!page_pool_refill_allowed(pool))
		return false;

	return page_pool_try_add(pool, page, migratetype);
}
EXPORT_SYMBOL(ux_page_pool_refill);

static ssize_t ux_page_pool_write(struct file *file,
		const char __user *buff, size_t len, loff_t *ppos)
{
	char kbuf[PARA_BUF_LEN] = {'\0'};
	char *str;
	int high_0, high_1;
	int i, ret;
	struct ux_page_pool *pool;
	unsigned long flags;

	if (len > PARA_BUF_LEN - 1) {
		pr_err("len %zu is too long\n", len);
		return -EINVAL;
	}

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str) {
		pr_err("buff %s is invalid\n", kbuf);
		return -EINVAL;
	}

	ret = sscanf(str, "%d %d", &high_0, &high_1);

	if (ret == 2) {
		if (high_0 < 0 || high_1 < 0 ||
		    (unsigned int)high_0 >
				(page_pool_nr_blocks[0] << orders[0]) ||
		    (unsigned int)high_1 >
				(page_pool_nr_blocks[1] << orders[1]))
			return -EINVAL;
		if (!pools[0] || !pools[1])
			return -ENODEV;

		for (i = 0; i < UX_POOL_MIGRATETYPE_TYPES_SIZE; i++) {
			pool = pools[0];
			spin_lock_irqsave(&pool->lock, flags);
			/* MIGRATETYPE: UNMOVABLE & MOVABLE */
			pool->high[i] = high_0 /
				(UX_POOL_MIGRATETYPE_TYPES_SIZE << pool->order);
			pool->low[i]  = pool->high[i] / 2;
			spin_unlock_irqrestore(&pool->lock, flags);
			pr_info("%s order:%d migratetype:%d low: %d high: %d count:%d.\n",
				__func__, pool->order, i,
				pool->low[i], pool->high[i], pool->count[i]);

			pool = pools[1];
			spin_lock_irqsave(&pool->lock, flags);
			/* MIGRATETYPE: UNMOVABLE & MOVABLE */
			pool->high[i] = high_1 /
				(UX_POOL_MIGRATETYPE_TYPES_SIZE << pool->order);
			pool->low[i]  = pool->high[i] / 2;
			spin_unlock_irqrestore(&pool->lock, flags);
			pr_info("%s order:%d migratetype:%d low: %d high: %d count:%d.\n",
				__func__, pool->order, i,
				pool->low[i], pool->high[i], pool->count[i]);
		}
		page_pool_wakeup_process(pools[0]);
		return len;
	}

	if (strstr(str, "fillthread_pause")) {
		WRITE_ONCE(fillthread_enabled, false);
		return len;
	}

	if (strstr(str, "fillthread_resume")) {
		WRITE_ONCE(fillthread_enabled, true);
		page_pool_wakeup_process(pools[0]);
		return len;
	}

	if (strstr(str, "free_to_pool=0")) {
		WRITE_ONCE(free_to_pool, false);
		return len;
	}

	if (strstr(str, "free_to_pool=1")) {
		WRITE_ONCE(free_to_pool, true);
		return len;
	}

	return -EINVAL;
}

static ssize_t ux_page_pool_read(struct file *file,
		char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[PARA_BUF_LEN] = {'\0'};
	int len = 0;
	int i, j;
	struct ux_page_pool *pool;

	for (i = 0; i < NUM_ORDERS; i++) {
		pool = READ_ONCE(pools[i]);
		if (!pool)
			continue;
		for (j = 0; j < UX_POOL_MIGRATETYPE_TYPES_SIZE; j++) {
			unsigned long flags;
			int low, high, pool_count;

			spin_lock_irqsave(&pool->lock, flags);
			low = pool->low[j];
			high = pool->high[j];
			pool_count = pool->count[j];
			spin_unlock_irqrestore(&pool->lock, flags);
			len += snprintf(kbuf + len, PARA_BUF_LEN - len,
					"order:%d migratetype:%d low: %d high: %d count:%d.\n",
					pool->order, j, low, high, pool_count);
		}
	}

	len += snprintf(kbuf + len, PARA_BUF_LEN - len,
			"page_pool alloc fail count:%ld\n",
			atomic_long_read(&ux_pool_alloc_fail));
	len += snprintf(kbuf + len, PARA_BUF_LEN - len,
			"page_pool fillthread status:%s\n",
			READ_ONCE(fillthread_enabled) ? "running" : "not running");
	len += snprintf(kbuf + len, PARA_BUF_LEN - len,
			"page_pool free_to_pool:%s\n",
			READ_ONCE(free_to_pool) ? "enabled" : "disabled");

	if (len == PARA_BUF_LEN)
		kbuf[len - 1] = '\0';

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count)))
		return -EFAULT;

	*off += (len < count ? len : count);
	return (len < count ? len : count);
}

static const struct file_operations ux_page_pool_fops = {
	.write = ux_page_pool_write,
	.read = ux_page_pool_read,
};

static int ux_page_pool_init(void)
{
	int i, ret;
	static bool inited = false;
	if (inited) {
		pr_info("uxmem_opt already inited\n");
		return 0;
	}

	if (!ux_page_pool_enable) {
		pr_err("uxmem_opt is disabled\n");
		return -EINVAL;
	}

	for (i = 0; i < NUM_ORDERS; i++) {
		pools[i] = ux_page_pool_create((GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN |
			   __GFP_NORETRY) & ~__GFP_RECLAIM, orders[i], page_pool_nr_blocks[i]);
		if (IS_ERR_OR_NULL(pools[i])) {
			ret = pools[i] ? PTR_ERR(pools[i]) : -ENOMEM;
			pools[i] = NULL;
			goto err_free_pools;
		}
	}

	init_waitqueue_head(&kworkthread_waitq);
	ret = register_shrinker(&ux_page_pool_shrinker);
	if (ret) {
		pr_err("%s: register shrinker failed: %d\n", __func__, ret);
		goto err_free_pools;
	}

	WRITE_ONCE(ux_page_pool_enabled, true);
	ux_page_pool_tsk = kthread_run(ux_page_pool_fillthread, NULL, UX_PAGE_POOL_NAME);
	if (IS_ERR_OR_NULL(ux_page_pool_tsk)) {
		ret = ux_page_pool_tsk ? PTR_ERR(ux_page_pool_tsk) : -ENOMEM;
		pr_err("%s: run ux_page_pool_fillthread failed: %d\n", __func__, ret);
		ux_page_pool_tsk = NULL;
		WRITE_ONCE(ux_page_pool_enabled, false);
		unregister_shrinker(&ux_page_pool_shrinker);
		goto err_free_pools;
	}
	WRITE_ONCE(fillthread_enabled, true);
	WRITE_ONCE(free_to_pool, true);
	if (pools[0])
		page_pool_wakeup_process(pools[0]);

	if (!proc_create("ux_page_pool", 0644, NULL, &ux_page_pool_fops))
		pr_err("%s: failed to create /proc/ux_page_pool\n", __func__);

	inited = true;
	return 0;

err_free_pools:
	for (i = 0; i < NUM_ORDERS; i++) {
		if (!IS_ERR_OR_NULL(pools[i]))
			kfree(pools[i]);
		pools[i] = NULL;
	}
	return ret;
}
module_init(ux_page_pool_init);

int ux_page_pool_enable_handler(struct ctl_table *table, int write,
  	void __user *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		if (ux_page_pool_enable) {
			ret = ux_page_pool_init();
			if (ret)
				return ret;
		}
	}
	return 0;
}
