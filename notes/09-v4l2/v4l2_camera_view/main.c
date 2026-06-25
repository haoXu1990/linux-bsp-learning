#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "app_args.h"
#include "yuyv_rgb565.h"

#define BUFFER_COUNT 4
#define DEFAULT_WIDTH 1024
#define DEFAULT_HEIGHT 600

struct video_buffer {
    void *start;
    size_t length;
};

struct fb_device {
    int fd;
    const char *name;
    unsigned char *mem;
    size_t mem_size;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

static void fourcc_to_string(unsigned int fmt, char out[5])
{
    out[0] = fmt & 0xff;
    out[1] = (fmt >> 8) & 0xff;
    out[2] = (fmt >> 16) & 0xff;
    out[3] = (fmt >> 24) & 0xff;
    out[4] = '\0';
}

static int open_framebuffer(const char *fb_name, struct fb_device *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->name = fb_name;
    fb->fd = open(fb_name, O_RDWR);
    if (fb->fd < 0) {
        perror("open framebuffer");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
        perror("FBIOGET_VSCREENINFO");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
        perror("FBIOGET_FSCREENINFO");
        return -1;
    }

    if (fb->var.bits_per_pixel != 16 && fb->var.bits_per_pixel != 32) {
        fprintf(stderr, "当前示例只支持 16bpp/32bpp framebuffer, 当前 bpp=%u\n",
                fb->var.bits_per_pixel);
        return -1;
    }

    fb->mem_size = fb->fix.smem_len;
    fb->mem = mmap(NULL, fb->mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("mmap framebuffer");
        fb->mem = NULL;
        return -1;
    }

    printf("LCD: %ux%u, %u bpp, line_length=%u, R:%u/%u G:%u/%u B:%u/%u\n",
           fb->var.xres, fb->var.yres, fb->var.bits_per_pixel, fb->fix.line_length,
           fb->var.red.offset, fb->var.red.length,
           fb->var.green.offset, fb->var.green.length,
           fb->var.blue.offset, fb->var.blue.length);
    return 0;
}

static void close_framebuffer(struct fb_device *fb)
{
    if (fb->mem)
        munmap(fb->mem, fb->mem_size);
    if (fb->fd >= 0)
        close(fb->fd);
}

static void draw_rgb565_to_fb(const struct fb_device *fb,
                              const uint16_t *rgb565,
                              unsigned int width,
                              unsigned int height)
{
    unsigned int copy_width = width < fb->var.xres ? width : fb->var.xres;
    unsigned int copy_height = height < fb->var.yres ? height : fb->var.yres;
    unsigned int left = (fb->var.xres - copy_width) / 2;
    unsigned int top = (fb->var.yres - copy_height) / 2;
    unsigned int y;

    for (y = 0; y < copy_height; y++) {
        unsigned char *dst = fb->mem + (top + y) * fb->fix.line_length + left * 2;
        const unsigned char *src = (const unsigned char *)(rgb565 + y * width);

        memcpy(dst, src, copy_width * 2);
    }
}

static uint32_t pack_fb32_pixel(const struct fb_device *fb, uint32_t rgb888)
{
    uint32_t r = (rgb888 >> 16) & 0xff;
    uint32_t g = (rgb888 >> 8) & 0xff;
    uint32_t b = rgb888 & 0xff;

    r >>= 8 - fb->var.red.length;
    g >>= 8 - fb->var.green.length;
    b >>= 8 - fb->var.blue.length;

    return (r << fb->var.red.offset) |
           (g << fb->var.green.offset) |
           (b << fb->var.blue.offset);
}

static uint32_t rgb565_to_rgb888(uint16_t rgb565)
{
    uint32_t r = (rgb565 >> 11) & 0x1f;
    uint32_t g = (rgb565 >> 5) & 0x3f;
    uint32_t b = rgb565 & 0x1f;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    return (r << 16) | (g << 8) | b;
}

static void draw_yuyv_to_fb32(const struct fb_device *fb,
                              const unsigned char *yuyv,
                              unsigned int width,
                              unsigned int height)
{
    unsigned int copy_width = width < fb->var.xres ? width : fb->var.xres;
    unsigned int copy_height = height < fb->var.yres ? height : fb->var.yres;
    unsigned int left = (fb->var.xres - copy_width) / 2;
    unsigned int top = (fb->var.yres - copy_height) / 2;
    unsigned int y;

    for (y = 0; y < copy_height; y++) {
        uint32_t *dst = (uint32_t *)(fb->mem + (top + y) * fb->fix.line_length + left * 4);
        const unsigned char *src = yuyv + y * width * 2;
        unsigned int x;

        for (x = 0; x + 1 < copy_width; x += 2) {
            unsigned char y0 = src[x * 2 + 0];
            unsigned char u  = src[x * 2 + 1];
            unsigned char y1 = src[x * 2 + 2];
            unsigned char v  = src[x * 2 + 3];

            dst[x] = pack_fb32_pixel(fb, yuv_to_rgb888_pixel(y0, u, v));
            dst[x + 1] = pack_fb32_pixel(fb, yuv_to_rgb888_pixel(y1, u, v));
        }

        if (x < copy_width) {
            unsigned char y0 = src[x * 2 + 0];
            unsigned char u  = src[x * 2 + 1];
            unsigned char v  = src[x * 2 + 1];

            dst[x] = pack_fb32_pixel(fb, yuv_to_rgb888_pixel(y0, u, v));
        }
    }
}

static void draw_rgb565_to_fb32(const struct fb_device *fb,
                                const uint16_t *rgb565,
                                unsigned int width,
                                unsigned int height)
{
    unsigned int copy_width = width < fb->var.xres ? width : fb->var.xres;
    unsigned int copy_height = height < fb->var.yres ? height : fb->var.yres;
    unsigned int left = (fb->var.xres - copy_width) / 2;
    unsigned int top = (fb->var.yres - copy_height) / 2;
    unsigned int y;

    for (y = 0; y < copy_height; y++) {
        uint32_t *dst = (uint32_t *)(fb->mem + (top + y) * fb->fix.line_length + left * 4);
        const uint16_t *src = rgb565 + y * width;
        unsigned int x;

        for (x = 0; x < copy_width; x++)
            dst[x] = pack_fb32_pixel(fb, rgb565_to_rgb888(src[x]));
    }
}

static int init_camera(const char *dev_name,
                       unsigned int req_width,
                       unsigned int req_height,
                       struct video_buffer buffers[BUFFER_COUNT],
                       unsigned int *width,
                       unsigned int *height,
                       unsigned int *pixel_format)
{
    int fd;
    unsigned int i;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    char fourcc[5];

    /* 步骤 1：打开摄像头设备节点。 */
    fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        perror("open camera");
        return -1;
    }

