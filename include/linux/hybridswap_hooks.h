/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HYBRIDSWAP_HOOKS_H
#define _LINUX_HYBRIDSWAP_HOOKS_H

#include <linux/types.h>

struct mem_cgroup;

void hybridswap_memcg_alloc(struct mem_cgroup *memcg);
void hybridswap_memcg_free(struct mem_cgroup *memcg);
void hybridswap_memcg_online(struct mem_cgroup *memcg);
void hybridswap_memcg_offline(struct mem_cgroup *memcg);
void hybridswap_memcg_id_remove(struct mem_cgroup *memcg);
void hybridswap_tune_scan_type(char *scan_balance);
void hybridswap_rmqueue(gfp_t gfp_flags);

#endif
