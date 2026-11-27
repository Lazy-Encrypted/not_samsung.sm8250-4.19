// SPDX-License-Identifier: GPL-2.0-or-later
/* Filesystem access-by-fd.
 *
 * Copyright (C) 2017 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/anon_inodes.h>
#include <linux/namei.h>
#include <linux/file.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <uapi/linux/mount.h>
#include "internal.h"

/*
 * Allow the user to read back any error, warning or informational messages.
 */
static int fscontext_release(struct inode *inode, struct file *file)
{
	struct fs_context *fc = file->private_data;

	if (fc) {
		file->private_data = NULL;
		put_fs_context(fc);
	}
	return 0;
}

const struct file_operations fscontext_fops = {
	.release	= fscontext_release,
	.llseek		= no_llseek,
};

/*
 * Attach a filesystem context to a file and an fd.
 */
static int fscontext_create_fd(struct fs_context *fc, unsigned int o_flags)
{
	int fd;

	fd = anon_inode_getfd("[fscontext]", &fscontext_fops, fc,
			      O_RDWR | o_flags);
	if (fd < 0)
		put_fs_context(fc);
	return fd;
}

static int fscontext_alloc_log(struct fs_context *fc)
{
	return 0;
}

/**
 * sys_fsopen - Open a filesystem context
 * @_fs_name: The name of the filesystem to open
 * @flags: FSOPEN_* flags
 *
 * This does not mount the filesystem, but opens a context file descriptor
 * that can be used to configure the superblock via fsconfig() and then
 * instantiated as a mount via fsmount().
 */
SYSCALL_DEFINE2(fsopen, const char __user *, _fs_name, unsigned int, flags)
{
	struct file_system_type *fs_type;
	struct fs_context *fc;
	const char *subtype = NULL;
	char *fs_name;
	int ret;

	if (!may_mount())
		return -EPERM;

	if (flags & ~FSOPEN_CLOEXEC)
		return -EINVAL;

	fs_name = strndup_user(_fs_name, PAGE_SIZE);
	if (IS_ERR(fs_name))
		return PTR_ERR(fs_name);

	fs_type = get_fs_type(fs_name);
	if (!fs_type) {
		kfree(fs_name);
		return -ENODEV;
	}

	if (fs_type->fs_flags & FS_HAS_SUBTYPE) {
		subtype = strchr(fs_name, '.');
		if (subtype) {
			subtype++;
			if (!*subtype) {
				put_filesystem(fs_type);
				kfree(fs_name);
				return -EINVAL;
			}
		}
	}

	fc = fs_context_for_mount(fs_type, 0);
	put_filesystem(fs_type);
	if (IS_ERR(fc)) {
		kfree(fs_name);
		return PTR_ERR(fc);
	}

	ret = fscontext_alloc_log(fc);
	if (ret < 0)
		goto err_fc;

	if (subtype) {
		ret = vfs_parse_fs_string(fc, "subtype",
					  subtype, strlen(subtype));
		if (ret < 0)
			goto err_fc;
	}

	kfree(fs_name);
	return fscontext_create_fd(fc, flags & FSOPEN_CLOEXEC ? O_CLOEXEC : 0);

err_fc:
	kfree(fs_name);
	put_fs_context(fc);
	return ret;
}

/**
 * sys_fspick - Open a filesystem context for reconfiguring an existing mount
 * @dfd: Directory file descriptor to resolve @path relative to
 * @path: Path to an existing mountpoint
 * @flags: FSPICK_* flags
 */