    /* 步骤 2：查询设备能力，确认它是视频采集设备并支持 mmap streaming。 */
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s 不是 video capture 设备\n", dev_name);
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s 不支持 streaming I/O\n", dev_name);
        close(fd);
        return -1;
    }

    /*
     * 步骤 3：设置采集格式。
     * 这个学习版优先使用 YUYV，因为 YUYV -> RGB565 可以自己完成，
     * 不需要引入 JPEG 解码库。
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = req_width;
    fmt.fmt.pix.height = req_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT YUYV");
        close(fd);
        return -1;
    }

    *width = fmt.fmt.pix.width;
    *height = fmt.fmt.pix.height;
    *pixel_format = fmt.fmt.pix.pixelformat;
    fourcc_to_string(*pixel_format, fourcc);
    printf("Camera format: %s, %ux%u\n", fourcc, *width, *height);

    if (*pixel_format != V4L2_PIX_FMT_YUYV && *pixel_format != V4L2_PIX_FMT_RGB565) {
        fprintf(stderr, "当前示例只支持 YUYV/RGB565，实际格式是 %s\n", fourcc);
        close(fd);
        return -1;
    }

    /* 步骤 4：申请 V4L2 mmap 缓冲区。 */
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return -1;
    }

    if (req.count < BUFFER_COUNT) {
        fprintf(stderr, "驱动只分配了 %u 个 buffer\n", req.count);
        close(fd);
        return -1;
    }

    /* 步骤 5：查询每个 buffer，并 mmap 到用户空间。 */
    for (i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            close(fd);
            return -1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap video buffer");
            buffers[i].start = NULL;
            close(fd);
            return -1;
        }
    }

    /* 步骤 6：把所有 buffer 放入驱动队列，等待摄像头填充。 */
    for (i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            close(fd);
            return -1;
        }
    }

    return fd;
}

static void unmap_video_buffers(struct video_buffer buffers[BUFFER_COUNT])
{
    unsigned int i;

    for (i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i].start)
            munmap(buffers[i].start, buffers[i].length);
    }
}

