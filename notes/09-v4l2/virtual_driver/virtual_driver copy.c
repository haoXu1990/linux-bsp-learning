/*
 *  V4L2 还是比较复杂， 这里再次记录下实现流程
 *
 * 先按三层模型理解本文件：
 *   - APP 层：open/ioctl/mmap/poll，负责交空 buffer、取满 buffer；
 *   - V4L2 Core + vb2 层：video_ioctl2 分发 ioctl，vb2 管 buffer 状态；
 *   - 驱动层：回答格式能力、接收空 buffer、模拟硬件产帧、通知完成。
 *
 * 主流程：
 *
 *   1. 注册 video_device，生成 /dev/videoX
 *   2. APP 通过 video_ioctl2 进入 v4l2_ioctl_ops
 *   3. 格式/能力类 ioctl 由本驱动回答
 *   4. buffer 类 ioctl 交给 videobuf2(vb2) 通用代码
 *   5. APP QBUF 后，vb2 调用 virtual_buf_queue，把空 buffer 交给驱动
 *   6. APP STREAMON 后，本驱动启动 timer 模拟“硬件开始采集”
 *   7. timer 到期模拟“一帧完成”，填充 buffer 并调用 vb2_buffer_done
 *   8. APP DQBUF 从 vb2 取回完成帧
 *   9. APP STREAMOFF 时，停止 timer，并归还驱动仍持有的 buffer
 *
 * 可以把整个模型记成一句话：
 *
 *   APP QBUF 交空桶 -> 驱动填满 -> vb2_buffer_done 通知桶满
 *   -> APP DQBUF 取满桶 -> 再 QBUF 循环使用。
 * 这部分再文档中也有总结
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define VIRTUAL_WIDTH        800
#define VIRTUAL_HEIGHT       600
#define VIRTUAL_PIXELFORMAT  V4L2_PIX_FMT_YUYV
#define VIRTUAL_IMAGE_SIZE   (VIRTUAL_WIDTH * VIRTUAL_HEIGHT * 2)
#define VIRTUAL_BUFFER_SIZE  PAGE_ALIGN(VIRTUAL_IMAGE_SIZE)
#define VIRTUAL_FPS          30

/*
 * 驱动自己的 buffer 包装。
 *
 * vb2 认识的是 struct vb2_buffer / struct vb2_v4l2_buffer；
 * 驱动还需要把“已经 QBUF、等待硬件填充”的 buffer 挂到自己的链表，
 * 所以在 vb2_v4l2_buffer 外面包一层 struct my_buffer。
 */
struct my_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/*
 * 老师示例里很多对象是全局变量；这里改成标准驱动常见写法：
 * 把 v4l2_device、video_device、vb2_queue、锁、buffer 链表、timer
 * 都收进设备私有结构体。
 *
 * 这样各个回调可以用两条路径找回设备对象：
 *
 *   ioctl 回调：struct my_video *dev = video_drvdata(file);
 *   vb2  回调：struct my_video *dev = vb2_get_drv_priv(q);
 *
 * timer 回调在 Linux 4.9 中通过 setup_timer 的 data 参数拿回 dev。
 */
struct my_video {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb_queue;

	struct mutex queue_lock;
	spinlock_t qlock;
	struct list_head active;

	struct timer_list virtual_timer;

	u32 width;
	u32 height;
	u32 pixelformat;
	u32 bytesperline;
	u32 sizeimage;

	unsigned int sequence;
	bool streaming;
};


static struct my_video *g_my_video;


static void virtual_fill_format(struct v4l2_pix_format *pix)
{
	pix->width = VIRTUAL_WIDTH;
	pix->height = VIRTUAL_HEIGHT;
	pix->pixelformat = VIRTUAL_PIXELFORMAT;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = VIRTUAL_WIDTH * 2;
	pix->sizeimage = VIRTUAL_IMAGE_SIZE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
}

// APP 第一步通常会查询设备能力。这里告诉 APP
static int virtual_querycap(struct file *file, void *fh,
				    struct v4l2_capability *cap)
{
	strlcpy(cap->driver, "100ask_virtual", sizeof(cap->driver));
	strlcpy(cap->card, "100ask virtual camera", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:100ask_virtual",
		sizeof(cap->bus_info));

	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE |
			   V4L2_CAP_STREAMING |
			   V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

// APP 用 index 枚举驱动支持的像素格式。
static int virtual_enum_fmt_cap(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = VIRTUAL_PIXELFORMAT;
	strlcpy(f->description, "YUYV 4:2:2", sizeof(f->description));

	return 0;
}

/*
 * VIDIOC_G_FMT
 *
 * APP 查询当前格式。很多应用会先 G_FMT，再根据结果决定是否 S_FMT，
 * 所以即使格式固定，也应该提供这个回调。
 */
static int virtual_g_fmt_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_format(&f->fmt.pix);
	return 0;
}

