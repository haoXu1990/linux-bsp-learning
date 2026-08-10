#ifndef _MXC_MEDIA_DEV_H
#define _MXC_MEDIA_DEV_H

#include <media/media-device.h>

struct device;

#if defined(CONFIG_MEDIA_CONTROLLER)
struct media_device *mxc_get_shared_media_device(struct device *dev);
int mxc_register_shared_media_device(struct media_device *mdev);
void mxc_put_shared_media_device(struct media_device *mdev);
#endif

#endif
