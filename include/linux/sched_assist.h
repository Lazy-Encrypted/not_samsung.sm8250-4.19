/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _LINUX_SCHED_ASSIST_H
#define _LINUX_SCHED_ASSIST_H

#include <linux/types.h>

struct task_struct;

/* Define for sched assist thread type */
#define SA_OPT_CLEAR     (0)
#define SA_TYPE_LIGHT    (1 << 0)
#define SA_TYPE_HEAVY    (1 << 1)
#define SA_TYPE_ANIMATOR (1 << 2)
#define SA_TYPE_LISTPICK (1 << 3)
#define SA_OPT_SET       (1 << 7)
#define SA_TYPE_INHERIT  (1 << 8)
#define SA_TYPE_ONCE_UX  (1 << 9)
#define SA_TYPE_ID_CAMERA_PROVIDER  (1 << 10)
#define SA_TYPE_ID_ALLOCATOR_SER    (1 << 11)

#define SCHED_ASSIST_UX_MASK (0xFF)

/* Define for sched assist scene type */
#define SA_SCENE_OPT_CLEAR  (0)
#define SA_LAUNCH           (1 << 0)
#define SA_SLIDE            (1 << 1)
#define SA_CAMERA           (1 << 2)
#define SA_ANIM_START       (1 << 3)
#define SA_ANIM             (1 << 4)
#define SA_INPUT            (1 << 5)
#define SA_LAUNCHER_SI      (1 << 6)
#define SA_SCENE_OPT_SET    (1 << 7)
#define SA_GPU_COMPOSITION  (1 << 8)
#define SA_AUDIO            (1 << 9)

/* IM Flag Types */
enum IM_FLAG_TYPE {
	IM_FLAG_NONE = 0,
	IM_FLAG_SURFACEFLINGER,
	IM_FLAG_HWC,
	IM_FLAG_RENDERENGINE,
	IM_FLAG_WEBVIEW,
	IM_FLAG_CAMERA_HAL,
	IM_FLAG_AUDIO,
	IM_FLAG_HWBINDER,
	IM_FLAG_LAUNCHER,
	IM_FLAG_LAUNCHER_NON_UX_RENDER,
	IM_FLAG_SS_LOCK_OWNER,
	IM_FLAG_CAMERA_SERVER,
	IM_FLAG_SYSTEMSERVER_PID,
	IM_FLAG_FORBID_SET_CPU_AFFINITY_IN_KERNEL,
	IM_FLAG_MIDASD,
	MAX_IM_FLAG_TYPE,
	IM_FLAG_FORBID_SET_CPU_AFFINITY = 11,
};

extern int sysctl_sched_assist_enabled;
extern int sysctl_sched_assist_scene;
extern int sysctl_oplus_binder_async_ux_enable;
extern int sysctl_oplus_binder_ux_flag;

#ifdef CONFIG_OPLUS_FEATURE_SCHED_ASSIST
bool test_task_ux(struct task_struct *task);
#else
static inline bool test_task_ux(struct task_struct *task) { return false; }
#endif

#endif /* _LINUX_SCHED_ASSIST_H */
