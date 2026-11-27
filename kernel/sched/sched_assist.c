// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 * Ported for OxygenOS 16 / ColorOS support on Samsung SM8250 (r8q).
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched_assist.h>
#include <linux/tachyon.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>

#define OPLUS_SCHEDULER_PROC_DIR	"oplus_scheduler"
#define OPLUS_SCHEDASSIST_PROC_DIR	"sched_assist"

int sysctl_sched_assist_enabled = 1;
EXPORT_SYMBOL(sysctl_sched_assist_enabled);

int sysctl_sched_assist_scene = 0;
EXPORT_SYMBOL(sysctl_sched_assist_scene);

int global_debug_enabled = 0;
EXPORT_SYMBOL(global_debug_enabled);

static int save_top_app_tgid = 0;
static int top_app_type = 0;
static int save_audio_tgid = 0;
static pid_t global_im_flag_pid = -1;

/* Audio performance nodes */
int sa_audio_perf_enable = 1;
EXPORT_SYMBOL(sa_audio_perf_enable);

int sa_audio_perf_status = 0;
EXPORT_SYMBOL(sa_audio_perf_status);

int sa_audio_debug_enable = 0;
EXPORT_SYMBOL(sa_audio_debug_enable);

/* EAS Opt and IOWait nodes */
int eas_opt_enable = 0;
EXPORT_SYMBOL(eas_opt_enable);

int eas_opt_debug_enable = 0;
EXPORT_SYMBOL(eas_opt_debug_enable);

int force_apply_ocap_enable = 0;
EXPORT_SYMBOL(force_apply_ocap_enable);

int group_adjust_enable = 0;
EXPORT_SYMBOL(group_adjust_enable);

int fake_cap_multiple = 100;
EXPORT_SYMBOL(fake_cap_multiple);

int util_thresh_percent = 85;
EXPORT_SYMBOL(util_thresh_percent);

int group_adjust = 0;
EXPORT_SYMBOL(group_adjust);

unsigned int sysctl_iowait_reset_ticks = 1;
EXPORT_SYMBOL(sysctl_iowait_reset_ticks);

unsigned int sysctl_iowait_apply_ticks = 0;
EXPORT_SYMBOL(sysctl_iowait_apply_ticks);

unsigned int sysctl_oplus_iowait_boost_enabled = 0;
EXPORT_SYMBOL(sysctl_oplus_iowait_boost_enabled);

unsigned int sysctl_oplus_iowait_skip_min_enabled = 1;
EXPORT_SYMBOL(sysctl_oplus_iowait_skip_min_enabled);

static char tpd_cmds_buf[128] = "";
static int tpd_id_val = 0;

void sched_assist_tag_comm(struct task_struct *task)
{
	if (!task || !task->comm[0])
		return;

	if (!strcmp(task->comm, "surfaceflinger") ||
	    !strcmp(task->comm, "RenderEngine")) {
		task->ux_im_flag = IM_FLAG_SURFACEFLINGER;
		task->ux_state |= SA_TYPE_ANIMATOR | SA_TYPE_AUTO;
	} else if (!strcmp(task->comm, "RenderThread")) {
		task->ux_state |= SA_TYPE_ANIMATOR | SA_TYPE_AUTO;
	} else if (strstr(task->comm, "composer")) {
		task->ux_im_flag = IM_FLAG_HWC;
		task->ux_state |= SA_TYPE_AUTO;
	} else if (!strcmp(task->comm, "system_server") ||
		   strstr(task->comm, "launcher")) {
		task->ux_im_flag = IM_FLAG_LAUNCHER;
		task->ux_state |= SA_TYPE_HEAVY | SA_TYPE_AUTO;
	} else if (task->ux_state & SA_TYPE_AUTO) {
		task->ux_state &= ~(SA_TYPE_HEAVY | SA_TYPE_ANIMATOR | SA_TYPE_AUTO);
		if (task->ux_im_flag == IM_FLAG_SURFACEFLINGER ||
		    task->ux_im_flag == IM_FLAG_HWC ||
		    task->ux_im_flag == IM_FLAG_LAUNCHER)
			task->ux_im_flag = 0;
	}
}
EXPORT_SYMBOL(sched_assist_tag_comm);