/*
 * VIDIOC_TRY_FMT
 *
 * TRY_FMT 的语义是“试一下这个格式能不能支持，但不要真正改变硬件状态”。
 * 当前驱动只有一种固定格式，所以无论 APP 传什么，都修正成 800x600 YUYV。
 */
static int virtual_try_fmt_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* 当前第一版只支持一种固定格式，所以把 APP 参数修正成最接近值。 */
	virtual_fill_format(&f->fmt.pix);
	return 0;
}

/*
 * VIDIOC_S_FMT
 *
 * S_FMT 会真正更新驱动记录的格式状态。
 * 如果 APP 已经 REQBUFS 或开始 streaming，vb2_is_busy() 为真，此时不允许
 * 改格式，否则已经分配好的 buffer 大小和新格式可能对不上。
 */
static int virtual_s_fmt_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct my_video *dev = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	ret = virtual_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	dev->width = f->fmt.pix.width;
	dev->height = f->fmt.pix.height;
	dev->pixelformat = f->fmt.pix.pixelformat;
	dev->bytesperline = f->fmt.pix.bytesperline;
	dev->sizeimage = f->fmt.pix.sizeimage;

	return 0;
}

static int virtual_enum_framesizes(struct file *file, void *fh,
				   struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;

	if (fsize->pixel_format != VIRTUAL_PIXELFORMAT)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = VIRTUAL_WIDTH;
	fsize->discrete.height = VIRTUAL_HEIGHT;

	return 0;
}

/*
 * vb2_ops.queue_setup
 *
 * APP 调 VIDIOC_REQBUFS / VIDIOC_CREATE_BUFS 时，vb2 在真正分配内存前
 * 会回调这里询问驱动：
 *
 *   - 至少需要几个 buffer？
 *   - 每个 buffer 有几个 plane？
 *   - 每个 plane 需要多大？
 *
 * 这个函数只回答“桶怎么分配”，不启动硬件，也不填图像数据。
 */
static int virtual_queue_setup(struct vb2_queue *vq,
			       unsigned int *num_buffers,
			       unsigned int *num_planes,
			       unsigned int sizes[],
			       struct device *alloc_devs[])
{
	/* APP 申请 buffer 前，vb2 会问驱动：需要几个桶、每个桶多大。 */
	if (vq->num_buffers + *num_buffers < 8)
		*num_buffers = 8 - vq->num_buffers;

	if (*num_planes) {
		if (sizes[0] < VIRTUAL_BUFFER_SIZE)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = VIRTUAL_BUFFER_SIZE;

	return 0;
}

/*
 * vb2_ops.buf_queue
 *
 * APP 调 VIDIOC_QBUF 后，vb2 完成通用检查和状态转换，然后调用这里。
 * 到这里时可以理解为：APP 已经把一个“空 buffer”交给驱动。
 *
 * 虚拟驱动没有 DMA，所以只是把 buffer 加入 dev->active 链表。
 * 真实硬件驱动通常会在这里把 DMA 地址交给硬件，或者加入硬件待处理队列。
 */
static void virtual_buf_queue(struct vb2_buffer *vb)
{
	struct my_video *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct my_buffer *buf = container_of(vbuf, struct my_buffer, vb);
	unsigned long flags;

	/* QBUF 的本质：APP 把一个空 buffer 交给驱动。 */
	spin_lock_irqsave(&dev->qlock, flags);
	list_add_tail(&buf->list, &dev->active);
	spin_unlock_irqrestore(&dev->qlock, flags);
}

/*
 * 从驱动持有的空 buffer 队列中取出一个。
 *
 * dev->active 可能同时被 QBUF 路径和 timer 路径访问：
 *   - QBUF 路径把 buffer 加进去；
 *   - timer 路径取 buffer 出来填帧。
 * 所以这里用 spinlock 保护链表。
 */
static struct my_buffer *virtual_get_next_buf(struct my_video *dev)
{
	struct my_buffer *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	if (!list_empty(&dev->active)) {
		buf = list_first_entry(&dev->active, struct my_buffer, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);