int main(int argc, char **argv)
{
    int video_fd;
    struct app_args args;
    struct fb_device fb;
    struct video_buffer buffers[BUFFER_COUNT];
    unsigned int width;
    unsigned int height;
    unsigned int pixel_format;
    uint16_t *rgb565 = NULL;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (parse_app_args(argc, argv, &args) != 0) {
        printf("Usage: %s </dev/videoX> [fb device]\n", argv[0]);
        printf("Example: %s /dev/video1 /dev/fb0\n", argv[0]);
        return -1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    memset(buffers, 0, sizeof(buffers));
    fb.fd = -1;

    printf("使用摄像头设备: %s\n", args.video_device);
    printf("使用 framebuffer: %s\n", args.fb_device);

    /* 步骤 1：打开 LCD framebuffer，后续把图像写到这里显示。 */
    if (open_framebuffer(args.fb_device, &fb) < 0)
        return -1;

    // if (!is_primary_fb(args.fb_device))
    //     unblank_primary_fb_if_needed(args.fb_device);

    // /*
    //  * 步骤 1.1：如果当前 framebuffer 是 overlay 层，需要 unblank 才会显示。
    //  * 从 mxsfb overlay 驱动看，fb1 的显示/隐藏由 overlayfb_blank 控制：
    //  * FB_BLANK_UNBLANK 会 enable overlay，FB_BLANK_NORMAL 等状态会 disable overlay。
    //  */
    // if (!is_primary_fb(args.fb_device)) {
    //     apply_fb_var(&fb);
    //     set_fb_blank(&fb, FB_BLANK_UNBLANK);
    // } else {
    //     printf("%s: 主 framebuffer 显示只写显存，不执行 FBIOPUT/FBIOBLANK/alpha\n",
    //            fb.name);
    // }

    /*
     * 步骤 1.2： 这里好像也不是必须的
     * 当前 100ask mxsfb overlay_fb_ops 没有实现 .fb_ioctl，这个 ioctl 可能失败。
     * 失败只打印提示，不中断流程；真正关键的是上面的 FBIOBLANK unblank。
     */
    // if (!is_primary_fb(args.fb_device))
    //     set_fb_global_alpha(&fb, 1, 255);

    /* 步骤 2：初始化摄像头，设置 YUYV 格式，申请并映射 V4L2 buffer。 */
    video_fd = init_camera(args.video_device, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                           buffers, &width, &height, &pixel_format);
    if (video_fd < 0) {
        close_framebuffer(&fb);
        return -1;
    }

    if (pixel_format == V4L2_PIX_FMT_YUYV && fb.var.bits_per_pixel == 16) {
        rgb565 = malloc(width * height * sizeof(uint16_t));
        if (!rgb565) {
            perror("malloc rgb565");
            unmap_video_buffers(buffers);
            close(video_fd);
            close_framebuffer(&fb);
            return -1;
        }
    }

    /* 步骤 3：启动视频流。 */
    if (xioctl(video_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        free(rgb565);
        unmap_video_buffers(buffers);
        close(video_fd);
        close_framebuffer(&fb);
        return -1;
    }

    printf("开始显示，按 Ctrl+C 退出\n");

    while (!g_stop) {
        struct pollfd pfd;
        struct v4l2_buffer buf;
        /* 步骤 4：等待摄像头产生一帧数据。 */
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = video_fd;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 1000) <= 0)
            continue;

        /* 步骤 5：从驱动队列取出已经填充好的 buffer。 */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(video_fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("VIDIOC_DQBUF");
            break;
        }

        /*
         * 步骤 6：把摄像头帧转换成 LCD 使用的像素格式。
         * - 16bpp LCD：显示格式是 RGB565。
         * - 32bpp LCD：按 framebuffer 的 RGB offset 打包成 32 位像素。
         */
        if (fb.var.bits_per_pixel == 16) {
            const uint16_t *frame_rgb565;

            if (pixel_format == V4L2_PIX_FMT_YUYV) {
                yuyv_to_rgb565(buffers[buf.index].start, rgb565, width * height);
                frame_rgb565 = rgb565;
            } else {
                frame_rgb565 = buffers[buf.index].start;
            }

            /* 步骤 7：把 RGB565 图像居中写入 framebuffer，LCD 立即显示。 */
            draw_rgb565_to_fb(&fb, frame_rgb565, width, height);
        } else {
            if (pixel_format == V4L2_PIX_FMT_YUYV)
                draw_yuyv_to_fb32(&fb, buffers[buf.index].start, width, height);
            else
                draw_rgb565_to_fb32(&fb, buffers[buf.index].start, width, height);
        }

        /* 步骤 8：处理完这一帧后，把 buffer 重新放回驱动队列。 */
        if (xioctl(video_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    /* 步骤 9：停止采集并释放资源。 */
    xioctl(video_fd, VIDIOC_STREAMOFF, &type);
    /*
     * 暂时不在退出时 blank fb1。
     * 当前板端现象显示 blank fb1 会触发 goodix 触摸进入 sleep，
     * 所以先保留 overlay 状态，用下一轮实验确认显示路径。
     */
    free(rgb565);
    unmap_video_buffers(buffers);
    close(video_fd);
    close_framebuffer(&fb);

    return 0;
}