/* UX task identification for block UXIO and scheduler */
bool test_task_ux(struct task_struct *task)
{
	if (!sysctl_sched_assist_enabled || !task)
		return false;

	if (task->ux_state & (SA_TYPE_LIGHT | SA_TYPE_HEAVY | SA_TYPE_ANIMATOR |
			      SA_TYPE_LISTPICK | SA_TYPE_INHERIT | SA_TYPE_ONCE_UX |
			      SA_TYPE_ID_CAMERA_PROVIDER | SA_TYPE_ID_ALLOCATOR_SER))
		return true;

	if (task->ux_im_flag == IM_FLAG_SURFACEFLINGER ||
	    task->ux_im_flag == IM_FLAG_RENDERENGINE ||
	    task->ux_im_flag == IM_FLAG_HWC ||
	    task->ux_im_flag == IM_FLAG_LAUNCHER ||
	    task->ux_im_flag == IM_FLAG_LAUNCHER_NON_UX_RENDER)
		return true;

	return false;
}
EXPORT_SYMBOL(test_task_ux);

/* Generic simple int proc read/write macros */
#define DEFINE_PROC_INT_OPS(name, var)						\
static ssize_t proc_##name##_read(struct file *file, char __user *buf,		\
		size_t count, loff_t *ppos)					\
{										\
	char buffer[32];							\
	size_t len = snprintf(buffer, sizeof(buffer), "%d\n", var);		\
	return simple_read_from_buffer(buf, count, ppos, buffer, len);		\
}										\
static ssize_t proc_##name##_write(struct file *file, const char __user *buf,	\
		size_t count, loff_t *ppos)					\
{										\
	char buffer[32];							\
	int val, err;								\
	if (count > sizeof(buffer) - 1)						\
		return -EFAULT;							\
	if (copy_from_user(buffer, buf, count))					\
		return -EFAULT;							\
	buffer[count] = '\0';							\
	err = kstrtoint(strstrip(buffer), 0, &val);				\
	if (err)								\
		return err;							\
	var = val;								\
	return count;								\
}										\
static const struct file_operations proc_##name##_fops = {			\
	.read = proc_##name##_read,						\
	.write = proc_##name##_write,						\
	.llseek = default_llseek,						\
};

#define DEFINE_PROC_UINT_OPS(name, var)						\
static ssize_t proc_##name##_read(struct file *file, char __user *buf,		\
		size_t count, loff_t *ppos)					\
{										\
	char buffer[32];							\
	size_t len = snprintf(buffer, sizeof(buffer), "%u\n", var);		\
	return simple_read_from_buffer(buf, count, ppos, buffer, len);		\
}										\
static ssize_t proc_##name##_write(struct file *file, const char __user *buf,	\
		size_t count, loff_t *ppos)					\
{										\
	char buffer[32];							\
	unsigned int val;							\
	int err;								\
	if (count > sizeof(buffer) - 1)						\
		return -EFAULT;							\
	if (copy_from_user(buffer, buf, count))					\
		return -EFAULT;							\
	buffer[count] = '\0';							\
	err = kstrtouint(strstrip(buffer), 0, &val);				\
	if (err)								\
		return err;							\
	var = val;								\
	return count;								\
}										\
static const struct file_operations proc_##name##_fops = {			\
	.read = proc_##name##_read,						\
	.write = proc_##name##_write,						\
	.llseek = default_llseek,						\
};

DEFINE_PROC_INT_OPS(sa_enabled, sysctl_sched_assist_enabled);
DEFINE_PROC_INT_OPS(debug_enabled, global_debug_enabled);
DEFINE_PROC_INT_OPS(audio_enable, sa_audio_perf_enable);
DEFINE_PROC_INT_OPS(audio_status, sa_audio_perf_status);
DEFINE_PROC_INT_OPS(audio_debug, sa_audio_debug_enable);