	return buf;
}

/*
 * 生成一帧假的 YUYV 图像。
 *
 * YUYV 每 4 个字节表示两个像素：Y0 U Y1 V。
 * 这里不追求真实图像，只生成纯色块，并每秒改变一次颜色，方便确认
 * APP 看到的画面在持续更新。
 */
static void virtual_fill_yuyv_frame(void *dst, unsigned int sequence)
{
	u8 *p = dst;
	u8 y, u, v;
	unsigned int i;

	/* 每秒换一种色调，方便肉眼确认帧在更新。 */
	switch ((sequence / VIRTUAL_FPS) % 3) {
	case 0:
		y = 0x50;
		u = 0x40;
		v = 0xf0;
		break;
	case 1:
		y = 0x80;
		u = 0x80;
		v = 0x80;
		break;
	default:
		y = 0xc0;
		u = 0xf0;
		v = 0x40;
		break;
	}

	for (i = 0; i < VIRTUAL_IMAGE_SIZE; i += 4) {
		p[i + 0] = y;
		p[i + 1] = u;
		p[i + 2] = y;
		p[i + 3] = v;
	}
}

/*
 * timer 回调：模拟“硬件产生了一帧”。
 *
 * 真实摄像头驱动里，这个位置通常对应：
 *   - DMA 完成中断
 *   - CSI/MIPI frame done 中断
 *   - USB URB complete 回调
 *
 * 本虚拟驱动用 timer 代替硬件中断。核心动作是：
 *   1. 从 dev->active 取一个 APP 交来的空 buffer；
 *   2. 获取 buffer 的内核虚拟地址；
 *   3. 填入一帧 YUYV 数据；
 *   4. 设置 payload，也就是 APP DQBUF 后看到的 bytesused；
 *   5. 调用 vb2_buffer_done(DONE)，通知 vb2 这帧完成。
 */
static void virtual_timer_expire(unsigned long data)
{
	struct my_video *dev = (struct my_video *)data;
	struct my_buffer *buf;
	void *ptr;

	if (!dev->streaming)
		return;

	buf = virtual_get_next_buf(dev);
	if (buf) {
		ptr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
		if (ptr) {
			virtual_fill_yuyv_frame(ptr, dev->sequence);
			vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
					      VIRTUAL_IMAGE_SIZE);
			buf->vb.sequence = dev->sequence++;
			buf->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_DONE);
		} else {
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
		}
	}

	if (dev->streaming)
		mod_timer(&dev->virtual_timer, jiffies + HZ / VIRTUAL_FPS);
}

/*
 * vb2_ops.start_streaming
 *
 * APP 调 VIDIOC_STREAMON 后，vb2 会调用这里。
 * 对真实硬件来说，这里应该启动 sensor/CSI/DMA/USB 传输。
 * 对本虚拟驱动来说，这里启动 timer，让 timer 周期性地产生假帧。
 */
static int virtual_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct my_video *dev = vb2_get_drv_priv(q);

	dev->sequence = 0;
	dev->streaming = true;

	/* Linux 4.9 风格：timer 回调参数直接传 my_video。 */
	setup_timer(&dev->virtual_timer, virtual_timer_expire,
		    (unsigned long)dev);
	mod_timer(&dev->virtual_timer, jiffies + HZ / VIRTUAL_FPS);

	return 0;
}

/*
 * 把驱动仍然持有的 buffer 全部还给 vb2。
 *
 * STREAMOFF 或启动失败时必须做这件事。只要 APP QBUF 后 buffer 进入了
 * 驱动，驱动最终就必须用 vb2_buffer_done() 把它还回去。
 * 未完成的 buffer 用 VB2_BUF_STATE_ERROR，表示“没有有效图像，但生命周期结束”。
 */
static void virtual_return_all_buffers(struct my_video *dev,
				       enum vb2_buffer_state state)
{
	struct my_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	while (!list_empty(&dev->active)) {
		buf = list_first_entry(&dev->active, struct my_buffer, list);
		list_del(&buf->list);
		spin_unlock_irqrestore(&dev->qlock, flags);

		vb2_buffer_done(&buf->vb.vb2_buf, state);

		spin_lock_irqsave(&dev->qlock, flags);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);
}

/*
 * vb2_ops.stop_streaming
 *
 * APP 调 VIDIOC_STREAMOFF，或关闭设备导致停止 streaming 时，vb2 会调用这里。
 * 这里要停止“硬件”，并归还所有仍在驱动队列里的 buffer。
 */
static void virtual_stop_streaming(struct vb2_queue *q)
{
	struct my_video *dev = vb2_get_drv_priv(q);

	dev->streaming = false;
	del_timer_sync(&dev->virtual_timer);
	virtual_return_all_buffers(dev, VB2_BUF_STATE_ERROR);
}

/*
 * 这组 ops 是驱动交给 vb2 的“硬件相关回调”。
 *
 * 可以这样记：
 *   queue_setup     ：REQBUFS 时问驱动 buffer 怎么分配；
 *   buf_queue       ：QBUF 时把空 buffer 交给驱动；
 *   start_streaming ：STREAMON 时启动硬件/虚拟 timer；
 *   stop_streaming  ：STREAMOFF 时停止硬件并归还 buffer。
 */
