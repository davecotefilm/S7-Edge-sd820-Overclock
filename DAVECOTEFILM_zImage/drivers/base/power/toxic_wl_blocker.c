/*
 * Author: andip71, 28.08.2017
 *
 * Version 1.0.0
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
 * Change log:
 *
 * 1.0.0 (28.08.2017)
 *   - Initial version
 *
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/printk.h>


#define TOXIC_WL_BLOCKER_VERSION	"1.0.0"


/*****************************************/
// Variables
/*****************************************/

extern char list_wl[255];
extern char list_wl_search[257];
extern bool wl_blocker_active;
extern bool wl_blocker_debug;


/*****************************************/
// sysfs interface functions
/*****************************************/

static ssize_t wakelock_blocker_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	// return list of wakelocks to be blocked
	return sprintf(buf, "%s\n", list_wl);
}


static ssize_t wakelock_blocker_store(struct device * dev, struct device_attribute *attr,
			     const char * buf, size_t n)
{
	int len = n;

	// only strings up to 255 characters are allowed
	if (len > 255)
		return -EINVAL;

	// set flag if wakelock blocker should be active (for performance reasons)
	if (len > 1)
		wl_blocker_active = true;
	else
		wl_blocker_active = false;

	// store wakelock list and search string (with semicolons added at start and end)
	sscanf(buf, "%s", list_wl);
	sprintf(list_wl_search, ";%s;", list_wl);

	return n;
}


static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	// return current debug status
	return sprintf(buf, "Debug status: %d\n\nList: %s\nSearch list: %s\nActive: %d\n",
					wl_blocker_debug, list_wl, list_wl_search, wl_blocker_active);
}


static ssize_t debug_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	unsigned int ret = -EINVAL;
	unsigned int val;

	// check data and store if valid
	ret = sscanf(buf, "%d", &val);

	if (ret != 1)
		return -EINVAL;

	if (val == 1)
		wl_blocker_debug = true;
	else
		wl_blocker_debug = false;

	return count;
}


static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	// return version information
	return sprintf(buf, "%s\n", TOXIC_WL_BLOCKER_VERSION);
}



/*****************************************/
// Initialize sysfs objects
/*****************************************/

// define objects
static DEVICE_ATTR(wakelock_blocker, 0644, wakelock_blocker_show, wakelock_blocker_store);
static DEVICE_ATTR(debug, 0664, debug_show, debug_store);
static DEVICE_ATTR(version, 0664, version_show, NULL);

// define attributes
static struct attribute *toxic_wl_blocker_attributes[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL
};

// define attribute group
static struct attribute_group toxic_wl_blocker_control_group = {
	.attrs = toxic_wl_blocker_attributes,
};

// define control device
static struct miscdevice toxic_wl_blocker_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "toxic_wakelock_blocker",
};


/*****************************************/
// Driver init and exit functions
/*****************************************/

static int toxic_wl_blocker_init(void)
{
	// register toxic wakelock blocker control device
	misc_register(&toxic_wl_blocker_control_device);
	if (sysfs_create_group(&toxic_wl_blocker_control_device.this_device->kobj,
				&toxic_wl_blocker_control_group) < 0) {
		printk("toxic WL blocker: failed to create sys fs object.\n");
		return 0;
	}

	// Print debug info
	printk("toxic WL blocker: driver version %s started\n", TOXIC_WL_BLOCKER_VERSION);

	return 0;
}


static void toxic_wl_blocker_exit(void)
{
	// remove toxic wakelock blocker control device
	sysfs_remove_group(&toxic_wl_blocker_control_device.this_device->kobj,
                           &toxic_wl_blocker_control_group);

	// Print debug info
	printk("toxic WL blocker: driver stopped\n");
}


/* define driver entry points */
module_init(toxic_wl_blocker_init);
module_exit(toxic_wl_blocker_exit);