DEFINE_PROC_INT_OPS(eas_opt_enable, eas_opt_enable);
DEFINE_PROC_INT_OPS(group_adjust_enable, group_adjust_enable);
DEFINE_PROC_INT_OPS(fake_cap_multiple, fake_cap_multiple);
DEFINE_PROC_INT_OPS(util_thresh_percent, util_thresh_percent);
DEFINE_PROC_INT_OPS(group_adjust, group_adjust);

DEFINE_PROC_UINT_OPS(iowait_reset_ticks, sysctl_iowait_reset_ticks);
DEFINE_PROC_UINT_OPS(iowait_apply_ticks, sysctl_iowait_apply_ticks);
DEFINE_PROC_UINT_OPS(oplus_iowait_boost_enabled, sysctl_oplus_iowait_boost_enabled);
DEFINE_PROC_UINT_OPS(oplus_iowait_skip_min_enabled, sysctl_oplus_iowait_skip_min_enabled);

DEFINE_PROC_INT_OPS(tpd_id, tpd_id_val);

/* /proc/oplus_scheduler/sched_assist/sched_assist_scene */
static ssize_t proc_sched_assist_scene_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[32];
	size_t len = snprintf(buffer, sizeof(buffer), "%d\n", sysctl_sched_assist_scene);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_sched_assist_scene_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[32];
	int val, err, save_sa;

	if (count > sizeof(buffer) - 1)
		return -EFAULT;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 0, &val);
	if (err)
		return err;

	save_sa = sysctl_sched_assist_scene;
	if (val == SA_SCENE_OPT_CLEAR)
		save_sa = SA_SCENE_OPT_CLEAR;
	else if (val & SA_SCENE_OPT_SET)
		save_sa |= (val & ~SA_SCENE_OPT_SET);
	else
		save_sa &= ~val;

	sysctl_sched_assist_scene = save_sa;
	return count;
}

static const struct file_operations proc_sched_assist_scene_fops = {
	.read = proc_sched_assist_scene_read,
	.write = proc_sched_assist_scene_write,
	.llseek = default_llseek,
};

int sched_assist_scene_sysctl_handler(struct ctl_table *table, int write,
				      void __user *buffer, size_t *lenp,
				      loff_t *ppos)
{
	int val, err, save_sa;
	struct ctl_table tmp = *table;

	if (!write)
		return proc_dointvec(table, write, buffer, lenp, ppos);

	tmp.data = &val;
	err = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (err)
		return err;

	save_sa = sysctl_sched_assist_scene;
	if (val == SA_SCENE_OPT_CLEAR)
		save_sa = SA_SCENE_OPT_CLEAR;
	else if (val & SA_SCENE_OPT_SET)
		save_sa |= (val & ~SA_SCENE_OPT_SET);
	else
		save_sa &= ~val;

	sysctl_sched_assist_scene = save_sa;
	return 0;
}
EXPORT_SYMBOL(sched_assist_scene_sysctl_handler);

/* /proc/oplus_scheduler/sched_assist/sched_impt_task */
static ssize_t proc_sched_impt_task_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char temp_buf[128];
	char *temp_str, *token;
	char in_str[2][32];
	int cnt, err, pid;
	static DEFINE_MUTEX(impt_thd_mutex);

	if (count > sizeof(temp_buf) - 1)
		count = sizeof(temp_buf) - 1;
	if (copy_from_user(temp_buf, buf, count))
		return -EFAULT;

	temp_buf[count] = '\0';
	cnt = 0;
	temp_str = strstrip(temp_buf);
	while ((token = strsep(&temp_str, " ")) && *token && (cnt < 2)) {
		strncpy(in_str[cnt], token, sizeof(in_str[cnt]) - 1);
		in_str[cnt][sizeof(in_str[cnt]) - 1] = '\0';
		cnt++;
	}

	if (cnt != 2)
		return -EINVAL;

	err = kstrtoint(strstrip(in_str[1]), 0, &pid);
	if (err)
		return err;

	if (pid < 0 || pid > PID_MAX_LIMIT)
		return -EINVAL;

	mutex_lock(&impt_thd_mutex);
	if (!strncmp(in_str[0], "fg", 2) || !strncmp(in_str[0], "top", 3)) {
		save_top_app_tgid = pid;
		top_app_type = 0;
	} else if (!strncmp(in_str[0], "au", 2)) {
		save_audio_tgid = pid;
	}
	mutex_unlock(&impt_thd_mutex);

	return count;
}

