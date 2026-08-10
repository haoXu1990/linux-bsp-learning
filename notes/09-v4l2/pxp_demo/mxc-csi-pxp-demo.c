/*
 * Minimal i.MX6ULL OV5640 -> CSI -> userspace copy -> PXP -> LCD demo.
 *
 * This sample is intentionally simple:
 * 1. Optionally configure the OV5640 and CSI subdev pads.
 * 2. Capture frames from the CSI video node with MMAP buffers.
 * 3. Copy each captured frame into the PXP VIDEO_OUTPUT queue.
 * 4. Let PXP process the frame and display it on the LCD framebuffer.
 *
 * The CSI and PXP blocks are not wired as a direct hardware pipeline in the
 * current driver model, so userspace must bridge them through memory.
 *
 * Build example:
 *   arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -o mxc-csi-pxp-demo \
 *       tools/mxc-csi-pxp-demo.c
 *
 * Run example:
 *   ./mxc-csi-pxp-demo \
 *       --capture /dev/video1 \
 *       --pxp /dev/video0 \
 *       --sensor-subdev /dev/v4l-subdev0 \
 *       --csi-subdev /dev/v4l-subdev1 \
 *       --width 640 --height 480 --frames 300 --pixfmt YUYV
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define OV5640_PAD_SOURCE 0
#define MX6S_CSI_PAD_SINK 0
#define MX6S_CSI_PAD_SOURCE 1

#define DEFAULT_CAPTURE_DEV "/dev/video1"
#define DEFAULT_PXP_DEV "/dev/video0"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_FRAMES 0
#define DEFAULT_PXP_OUTPUT 0
#define DEFAULT_FB_NODE (-2)

#define PXP_IOC_MAGIC 'P'
struct pxp_video_output_fb {
	int32_t fb_index;
	uint32_t reserved[3];
};

#define PXP_IOC_V4L2_S_OUTPUT_FB \
	_IOW(PXP_IOC_MAGIC, 8, struct pxp_video_output_fb)

struct mapped_buffer {
	void *start;
	size_t length;
};

struct video_queue {
	int fd;
	enum v4l2_buf_type type;
	enum v4l2_memory memory;
	struct mapped_buffer *buffers;
	unsigned int count;
};

struct demo_config {
	const char *capture_dev;
	const char *pxp_dev;
	const char *sensor_subdev;
	const char *csi_subdev;
	unsigned int width;
	unsigned int height;
	unsigned int display_width;
	unsigned int display_height;
	unsigned int frames;
	unsigned int pxp_output;
	int fb_node;
	uint32_t pixfmt;
	uint32_t mbus_code;
};

static volatile sig_atomic_t g_running = 1;

static const struct option demo_options[] = {
	{ "capture", required_argument, NULL, 'c' },
	{ "pxp", required_argument, NULL, 'p' },
	{ "sensor-subdev", required_argument, NULL, 's' },
	{ "csi-subdev", required_argument, NULL, 'i' },
	{ "width", required_argument, NULL, 'w' },
	{ "height", required_argument, NULL, 'h' },
	{ "display-width", required_argument, NULL, 'W' },
	{ "display-height", required_argument, NULL, 'H' },
	{ "frames", required_argument, NULL, 'n' },
	{ "pixfmt", required_argument, NULL, 'f' },
	{ "pxp-output", required_argument, NULL, 'o' },
	{ "fb", required_argument, NULL, 'b' },
	{ "help", no_argument, NULL, '?' },
	{ NULL, 0, NULL, 0 }
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR && g_running);

	return ret;
}

static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static void fourcc_to_string(uint32_t fourcc, char out[5])
{
	out[0] = fourcc & 0xff;
	out[1] = (fourcc >> 8) & 0xff;
	out[2] = (fourcc >> 16) & 0xff;
	out[3] = (fourcc >> 24) & 0xff;
	out[4] = '\0';
}

static uint32_t mbus_code_from_pixfmt(uint32_t pixfmt)
{
	switch (pixfmt) {
	case V4L2_PIX_FMT_UYVY:
		return MEDIA_BUS_FMT_UYVY8_2X8;
	case V4L2_PIX_FMT_YUYV:
		return MEDIA_BUS_FMT_YUYV8_2X8;
	case V4L2_PIX_FMT_YVYU:
		return MEDIA_BUS_FMT_YVYU8_2X8;
	default:
		return 0;
	}
}

static uint32_t parse_fourcc(const char *name)
{
	if (!strcasecmp(name, "UYVY"))
		return V4L2_PIX_FMT_UYVY;
	if (!strcasecmp(name, "YUYV"))
		return V4L2_PIX_FMT_YUYV;
	if (!strcasecmp(name, "YVYU"))
		return V4L2_PIX_FMT_YVYU;
	if (!strcasecmp(name, "RGB3"))
		return V4L2_PIX_FMT_RGB24;
	if (!strcasecmp(name, "RGBP"))
		return V4L2_PIX_FMT_RGB565;

	if (strlen(name) == 4)
		return v4l2_fourcc(name[0], name[1], name[2], name[3]);

	return 0;
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --capture DEV         CSI capture video node (default %s)\n"
		"  --pxp DEV             PXP video node (default %s)\n"
		"  --sensor-subdev DEV   optional OV5640 subdev node\n"
		"  --csi-subdev DEV      optional CSI subdev node\n"
		"  --width N             capture width (default %u)\n"
		"  --height N            capture height (default %u)\n"
		"  --display-width N     LCD destination width (default width)\n"
		"  --display-height N    LCD destination height (default height)\n"
		"  --frames N            frame count, 0 means endless (default endless)\n"
		"  --pixfmt FOURCC       UYVY/YUYV/YVYU... (default YUYV)\n"
		"  --pxp-output N        0=LCD, 1=virtual output (default %u)\n"
		"  --fb N                target /dev/fbN, -1=driver auto-select\n",
		prog, DEFAULT_CAPTURE_DEV, DEFAULT_PXP_DEV,
		DEFAULT_WIDTH, DEFAULT_HEIGHT,
		DEFAULT_PXP_OUTPUT);
}

static int open_device(const char *path, int flags)
{
	int fd = open(path, flags);

	if (fd < 0)
		fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));

	return fd;
}

static int set_subdev_pad_format(const char *devnode, uint32_t pad,
				 uint32_t width, uint32_t height,
				 uint32_t mbus_code)
{
	struct v4l2_subdev_format fmt;
	int fd;

	fd = open_device(devnode, O_RDWR);
	if (fd < 0)
		return -1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = pad;
	fmt.format.width = width;
	fmt.format.height = height;
	fmt.format.code = mbus_code;
	fmt.format.field = V4L2_FIELD_NONE;
	fmt.format.colorspace = V4L2_COLORSPACE_JPEG;

	if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0) {
		fprintf(stderr, "%s: VIDIOC_SUBDEV_S_FMT pad %u failed: %s\n",
			devnode, pad, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int configure_optional_subdevs(const struct demo_config *cfg)
{
	if (!cfg->mbus_code) {
		fprintf(stderr, "Unsupported pixfmt for subdev setup\n");
		return -1;
	}

	if (cfg->sensor_subdev) {
		if (set_subdev_pad_format(cfg->sensor_subdev, OV5640_PAD_SOURCE,
					  cfg->width, cfg->height,
					  cfg->mbus_code) < 0)
			return -1;
	}

	if (cfg->csi_subdev) {
		if (set_subdev_pad_format(cfg->csi_subdev, MX6S_CSI_PAD_SINK,
					  cfg->width, cfg->height,
					  cfg->mbus_code) < 0)
			return -1;
		if (set_subdev_pad_format(cfg->csi_subdev, MX6S_CSI_PAD_SOURCE,
					  cfg->width, cfg->height,
					  cfg->mbus_code) < 0)
			return -1;
	}

	return 0;
}

static int set_capture_format(int fd, uint32_t width, uint32_t height,
			      uint32_t pixfmt)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "CSI VIDIOC_S_FMT failed: %s\n", strerror(errno));
		return -1;
	}

	if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height ||
	    fmt.fmt.pix.pixelformat != pixfmt) {
		char req[5], got[5];

		fourcc_to_string(pixfmt, req);
		fourcc_to_string(fmt.fmt.pix.pixelformat, got);
		fprintf(stderr,
			"CSI adjusted format to %ux%u %s (requested %ux%u %s)\n",
			fmt.fmt.pix.width, fmt.fmt.pix.height, got,
			width, height, req);
	}

	return 0;
}

static int set_pxp_output(int fd, unsigned int output)
{
	if (xioctl(fd, VIDIOC_S_OUTPUT, &output) < 0) {
		fprintf(stderr, "PXP VIDIOC_S_OUTPUT failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int set_pxp_fb_node(int fd, int fb_node)
{
	struct pxp_video_output_fb fb_cfg;

	memset(&fb_cfg, 0, sizeof(fb_cfg));
	fb_cfg.fb_index = fb_node;

	if (xioctl(fd, PXP_IOC_V4L2_S_OUTPUT_FB, &fb_cfg) < 0) {
		fprintf(stderr, "PXP set output framebuffer failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int set_pxp_input_format(int fd, uint32_t width, uint32_t height,
				uint32_t pixfmt)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "PXP VIDIOC_S_FMT(VIDEO_OUTPUT) failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int set_pxp_display_window(int fd, uint32_t src_w, uint32_t src_h,
				  uint32_t dst_w, uint32_t dst_h)
{
	struct v4l2_format overlay_fmt;
	struct v4l2_crop crop;

	memset(&overlay_fmt, 0, sizeof(overlay_fmt));
	overlay_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
	overlay_fmt.fmt.win.w.left = 0;
	overlay_fmt.fmt.win.w.top = 0;
	overlay_fmt.fmt.win.w.width = src_w;
	overlay_fmt.fmt.win.w.height = src_h;
	overlay_fmt.fmt.win.field = V4L2_FIELD_NONE;
	overlay_fmt.fmt.win.global_alpha = 0xff;

	if (xioctl(fd, VIDIOC_S_FMT, &overlay_fmt) < 0) {
		fprintf(stderr,
			"PXP VIDIOC_S_FMT(VIDEO_OUTPUT_OVERLAY) failed: %s\n",
			strerror(errno));
		return -1;
	}

	memset(&crop, 0, sizeof(crop));
	crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
	crop.c.left = 0;
	crop.c.top = 0;
	crop.c.width = dst_w;
	crop.c.height = dst_h;

	if (xioctl(fd, VIDIOC_S_CROP, &crop) < 0) {
		fprintf(stderr, "PXP VIDIOC_S_CROP failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int request_mmap_buffers(struct video_queue *queue, unsigned int count)
{
	struct v4l2_requestbuffers req;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.type = queue->type;
	req.memory = V4L2_MEMORY_MMAP;
	req.count = count;

	if (xioctl(queue->fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		return -1;
	}

	if (!req.count) {
		fprintf(stderr, "Driver returned zero MMAP buffers\n");
		return -1;
	}

	queue->buffers = calloc(req.count, sizeof(*queue->buffers));
	if (!queue->buffers) {
		fprintf(stderr, "calloc buffers failed\n");
		return -1;
	}
	queue->count = req.count;

	for (i = 0; i < queue->count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = queue->type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(queue->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			fprintf(stderr, "VIDIOC_QUERYBUF[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}

		queue->buffers[i].length = buf.length;
		queue->buffers[i].start = mmap(NULL, buf.length,
					      PROT_READ | PROT_WRITE,
					      MAP_SHARED, queue->fd, buf.m.offset);
		if (queue->buffers[i].start == MAP_FAILED) {
			queue->buffers[i].start = NULL;
			fprintf(stderr, "mmap buffer[%u] failed: %s\n",
				i, strerror(errno));
			return -1;
		}
	}

	return 0;
}

static int request_userptr_buffers(struct video_queue *queue, unsigned int count)
{
	struct v4l2_requestbuffers req;

	memset(&req, 0, sizeof(req));
	req.type = queue->type;
	req.memory = V4L2_MEMORY_USERPTR;
	req.count = count;

	if (xioctl(queue->fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "VIDIOC_REQBUFS(USERPTR) failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (!req.count) {
		fprintf(stderr, "Driver returned zero USERPTR buffers\n");
		return -1;
	}

	queue->count = req.count;
	return 0;
}

static void release_buffers(struct video_queue *queue)
{
	unsigned int i;

	if (!queue->buffers)
		goto out;

	for (i = 0; i < queue->count; i++) {
		if (queue->buffers[i].start)
			munmap(queue->buffers[i].start, queue->buffers[i].length);
	}

	free(queue->buffers);
	queue->buffers = NULL;
out:
	queue->count = 0;
}

static int queue_mmap_buffer(struct video_queue *queue, unsigned int index,
			     size_t bytesused)
{
	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));
	buf.type = queue->type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index;
	buf.bytesused = bytesused;

	if (xioctl(queue->fd, VIDIOC_QBUF, &buf) < 0) {
		fprintf(stderr, "VIDIOC_QBUF[%u] failed: %s\n",
			index, strerror(errno));
		return -1;
	}

	return 0;
}

static int queue_userptr_buffer(struct video_queue *queue, unsigned int index,
				void *userptr, size_t length)
{
	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));
	buf.type = queue->type;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = index;
	buf.m.userptr = (unsigned long)userptr;
	buf.length = length;
	buf.bytesused = length;

	if (xioctl(queue->fd, VIDIOC_QBUF, &buf) < 0) {
		fprintf(stderr, "VIDIOC_QBUF(USERPTR,%u) failed: %s\n",
			index, strerror(errno));
		return -1;
	}

	return 0;
}

static int dequeue_buffer(struct video_queue *queue, struct v4l2_buffer *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->type = queue->type;
	buf->memory = queue->memory;

	if (xioctl(queue->fd, VIDIOC_DQBUF, buf) < 0) {
		fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int stream_toggle(struct video_queue *queue, bool enable)
{
	enum v4l2_buf_type type = queue->type;

	if (xioctl(queue->fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF,
		   &type) < 0) {
		fprintf(stderr, "VIDIOC_STREAM%s failed: %s\n",
			enable ? "ON" : "OFF", strerror(errno));
		return -1;
	}

	return 0;
}

static int fill_capture_queue(struct video_queue *queue)
{
	unsigned int i;

	for (i = 0; i < queue->count; i++) {
		if (queue_mmap_buffer(queue, i, 0) < 0)
			return -1;
	}

	return 0;
}

static int run_bridge_loop(const struct demo_config *cfg, int cap_fd, int pxp_fd)
{
	struct video_queue cap_q = {
		.fd = cap_fd,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	struct video_queue pxp_q = {
		.fd = pxp_fd,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_USERPTR,
	};
	struct v4l2_buffer cap_buf;
	struct v4l2_buffer pxp_buf;
	size_t copy_len;
	unsigned int frame = 0;
	unsigned int fps_frames = 0;
	bool cap_started = false;
	bool pxp_started = false;
	unsigned int pxp_index = 0;
	int ret = -1;
	struct timeval tv_start, tv_now;
	long elapsed_us;
	int *pending_cap_idx = NULL;
	bool *pending_cap_valid = NULL;

	if (request_mmap_buffers(&cap_q, 4) < 0)
		goto out;
	if (request_userptr_buffers(&pxp_q, 2) < 0)
		goto out;
	pending_cap_idx = calloc(pxp_q.count, sizeof(*pending_cap_idx));
	pending_cap_valid = calloc(pxp_q.count, sizeof(*pending_cap_valid));
	if (!pending_cap_idx || !pending_cap_valid) {
		fprintf(stderr, "calloc pending USERPTR state failed\n");
		goto out;
	}
	if (fill_capture_queue(&cap_q) < 0)
		goto out;

	if (stream_toggle(&cap_q, true) < 0)
		goto out;
	cap_started = true;
	gettimeofday(&tv_start, NULL);

	while (g_running && (cfg->frames == 0 || frame < cfg->frames)) {
		if (pxp_started) {
			if (dequeue_buffer(&pxp_q, &pxp_buf) < 0) {
				if (!g_running) {
					ret = 0;
					goto out;
				}
				goto out;
			}
			pxp_index = pxp_buf.index;
			if (pxp_index < pxp_q.count && pending_cap_valid[pxp_index]) {
				if (queue_mmap_buffer(&cap_q,
						      pending_cap_idx[pxp_index], 0) < 0)
					goto out;
				pending_cap_valid[pxp_index] = false;
			}
		}

		if (dequeue_buffer(&cap_q, &cap_buf) < 0) {
			if (!g_running) {
				ret = 0;
				goto out;
			}
			goto out;
		}

		copy_len = cap_buf.bytesused;
		if (queue_userptr_buffer(&pxp_q, pxp_index,
					 cap_q.buffers[cap_buf.index].start,
					 copy_len) < 0) {
			queue_mmap_buffer(&cap_q, cap_buf.index, 0);
			goto out;
		}
		pending_cap_idx[pxp_index] = cap_buf.index;
		pending_cap_valid[pxp_index] = true;

		if (!pxp_started) {
			if (stream_toggle(&pxp_q, true) < 0) {
				queue_mmap_buffer(&cap_q, cap_buf.index, 0);
				pending_cap_valid[pxp_index] = false;
				goto out;
			}
			pxp_started = true;
		}

		frame++;
		fps_frames++;
		if ((fps_frames % 30) == 0) {
			gettimeofday(&tv_now, NULL);
			elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000L +
				     (tv_now.tv_usec - tv_start.tv_usec);
			if (elapsed_us > 0) {
				fprintf(stdout, "FPS: %.1f (%u frame%s)\n",
					30.0 * 1000000.0 / elapsed_us, frame,
					(frame > 1) ? "s" : "");
				fflush(stdout);
			}
			tv_start = tv_now;
		}
	}

	ret = 0;

out:
	if (pxp_started)
		stream_toggle(&pxp_q, false);
	if (pending_cap_valid) {
		for (pxp_index = 0; pxp_index < pxp_q.count; pxp_index++) {
			if (!pending_cap_valid[pxp_index])
				continue;
			queue_mmap_buffer(&cap_q, pending_cap_idx[pxp_index], 0);
			pending_cap_valid[pxp_index] = false;
		}
	}
	if (cap_started)
		stream_toggle(&cap_q, false);
	free(pending_cap_valid);
	free(pending_cap_idx);
	release_buffers(&pxp_q);
	release_buffers(&cap_q);
	return ret;
}

static int parse_u32(const char *arg, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &end, 0);
	if (errno || !end || *end != '\0' || parsed > UINT32_MAX)
		return -1;

	*value = (unsigned int)parsed;
	return 0;
}

static int parse_s32(const char *arg, int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(arg, &end, 0);
	if (errno || !end || *end != '\0' ||
	    parsed < INT32_MIN || parsed > INT32_MAX)
		return -1;

	*value = (int)parsed;
	return 0;
}

static int parse_args(int argc, char **argv, struct demo_config *cfg)
{
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->capture_dev = DEFAULT_CAPTURE_DEV;
	cfg->pxp_dev = DEFAULT_PXP_DEV;
	cfg->width = DEFAULT_WIDTH;
	cfg->height = DEFAULT_HEIGHT;
	cfg->display_width = 0;
	cfg->display_height = 0;
	cfg->frames = DEFAULT_FRAMES;
	cfg->pxp_output = DEFAULT_PXP_OUTPUT;
	cfg->fb_node = DEFAULT_FB_NODE;
	cfg->pixfmt = V4L2_PIX_FMT_YUYV;
	cfg->mbus_code = MEDIA_BUS_FMT_YUYV8_2X8;

	while ((opt = getopt_long(argc, argv, "c:p:s:i:w:h:W:H:n:f:o:b:?",
				  demo_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cfg->capture_dev = optarg;
			break;
		case 'p':
			cfg->pxp_dev = optarg;
			break;
		case 's':
			cfg->sensor_subdev = optarg;
			break;
		case 'i':
			cfg->csi_subdev = optarg;
			break;
		case 'w':
			if (parse_u32(optarg, &cfg->width) < 0)
				return -1;
			break;
		case 'h':
			if (parse_u32(optarg, &cfg->height) < 0)
				return -1;
			break;
		case 'W':
			if (parse_u32(optarg, &cfg->display_width) < 0)
				return -1;
			break;
		case 'H':
			if (parse_u32(optarg, &cfg->display_height) < 0)
				return -1;
			break;
		case 'n':
			if (parse_u32(optarg, &cfg->frames) < 0)
				return -1;
			break;
		case 'f':
			cfg->pixfmt = parse_fourcc(optarg);
			if (!cfg->pixfmt)
				return -1;
			cfg->mbus_code = mbus_code_from_pixfmt(cfg->pixfmt);
			break;
		case 'o':
			if (parse_u32(optarg, &cfg->pxp_output) < 0)
				return -1;
			break;
		case 'b':
			if (parse_s32(optarg, &cfg->fb_node) < 0)
				return -1;
			break;
		case '?':
		default:
			return 1;
		}
	}

	if (!cfg->display_width)
		cfg->display_width = cfg->width;
	if (!cfg->display_height)
		cfg->display_height = cfg->height;

	return 0;
}

int main(int argc, char **argv)
{
	struct demo_config cfg;
	char fourcc_name[5];
	int cap_fd = -1;
	int pxp_fd = -1;
	int ret;

	ret = parse_args(argc, argv, &cfg);
	if (ret > 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (ret < 0) {
		fprintf(stderr, "Invalid arguments\n");
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	fourcc_to_string(cfg.pixfmt, fourcc_name);
	fprintf(stdout,
		"capture=%s pxp=%s size=%ux%u display=%ux%u fmt=%s frames=%s\n",
		cfg.capture_dev, cfg.pxp_dev, cfg.width, cfg.height,
		cfg.display_width, cfg.display_height, fourcc_name,
		cfg.frames ? "limited" : "endless");

	if (cfg.sensor_subdev || cfg.csi_subdev) {
		if (configure_optional_subdevs(&cfg) < 0)
			return EXIT_FAILURE;
	}

	cap_fd = open_device(cfg.capture_dev, O_RDWR);
	if (cap_fd < 0)
		goto fail;

	pxp_fd = open_device(cfg.pxp_dev, O_RDWR);
	if (pxp_fd < 0)
		goto fail;

	if (set_capture_format(cap_fd, cfg.width, cfg.height, cfg.pixfmt) < 0)
		goto fail;
	if (cfg.fb_node != DEFAULT_FB_NODE) {
		if (set_pxp_fb_node(pxp_fd, cfg.fb_node) < 0)
			goto fail;
	}
	if (set_pxp_output(pxp_fd, cfg.pxp_output) < 0)
		goto fail;
	if (set_pxp_input_format(pxp_fd, cfg.width, cfg.height, cfg.pixfmt) < 0)
		goto fail;
	if (set_pxp_display_window(pxp_fd, cfg.width, cfg.height,
				   cfg.display_width, cfg.display_height) < 0)
		goto fail;
	if (run_bridge_loop(&cfg, cap_fd, pxp_fd) < 0)
		goto fail;

	close(pxp_fd);
	close(cap_fd);
	return EXIT_SUCCESS;

fail:
	if (pxp_fd >= 0)
		close(pxp_fd);
	if (cap_fd >= 0)
		close(cap_fd);
	return EXIT_FAILURE;
}