static const struct vb2_ops virtual_vb2_ops = {
	.queue_setup = virtual_queue_setup,
	.buf_queue = virtual_buf_queue,
	.start_streaming = virtual_start_streaming,
	.stop_streaming = virtual_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/*
 * file_operations 是 /dev/videoX 的第一层入口。
 *
 * 这里大部分操作直接复用 V4L2/vb2 通用函数：
 *   - video_ioctl2 负责把 ioctl 分发到 virtual_ioctl_ops；
 *   - vb2_fop_mmap/read/poll/release 负责通用 buffer 行为。
 */
static const struct v4l2_file_operations virtual_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/*
 * V4L2 ioctl 操作表。
 *
 * 上半部分是“设备能力/格式”问题，必须由驱动自己回答；
 * 下半部分是“buffer 生命周期”问题，交给 vb2 通用 ioctl，再由 vb2
 * 在关键节点反调 virtual_vb2_ops。
 */
static const struct v4l2_ioctl_ops virtual_ioctl_ops = {
	.vidioc_querycap = virtual_querycap,
	.vidioc_enum_fmt_vid_cap = virtual_enum_fmt_cap,
	.vidioc_g_fmt_vid_cap = virtual_g_fmt_cap,
	.vidioc_s_fmt_vid_cap = virtual_s_fmt_cap,
	.vidioc_try_fmt_vid_cap = virtual_try_fmt_cap,
	.vidioc_enum_framesizes = virtual_enum_framesizes,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static void virtual_video_release(struct v4l2_device *v4l2_dev)
{
}

/*
 * 模块加载入口：创建并注册一个虚拟摄像头。
 *
 * 初始化顺序很重要：
 *   1. 分配并初始化 struct my_video；
 *   2. 初始化锁、active 链表和默认格式；
 *   3. 初始化 vb2_queue，并把 drv_priv 指向 my_video；
 *   4. 注册 v4l2_device；
 *   5. 填充 video_device，关联 fops/ioctl_ops/vb2_queue/v4l2_device；
 *   6. video_register_device() 生成 /dev/videoX。
 */
static int __init virtual_video_drv_init(void)
{
	struct my_video *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->queue_lock);
	spin_lock_init(&dev->qlock);
	INIT_LIST_HEAD(&dev->active);

	dev->width = VIRTUAL_WIDTH;
	dev->height = VIRTUAL_HEIGHT;
	dev->pixelformat = VIRTUAL_PIXELFORMAT;
	dev->bytesperline = VIRTUAL_WIDTH * 2;
	dev->sizeimage = VIRTUAL_IMAGE_SIZE;

	dev->vb_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->vb_queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	dev->vb_queue.drv_priv = dev;
	dev->vb_queue.buf_struct_size = sizeof(struct my_buffer);
	dev->vb_queue.ops = &virtual_vb2_ops;
	dev->vb_queue.mem_ops = &vb2_vmalloc_memops;
	dev->vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	dev->vb_queue.lock = &dev->queue_lock;

	ret = vb2_queue_init(&dev->vb_queue);
	if (ret) {
		pr_err("100ask_virtual: vb2_queue_init failed: %d\n", ret);
		goto err_free_dev;
	}

	dev->v4l2_dev.release = virtual_video_release;
	strlcpy(dev->v4l2_dev.name, "100ask_virtual",
		sizeof(dev->v4l2_dev.name));
	ret = v4l2_device_register(NULL, &dev->v4l2_dev);
	if (ret) {
		pr_err("100ask_virtual: v4l2_device_register failed: %d\n",
		       ret);
		goto err_free_dev;
	}

	strlcpy(dev->vdev.name, "100ask_virtual_video",
		sizeof(dev->vdev.name));
	dev->vdev.release = video_device_release_empty;
	dev->vdev.fops = &virtual_fops;
	dev->vdev.ioctl_ops = &virtual_ioctl_ops;
	dev->vdev.queue = &dev->vb_queue;
	dev->vdev.lock = &dev->queue_lock;
	dev->vdev.v4l2_dev = &dev->v4l2_dev;
	video_set_drvdata(&dev->vdev, dev);

	ret = video_register_device(&dev->vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		pr_err("100ask_virtual: video_register_device failed: %d\n",
		       ret);
		goto err_unreg_v4l2;
	}

	g_my_video = dev;
	pr_info("100ask_virtual: registered /dev/video%d\n", dev->vdev.num);
	return 0;

err_unreg_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_free_dev:
	kfree(dev);
	return ret;
}

/*
 * 模块卸载入口：按注册的反方向释放资源。
 */
static void __exit virtual_video_drv_exit(void)
{
	struct my_video *dev = g_my_video;

	if (!dev)
		return;

	video_unregister_device(&dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);
	kfree(dev);
	g_my_video = NULL;
}

module_init(virtual_video_drv_init);
module_exit(virtual_video_drv_exit);

MODULE_DESCRIPTION("100ask virtual V4L2 video driver");
MODULE_AUTHOR("100ask");
MODULE_LICENSE("GPL");