static ssize_t proc_sched_impt_task_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[64];
	size_t len = snprintf(buffer, sizeof(buffer), "top(%d %u) au(%d)\n",
			      save_top_app_tgid, top_app_type, save_audio_tgid);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct file_operations proc_sched_impt_task_fops = {
	.read = proc_sched_impt_task_read,
	.write = proc_sched_impt_task_write,
	.llseek = default_llseek,
};

/* /proc/oplus_scheduler/sched_assist/im_flag */
static ssize_t proc_im_flag_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char temp_buf[128];
	char *temp_str, *token;
	char in_str[3][32];
	int cnt = 0, pid = -1, flag = 0, err;
	struct task_struct *task;

	if (count > sizeof(temp_buf) - 1)
		count = sizeof(temp_buf) - 1;
	if (copy_from_user(temp_buf, buf, count))
		return -EFAULT;

	temp_buf[count] = '\0';
	temp_str = strstrip(temp_buf);
	while ((token = strsep(&temp_str, " ")) && *token && (cnt < 3)) {
		strncpy(in_str[cnt], token, sizeof(in_str[cnt]) - 1);
		in_str[cnt][sizeof(in_str[cnt]) - 1] = '\0';
		cnt++;
	}

	if (cnt == 2) {
		err = kstrtoint(strstrip(in_str[0]), 0, &pid);
		if (!err)
			err = kstrtoint(strstrip(in_str[1]), 0, &flag);
	} else if (cnt == 3 && !strncmp(in_str[0], "set", 3)) {
		err = kstrtoint(strstrip(in_str[1]), 0, &pid);
		if (!err)
			err = kstrtoint(strstrip(in_str[2]), 0, &flag);
	} else {
		return -EINVAL;
	}

	if (err || pid <= 0 || pid > PID_MAX_LIMIT)
		return -EINVAL;

	global_im_flag_pid = pid;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task) {
		get_task_struct(task);
		rcu_read_unlock();

		task->ux_im_flag = flag;
		if (flag == IM_FLAG_LAUNCHER_NON_UX_RENDER)
			task->ux_state |= SA_TYPE_HEAVY;
		else if (flag == IM_FLAG_SURFACEFLINGER || flag == IM_FLAG_RENDERENGINE)
			task->ux_state |= SA_TYPE_ANIMATOR;
		else if (flag == IM_FLAG_NONE) {
			task->ux_im_flag = 0;
			task->ux_state &= ~(SA_TYPE_HEAVY | SA_TYPE_ANIMATOR);
		}

		put_task_struct(task);
	} else {
		rcu_read_unlock();
	}

	return count;
}

static ssize_t proc_im_flag_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[32];
	size_t len = snprintf(buffer, sizeof(buffer), "im_flag_pid=%d\n", global_im_flag_pid);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct file_operations proc_im_flag_fops = {
	.read = proc_im_flag_read,
	.write = proc_im_flag_write,
	.llseek = default_llseek,
};

