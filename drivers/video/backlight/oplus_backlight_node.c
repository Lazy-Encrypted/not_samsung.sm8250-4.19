// SPDX-License-Identifier: GPL-2.0
/* ColorOS brightness compatibility interface for the primary panel. */
#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define OPLUS_BL_MAX 8191U
#define OPLUS_BL_DEVICE "panel0-backlight"

static struct kobject *touchpanel_kobj;

static ssize_t oplus_backlight_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct backlight_device *bd;
	int brightness, max_brightness;
	u64 value;

	bd = backlight_device_get_by_name(OPLUS_BL_DEVICE);
	if (!bd)
		return -ENODEV;

	max_brightness = READ_ONCE(bd->props.max_brightness);
	brightness = READ_ONCE(bd->props.brightness);
	put_device(&bd->dev);

	if (max_brightness <= 0)
		return -EINVAL;

	brightness = clamp(brightness, 0, max_brightness);
	value = (u64)brightness * OPLUS_BL_MAX;
	do_div(value, max_brightness);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", (unsigned long long)value);
}

static ssize_t oplus_backlight_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	struct backlight_device *bd;
	unsigned int value;
	int max_brightness, ret;
	u64 scaled;
	char input[32];
	size_t len = count;

	if (!len || len >= sizeof(input) || memchr(buf, '\0', len))
		return -EINVAL;

	memcpy(input, buf, len);
	input[len] = '\0';
	if (input[len - 1] == '\n')
		input[--len] = '\0';

	/* ColorOS may write an integral value with a .0 suffix. */
	if (len >= 2 && input[len - 2] == '.' && input[len - 1] == '0')
		input[len - 2] = '\0';

	if (kstrtouint(input, 10, &value))
		return -EINVAL;
	value = min(value, OPLUS_BL_MAX);

	bd = backlight_device_get_by_name(OPLUS_BL_DEVICE);
	if (!bd)
		return -ENODEV;

	max_brightness = READ_ONCE(bd->props.max_brightness);
	if (max_brightness <= 0) {
		ret = -EINVAL;
		goto out;
	}

	scaled = (u64)value * max_brightness;
	do_div(scaled, OPLUS_BL_MAX);
	if (value && !scaled)
		scaled = 1;

	ret = backlight_device_set_brightness(bd, (unsigned long)scaled);
out:
	put_device(&bd->dev);
	if (ret)
		return ret;
	return count;
}

static struct kobj_attribute oplus_backlight_attr =
	__ATTR(oplus_backlight, 0664, oplus_backlight_show, oplus_backlight_store);

static int __init oplus_backlight_node_init(void)
{
	int ret;

	touchpanel_kobj = kobject_create_and_add("touchpanel", NULL);
	if (!touchpanel_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(touchpanel_kobj, &oplus_backlight_attr.attr);
	if (ret) {
		kobject_put(touchpanel_kobj);
		touchpanel_kobj = NULL;
		return ret;
	}

	pr_info("oplus_backlight: registered successfully\n");
	return 0;
}
device_initcall(oplus_backlight_node_init);
