#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <media/media-device.h>

#include "mxc_media_dev.h"

#if defined(CONFIG_MEDIA_CONTROLLER)
static DEFINE_MUTEX(mxc_md_lock);
static struct media_device mxc_md;
static unsigned int mxc_md_users;
static bool mxc_md_initialized;
static bool mxc_md_registered;

struct media_device *mxc_get_shared_media_device(struct device *dev)
{
	struct media_device *mdev;

	mutex_lock(&mxc_md_lock);

	if (!mxc_md_initialized) {
		media_device_init(&mxc_md);
		strlcpy(mxc_md.model, "i.MX MXC Media",
			sizeof(mxc_md.model));
		strlcpy(mxc_md.driver_name, "mxc-media",
			sizeof(mxc_md.driver_name));
		snprintf(mxc_md.bus_info, sizeof(mxc_md.bus_info),
			 "platform:%s", dev ? dev_name(dev) : "mxc-media");
		mxc_md.dev = dev;
		mxc_md_initialized = true;
	} else if (!mxc_md.dev) {
		mxc_md.dev = dev;
	}

	mxc_md_users++;
	mdev = &mxc_md;

	mutex_unlock(&mxc_md_lock);
	return mdev;
}
EXPORT_SYMBOL_GPL(mxc_get_shared_media_device);

int mxc_register_shared_media_device(struct media_device *mdev)
{
	int ret = 0;

	if (mdev != &mxc_md)
		return -EINVAL;

	mutex_lock(&mxc_md_lock);

	if (!mxc_md_initialized) {
		ret = -ENODEV;
		goto out;
	}

	if (!mxc_md_registered) {
		ret = media_device_register(&mxc_md);
		if (!ret)
			mxc_md_registered = true;
	}

out:
	mutex_unlock(&mxc_md_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mxc_register_shared_media_device);

void mxc_put_shared_media_device(struct media_device *mdev)
{
	if (mdev != &mxc_md)
		return;

	mutex_lock(&mxc_md_lock);

	if (!mxc_md_users)
		goto out;

	mxc_md_users--;
	if (mxc_md_users)
		goto out;

	if (mxc_md_registered) {
		media_device_unregister(&mxc_md);
		mxc_md_registered = false;
	}

	media_device_cleanup(&mxc_md);
	mxc_md_initialized = false;
	mxc_md.dev = NULL;

out:
	mutex_unlock(&mxc_md_lock);
}
EXPORT_SYMBOL_GPL(mxc_put_shared_media_device);
#endif