/* /proc/oplus_scheduler/sched_assist/ux_task */
static ssize_t proc_ux_task_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char temp_buf[128];
	int pid = -1, state = 0;
	int ret_scan;
	struct task_struct *task;

	if (count > sizeof(temp_buf) - 1)
		count = sizeof(temp_buf) - 1;
	if (copy_from_user(temp_buf, buf, count))
		return -EFAULT;

	temp_buf[count] = '\0';
	ret_scan = sscanf(temp_buf, "%d %d", &pid, &state);
	if (ret_scan >= 1) {
		if (pid > 0 && pid <= PID_MAX_LIMIT) {
			rcu_read_lock();
			task = find_task_by_vpid(pid);
			if (task) {
				get_task_struct(task);
				rcu_read_unlock();

				if (ret_scan == 1 || state == SA_OPT_CLEAR) {
					task->ux_state = 0;
				} else if (state & SA_OPT_SET) {
					task->ux_state |= (state & ~SA_OPT_SET);
				} else {
					task->ux_state &= ~state;
				}

				put_task_struct(task);
			} else {
				rcu_read_unlock();
			}
		}
	}

	return count;
}

static ssize_t proc_ux_task_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[16] = "ux_task\n";
	return simple_read_from_buffer(buf, count, ppos, buffer, strlen(buffer));
}

static const struct file_operations proc_ux_task_fops = {
	.read = proc_ux_task_read,
	.write = proc_ux_task_write,
	.llseek = default_llseek,
};

/* /proc/oplus_scheduler/sched_assist/tpd_cmds */
static ssize_t proc_tpd_cmds_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, tpd_cmds_buf, strlen(tpd_cmds_buf));
}

static ssize_t proc_tpd_cmds_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	if (count > sizeof(tpd_cmds_buf) - 1)
		count = sizeof(tpd_cmds_buf) - 1;
	if (copy_from_user(tpd_cmds_buf, buf, count))
		return -EFAULT;
	tpd_cmds_buf[count] = '\0';
	return count;
}

static const struct file_operations proc_tpd_cmds_fops = {
	.read = proc_tpd_cmds_read,
	.write = proc_tpd_cmds_write,
	.llseek = default_llseek,
};

/* OPlus Binder UX interfaces */
int sysctl_oplus_binder_async_ux_enable = 1;
EXPORT_SYMBOL(sysctl_oplus_binder_async_ux_enable);

int sysctl_oplus_binder_ux_flag = 1;
EXPORT_SYMBOL(sysctl_oplus_binder_ux_flag);

static int proc_binder_async_ux_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_oplus_binder_async_ux_enable);
	return 0;
}

static int proc_binder_async_ux_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_binder_async_ux_enable_show, NULL);
}

static ssize_t proc_binder_async_ux_enable_write(struct file *file, const char __user *buf,
						size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (kstrtoint(strstrip(kbuf), 10, &val) == 0)
		sysctl_oplus_binder_async_ux_enable = val;

	return count;
}

static const struct file_operations proc_binder_async_ux_enable_fops = {
	.open = proc_binder_async_ux_enable_open,
	.read = seq_read,
	.write = proc_binder_async_ux_enable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int proc_binder_ux_flag_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_oplus_binder_ux_flag);
	return 0;
}

static int proc_binder_ux_flag_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_binder_ux_flag_show, NULL);
}

static ssize_t proc_binder_ux_flag_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	char kbuf[64];
	int tid = 0, flag = 0;
	struct task_struct *task;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d %d", &tid, &flag) == 2) {
		rcu_read_lock();
		task = find_task_by_vpid(tid);
		if (task) {
			if (flag)
				task->ux_state |= SA_TYPE_ONCE_UX;
			else
				task->ux_state &= ~SA_TYPE_ONCE_UX;
		}
		rcu_read_unlock();
	} else if (kstrtoint(strstrip(kbuf), 10, &flag) == 0) {
		sysctl_oplus_binder_ux_flag = flag;
		if (flag)
			current->ux_state |= SA_TYPE_ONCE_UX;
		else
			current->ux_state &= ~SA_TYPE_ONCE_UX;
	}

	return count;
}

static const struct file_operations proc_binder_ux_flag_fops = {
	.open = proc_binder_ux_flag_open,
	.read = seq_read,
	.write = proc_binder_ux_flag_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int proc_binder_async_ux_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_oplus_binder_async_ux_enable);
	return 0;
}