SYSCALL_DEFINE3(fspick, int, dfd, const char __user *, path, unsigned int, flags)
{
	struct fs_context *fc;
	struct path target;
	unsigned int lflags;
	int ret;

	if (!may_mount())
		return -EPERM;

	if ((flags & ~(FSPICK_CLOEXEC | FSPICK_SYMLINK_NOFOLLOW |
		       FSPICK_NO_AUTOMOUNT | FSPICK_EMPTY_PATH)) != 0)
		return -EINVAL;

	lflags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	if (flags & FSPICK_SYMLINK_NOFOLLOW)
		lflags &= ~LOOKUP_FOLLOW;
	if (flags & FSPICK_NO_AUTOMOUNT)
		lflags &= ~LOOKUP_AUTOMOUNT;
	if (flags & FSPICK_EMPTY_PATH)
		lflags |= LOOKUP_EMPTY;

	ret = user_path_at(dfd, path, lflags, &target);
	if (ret < 0)
		return ret;

	ret = -EINVAL;
	if (!our_mnt(target.mnt))
		goto err_path;

	ret = -EPERM;
	if (!ns_capable(target.dentry->d_sb->s_user_ns, CAP_SYS_ADMIN))
		goto err_path;

	fc = fs_context_for_reconfigure(target.dentry, 0, 0);
	path_put(&target);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	return fscontext_create_fd(fc, flags & FSPICK_CLOEXEC ? O_CLOEXEC : 0);

err_path:
	path_put(&target);
	return ret;
}

static int vfs_cmd_create(struct fs_context *fc)
{
	struct super_block *sb;
	int ret;

	if (fc->phase != FS_CONTEXT_CREATE_PARAMS)
		return -EBUSY;

	fc->phase = FS_CONTEXT_CREATING;
	ret = vfs_get_tree(fc);
	if (ret) {
		fc->phase = FS_CONTEXT_FAILED;
		return ret;
	}

	sb = fc->root->d_sb;
	ret = security_sb_kern_mount(sb);
	if (unlikely(ret)) {
		fc_drop_locked(fc);
		fc->phase = FS_CONTEXT_FAILED;
		return ret;
	}

	up_write(&sb->s_umount);
	fc->phase = FS_CONTEXT_AWAITING_MOUNT;
	return 0;
}

static int vfs_cmd_reconfigure(struct fs_context *fc)
{
	struct super_block *sb;
	int ret;

	if (fc->phase != FS_CONTEXT_RECONF_PARAMS)
		return -EBUSY;

	fc->phase = FS_CONTEXT_RECONFIGURING;
	sb = fc->root->d_sb;
	if (!ns_capable(sb->s_user_ns, CAP_SYS_ADMIN)) {
		fc->phase = FS_CONTEXT_FAILED;
		return -EPERM;
	}

	down_write(&sb->s_umount);
	ret = reconfigure_super(fc);
	up_write(&sb->s_umount);
	if (ret) {
		fc->phase = FS_CONTEXT_FAILED;
		return ret;
	}

	fc->phase = FS_CONTEXT_FAILED;
	return 0;
}

/*
 * Apply a configuration parameter or command while holding fc->uapi_mutex.
 */
static int vfs_fsconfig_locked(struct fs_context *fc, int cmd,
			       struct fs_parameter *param)
{
	switch (cmd) {
	case FSCONFIG_CMD_CREATE:
		return vfs_cmd_create(fc);
	case FSCONFIG_CMD_RECONFIGURE:
		return vfs_cmd_reconfigure(fc);
	default:
		if (fc->phase != FS_CONTEXT_CREATE_PARAMS &&
		    fc->phase != FS_CONTEXT_RECONF_PARAMS)
			return -EBUSY;
		return vfs_parse_fs_param(fc, param);
	}
}

/**
 * sys_fsconfig - Set filesystem configuration parameter or execute command
 * @fd: Filesystem context file descriptor from fsopen()/fspick()
 * @cmd: FSCONFIG_* command
 * @_key: Parameter key string (or NULL for commands)
 * @_value: Parameter value pointer (command-dependent)
 * @aux: Auxiliary integer (dirfd, size, or fd depending on @cmd)
 */
