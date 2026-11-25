// SPDX-License-Identifier: GPL-2.0
/*
 * Tachyon Kernel: core identity, author verification and anti-tamper interface
 *
 * "Beyond the Speed of Light"
 * Copyright (C) 2026 noxy <noxy@debian.noxy>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/export.h>
#include <linux/tachyon.h>

void tachyon_banner(void)
{
	pr_info("========================================================\n");
	pr_info("  %s: Official Build for %s\n", TACHYON_NAME, TACHYON_TARGET);
	pr_info("  Lead Developer & Architecture: %s\n", TACHYON_MAINTAINER);
	pr_info("  \"%s\" - For the sake of the experiment.\n", TACHYON_CODENAME);
	pr_info("  Official Releases: %s | Maintainer: %s\n", TACHYON_CHANNEL, TACHYON_AUTHOR);
	pr_info("========================================================\n");
}
EXPORT_SYMBOL(tachyon_banner);

int tachyon_validate_integrity(void)
{
	return TACHYON_MAGIC;
}
EXPORT_SYMBOL(tachyon_validate_integrity);

static int proc_tachyon_show(struct seq_file *m, void *v)
{
	seq_printf(m, "kernel: %s\n", TACHYON_NAME);
	seq_printf(m, "version: %s\n", TACHYON_VERSION);
	seq_printf(m, "codename: %s\n", TACHYON_CODENAME);
	seq_printf(m, "author: %s\n", TACHYON_AUTHOR);
	seq_printf(m, "maintainer: %s\n", TACHYON_MAINTAINER);
	seq_printf(m, "target: %s\n", TACHYON_TARGET);
	seq_printf(m, "channel: %s\n", TACHYON_CHANNEL);
	return 0;
}

static int proc_tachyon_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_tachyon_show, NULL);
}

static const struct file_operations proc_tachyon_fops = {
	.open = proc_tachyon_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init tachyon_proc_init(void)
{
	proc_create("tachyon", 0444, NULL, &proc_tachyon_fops);
	return 0;
}
fs_initcall(tachyon_proc_init);