static int proc_binder_async_ux_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_binder_async_ux_show, NULL);
}

static ssize_t proc_binder_async_ux_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	char kbuf[64];
	int tid = 0, val = 0;
	struct task_struct *task;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d %d", &tid, &val) == 2) {
		rcu_read_lock();
		task = find_task_by_vpid(tid);
		if (task) {
			if (val)
				task->ux_state |= SA_TYPE_HEAVY;
			else
				task->ux_state &= ~SA_TYPE_HEAVY;
		}
		rcu_read_unlock();
	} else if (kstrtoint(strstrip(kbuf), 10, &val) == 0) {
		sysctl_oplus_binder_async_ux_enable = val;
	}

	return count;
}

static const struct file_operations proc_binder_async_ux_fops = {
	.open = proc_binder_async_ux_open,
	.read = seq_read,
	.write = proc_binder_async_ux_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Initialization */
static struct proc_dir_entry *d_oplus_scheduler;
static struct proc_dir_entry *d_sched_assist;
static struct proc_dir_entry *d_sched_assist_audio;
static struct proc_dir_entry *d_eas_opt;
static struct proc_dir_entry *d_eas_opt_iowait;
static struct proc_dir_entry *d_oplus_binder;
static struct proc_dir_entry *d_oplus_afs_config;

/* /proc/oplus_afs_config/afs_config */
static int proc_afs_config_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enable: 1\n");
	return 0;
}

static int proc_afs_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_afs_config_show, NULL);
}

static ssize_t proc_afs_config_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	return count;
}