SYSCALL_DEFINE5(fsconfig, int, fd, unsigned int, cmd,
		const char __user *, _key,
		const void __user *, _value,
		int, aux)
{
	struct fs_context *fc;
	struct fd f;
	struct fs_parameter param = {
		.type = fs_value_is_undefined,
	};
	int ret;

	if (fd < 0)
		return -EINVAL;

	switch (cmd) {
	case FSCONFIG_SET_FLAG:
	case FSCONFIG_SET_STRING:
	case FSCONFIG_SET_BINARY:
	case FSCONFIG_SET_PATH:
	case FSCONFIG_SET_PATH_EMPTY:
	case FSCONFIG_SET_FD:
	case FSCONFIG_CMD_CREATE:
	case FSCONFIG_CMD_RECONFIGURE:
		break;
	default:
		return -EOPNOTSUPP;
	}

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EINVAL;
	if (f.file->f_op != &fscontext_fops)
		goto out_f;

	fc = f.file->private_data;

	switch (cmd) {
	case FSCONFIG_CMD_CREATE:
	case FSCONFIG_CMD_RECONFIGURE:
		if (_key || _value || aux)
			goto out_f;
		goto do_cmd;
	default:
		if (!_key)
			goto out_f;
		break;
	}

	param.key = strndup_user(_key, 256);
	if (IS_ERR(param.key)) {
		ret = PTR_ERR(param.key);
		goto out_f;
	}

	switch (cmd) {
	case FSCONFIG_SET_FLAG:
		if (_value || aux)
			goto out_clean;
		param.type = fs_value_is_flag;
		break;
	case FSCONFIG_SET_STRING:
		if (!_value || aux)
			goto out_clean;
		param.string = strndup_user(_value, PAGE_SIZE);
		if (IS_ERR(param.string)) {
			ret = PTR_ERR(param.string);
			param.string = NULL;
			goto out_clean;
		}
		param.size = strlen(param.string);
		param.type = fs_value_is_string;
		break;
	case FSCONFIG_SET_BINARY:
		if (!_value || aux <= 0 || aux > 1024 * 1024)
			goto out_clean;
		param.blob = memdup_user(_value, aux);
		if (IS_ERR(param.blob)) {
			ret = PTR_ERR(param.blob);
			param.blob = NULL;
			goto out_clean;
		}
		param.size = aux;
		param.type = fs_value_is_blob;
		break;
	case FSCONFIG_SET_PATH:
	case FSCONFIG_SET_PATH_EMPTY:
		if (!_value)
			goto out_clean;
		param.name = getname_flags(_value,
				cmd == FSCONFIG_SET_PATH_EMPTY ? LOOKUP_EMPTY : 0,
				NULL);
		if (IS_ERR(param.name)) {
			ret = PTR_ERR(param.name);
			param.name = NULL;
			goto out_clean;
		}
		param.dirfd = aux;
		param.type = (cmd == FSCONFIG_SET_PATH_EMPTY) ?
			fs_value_is_filename_empty : fs_value_is_filename;
		break;
	case FSCONFIG_SET_FD:
		if (_value || aux < 0)
			goto out_clean;
		param.file = fget(aux);
		if (!param.file) {
			ret = -EBADF;
			goto out_clean;
		}
		param.type = fs_value_is_file;
		break;
	}

do_cmd:
	ret = mutex_lock_interruptible(&fc->uapi_mutex);
	if (ret == 0) {
		ret = vfs_fsconfig_locked(fc, cmd, &param);
		mutex_unlock(&fc->uapi_mutex);
	}

out_clean:
	switch (param.type) {
	case fs_value_is_string:
	case fs_value_is_blob:
		kfree(param.string);
		break;
	case fs_value_is_filename:
	case fs_value_is_filename_empty:
		if (param.name)
			putname(param.name);
		break;
	case fs_value_is_file:
		if (param.file)
			fput(param.file);
		break;
	default:
		break;
	}
	kfree(param.key);
out_f:
	fdput(f);
	return ret;
}
