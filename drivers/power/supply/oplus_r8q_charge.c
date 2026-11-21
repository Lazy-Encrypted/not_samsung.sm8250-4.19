// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only OxygenOS battery attributes backed by the Samsung battery driver.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/power_supply.h>

static struct class *oplus_chg_class;
static struct device *oplus_battery;

static ssize_t oplus_battery_read(enum power_supply_property prop, char *buf)
{
	struct power_supply *psy;
	union power_supply_propval value;
	int ret;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, prop, &value);
	power_supply_put(psy);
	if (ret)
		return ret;

	/* The OxygenOS gauge ABI reports mV and mAh. */
	return scnprintf(buf, PAGE_SIZE, "%d\n", value.intval / 1000);
}

static ssize_t gauge_vbat_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return oplus_battery_read(POWER_SUPPLY_PROP_VOLTAGE_NOW, buf);
}
static DEVICE_ATTR_RO(gauge_vbat);

static ssize_t battery_fcc_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return oplus_battery_read(POWER_SUPPLY_PROP_CHARGE_FULL, buf);
}
static DEVICE_ATTR_RO(battery_fcc);

static struct attribute *oplus_battery_attrs[] = {
	&dev_attr_gauge_vbat.attr,
	&dev_attr_battery_fcc.attr,
	NULL,
};
ATTRIBUTE_GROUPS(oplus_battery);

static int __init oplus_r8q_charge_init(void)
{
	int ret;

	oplus_chg_class = class_create(THIS_MODULE, "oplus_chg");
	if (IS_ERR(oplus_chg_class))
		return PTR_ERR(oplus_chg_class);

	oplus_battery = device_create_with_groups(oplus_chg_class, NULL, 0,
						  NULL, oplus_battery_groups,
						  "battery");
	if (IS_ERR(oplus_battery)) {
		ret = PTR_ERR(oplus_battery);
		class_destroy(oplus_chg_class);
		return ret;
	}

	return 0;
}
module_init(oplus_r8q_charge_init);

static void __exit oplus_r8q_charge_exit(void)
{
	device_unregister(oplus_battery);
	class_destroy(oplus_chg_class);
}
module_exit(oplus_r8q_charge_exit);

MODULE_DESCRIPTION("OxygenOS battery telemetry for Samsung r8q");
MODULE_LICENSE("GPL");