static const struct file_operations proc_afs_config_fops = {
	.open = proc_afs_config_open,
	.read = seq_read,
	.write = proc_afs_config_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static inline void set_proc_system_user(struct proc_dir_entry *de)
{
	if (de)
		proc_set_user(de, make_kuid(&init_user_ns, 1000), make_kgid(&init_user_ns, 1000));
}

static int __init oplus_sched_assist_init(void)
{
	/* /proc/oplus_scheduler/sched_assist/ */
	d_oplus_scheduler = proc_mkdir(OPLUS_SCHEDULER_PROC_DIR, NULL);
	if (!d_oplus_scheduler)
		pr_err("sched_assist: failed to create proc dir %s\n", OPLUS_SCHEDULER_PROC_DIR);
	else
		set_proc_system_user(d_oplus_scheduler);

	d_sched_assist = proc_mkdir(OPLUS_SCHEDASSIST_PROC_DIR, d_oplus_scheduler);
	if (d_sched_assist) {
		set_proc_system_user(d_sched_assist);
		set_proc_system_user(proc_create("sched_assist_enabled", 0666, d_sched_assist, &proc_sa_enabled_fops));
		set_proc_system_user(proc_create("sched_assist_scene", 0666, d_sched_assist, &proc_sched_assist_scene_fops));
		set_proc_system_user(proc_create("sched_impt_task", 0666, d_sched_assist, &proc_sched_impt_task_fops));
		set_proc_system_user(proc_create("im_flag", 0666, d_sched_assist, &proc_im_flag_fops));
		set_proc_system_user(proc_create("im_flag_app", 0666, d_sched_assist, &proc_im_flag_fops));
		set_proc_system_user(proc_create("debug_enabled", 0666, d_sched_assist, &proc_debug_enabled_fops));
		set_proc_system_user(proc_create("ux_task", 0666, d_sched_assist, &proc_ux_task_fops));
		set_proc_system_user(proc_create("tpd_cmds", 0666, d_sched_assist, &proc_tpd_cmds_fops));
		set_proc_system_user(proc_create("tpd_id", 0666, d_sched_assist, &proc_tpd_id_fops));

		/* /proc/oplus_scheduler/sched_assist/audio/ */
		d_sched_assist_audio = proc_mkdir("audio", d_sched_assist);
		if (d_sched_assist_audio) {
			set_proc_system_user(d_sched_assist_audio);
			set_proc_system_user(proc_create("status", 0666, d_sched_assist_audio, &proc_audio_status_fops));
			set_proc_system_user(proc_create("enable", 0666, d_sched_assist_audio, &proc_audio_enable_fops));
			set_proc_system_user(proc_create("debug", 0666, d_sched_assist_audio, &proc_audio_debug_fops));
		}
	}

	/* /proc/eas_opt/ */
	d_eas_opt = proc_mkdir("eas_opt", NULL);
	if (d_eas_opt) {
		set_proc_system_user(d_eas_opt);
		set_proc_system_user(proc_create("eas_opt_enable", 0666, d_eas_opt, &proc_eas_opt_enable_fops));
		set_proc_system_user(proc_create("group_adjust_enable", 0666, d_eas_opt, &proc_group_adjust_enable_fops));
		set_proc_system_user(proc_create("fake_cap_multiple", 0666, d_eas_opt, &proc_fake_cap_multiple_fops));
		set_proc_system_user(proc_create("oplus_cap_multiple", 0666, d_eas_opt, &proc_fake_cap_multiple_fops));
		set_proc_system_user(proc_create("util_thresh_percent", 0666, d_eas_opt, &proc_util_thresh_percent_fops));
		set_proc_system_user(proc_create("group_adjust", 0666, d_eas_opt, &proc_group_adjust_fops));

		/* /proc/eas_opt/iowait_config/ */
		d_eas_opt_iowait = proc_mkdir("iowait_config", d_eas_opt);
		if (d_eas_opt_iowait) {
			set_proc_system_user(d_eas_opt_iowait);
			set_proc_system_user(proc_create("iowait_apply_ticks", 0666, d_eas_opt_iowait, &proc_iowait_apply_ticks_fops));
			set_proc_system_user(proc_create("iowait_reset_ticks", 0666, d_eas_opt_iowait, &proc_iowait_reset_ticks_fops));
			set_proc_system_user(proc_create("oplus_iowait_boost_enabled", 0666, d_eas_opt_iowait, &proc_oplus_iowait_boost_enabled_fops));
			set_proc_system_user(proc_create("oplus_iowait_skip_min_enabled", 0666, d_eas_opt_iowait, &proc_oplus_iowait_skip_min_enabled_fops));
		}
	}

	/* /proc/oplus_binder/ */
	d_oplus_binder = proc_mkdir("oplus_binder", NULL);
	if (d_oplus_binder) {
		set_proc_system_user(d_oplus_binder);
		set_proc_system_user(proc_create("async_ux_enable", 0666, d_oplus_binder, &proc_binder_async_ux_enable_fops));
		set_proc_system_user(proc_create("async_ux", 0666, d_oplus_binder, &proc_binder_async_ux_fops));
		set_proc_system_user(proc_create("ux_flag", 0666, d_oplus_binder, &proc_binder_ux_flag_fops));
		set_proc_system_user(proc_create("im_flag", 0666, d_oplus_binder, &proc_im_flag_fops));
	}

	/* /proc/oplus_afs_config/ */
	d_oplus_afs_config = proc_mkdir("oplus_afs_config", NULL);
	if (d_oplus_afs_config) {
		set_proc_system_user(d_oplus_afs_config);
		set_proc_system_user(proc_create("afs_config", 0666, d_oplus_afs_config, &proc_afs_config_fops));
	}

	/* Core author validation and anti-kanger integrity */
	BUILD_BUG_ON_MSG(!TACHYON_AUTHOR, "Tachyon Kernel: author signature missing");
	if (tachyon_validate_integrity() != TACHYON_MAGIC)
		panic("Tachyon Kernel: author verification failed\n");

	pr_info("sched_assist: proc interfaces registered successfully\n");
	return 0;
}
late_initcall(oplus_sched_assist_init);

MODULE_DESCRIPTION("Oplus Sched Assist & UX Controller");
MODULE_LICENSE("GPL v2");
