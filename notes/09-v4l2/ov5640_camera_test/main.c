#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct mapped_buffer {
    void *start;
    size_t length;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

static void fourcc_to_string(uint32_t fourcc, char text[5])
{
    text[0] = fourcc & 0xff;
    text[1] = (fourcc >> 8) & 0xff;
    text[2] = (fourcc >> 16) & 0xff;
    text[3] = (fourcc >> 24) & 0xff;
    text[4] = '\0';
}

static int parse_u32(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || parsed > UINT32_MAX)
        return -1;

    *value = (unsigned int)parsed;
    return 0;
}

static uint32_t parse_pixel_format(const char *text)
{
    if (strcmp(text, "YUYV") == 0)
        return V4L2_PIX_FMT_YUYV;
    return 0;
}

static void enumerate_formats(int fd)
{
    struct v4l2_fmtdesc desc;
    unsigned int index;

    printf("Supported capture formats:\n");
    for (index = 0;; index++) {
        char fourcc[5];

        memset(&desc, 0, sizeof(desc));
        desc.index = index;
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
            if (errno != EINVAL)
                perror("VIDIOC_ENUM_FMT");
            break;
        }

        fourcc_to_string(desc.pixelformat, fourcc);
        printf("  [%u] %s  %s\n", desc.index, fourcc, desc.description);
    }
}

static void enumerate_frame_sizes(int fd, uint32_t pixel_format)
{
    struct v4l2_frmsizeenum size;
    char fourcc[5];

    fourcc_to_string(pixel_format, fourcc);
    printf("Frame sizes for %s:\n", fourcc);

    for (size.index = 0;; size.index++) {
        unsigned int index = size.index;

        memset(&size, 0, sizeof(size));
        size.index = index;
        size.pixel_format = pixel_format;
        if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
            if (errno != EINVAL)
                perror("VIDIOC_ENUM_FRAMESIZES");
            break;
        }

        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            printf("  [%u] %ux%u\n", size.index,
                   size.discrete.width, size.discrete.height);
        } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            printf("  [%u] %ux%u .. %ux%u, step %ux%u\n", size.index,
                   size.stepwise.min_width, size.stepwise.min_height,
                   size.stepwise.max_width, size.stepwise.max_height,
                   size.stepwise.step_width, size.stepwise.step_height);
        } else {
            printf("  [%u] continuous %ux%u .. %ux%u\n", size.index,
                   size.stepwise.min_width, size.stepwise.min_height,
                   size.stepwise.max_width, size.stepwise.max_height);
        }
    }
}

static void print_format(const char *title, const struct v4l2_format *fmt)
{
    char fourcc[5];

    fourcc_to_string(fmt->fmt.pix.pixelformat, fourcc);
    printf("%s:\n", title);
    printf("  size         : %ux%u\n",
           fmt->fmt.pix.width, fmt->fmt.pix.height);
    printf("  pixel format : %s\n", fourcc);
    printf("  field        : %u\n", fmt->fmt.pix.field);
    printf("  bytesperline : %u\n", fmt->fmt.pix.bytesperline);
    printf("  sizeimage    : %u\n", fmt->fmt.pix.sizeimage);
    printf("  colorspace   : %u\n", fmt->fmt.pix.colorspace);
}

static void print_first_bytes(const unsigned char *data, size_t length)
{
    size_t count = length < 64 ? length : 64;
    size_t i;

    printf("First %u bytes:", (unsigned int)count);
    for (i = 0; i < count; i++) {
        if ((i % 16) == 0)
            printf("\n  ");
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static void print_byte_lane_statistics(const unsigned char *data, size_t length)
{
    uint64_t sum[4] = {0, 0, 0, 0};
    unsigned int count[4] = {0, 0, 0, 0};
    unsigned int min_value[4] = {255, 255, 255, 255};
    unsigned int max_value[4] = {0, 0, 0, 0};
    size_t sample_length = length < (1024U * 1024U) ? length : (1024U * 1024U);
    size_t i;

    for (i = 0; i < sample_length; i++) {
        unsigned int lane = i & 3;
        unsigned int value = data[i];

        sum[lane] += value;
        count[lane]++;
        if (value < min_value[lane])
            min_value[lane] = value;
        if (value > max_value[lane])
            max_value[lane] = value;
    }

    printf("Byte-lane statistics (offset in every four bytes):\n");
    for (i = 0; i < 4; i++) {
        unsigned int average = count[i] ? (unsigned int)(sum[i] / count[i]) : 0;
        printf("  lane %u: avg=%3u min=%3u max=%3u\n",
               (unsigned int)i, average, min_value[i], max_value[i]);
    }
    printf("  YUYV expects [Y0 U Y1 V]; UYVY expects [U Y0 V Y1].\n");
}

static int save_frame(const char *path, const void *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    size_t written;

    if (!file) {
        perror("fopen output");
        return -1;
    }

    written = fwrite(data, 1, length, file);
    if (written != length) {
        perror("fwrite output");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        perror("fclose output");
        return -1;
    }

    printf("Saved %u bytes to %s\n", (unsigned int)length, path);
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [video] [output.raw] [width] [height] [YUYV] [frames]\n",
           program);
    printf("Default: %s /dev/video1 ov5640_frame.raw 640 480 YUYV 10\n",
           program);
}

int main(int argc, char **argv)
{
    const char *video_path = argc > 1 ? argv[1] : "/dev/video1";
    const char *output_path = argc > 2 ? argv[2] : "ov5640_frame.raw";
    unsigned int width = 640;
    unsigned int height = 480;
    uint32_t requested_format = V4L2_PIX_FMT_YUYV;
    unsigned int frame_count = 10;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct mapped_buffer *buffers = NULL;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned int allocated_buffers = 0;
    int streaming = 0;
    int fd = -1;
    int result = EXIT_FAILURE;
    unsigned int i;

    if (argc > 7) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc > 3 && parse_u32(argv[3], &width) < 0) {
        fprintf(stderr, "Invalid width: %s\n", argv[3]);
        return EXIT_FAILURE;
    }
    if (argc > 4 && parse_u32(argv[4], &height) < 0) {
        fprintf(stderr, "Invalid height: %s\n", argv[4]);
        return EXIT_FAILURE;
    }
    if (argc > 5) {
        requested_format = parse_pixel_format(argv[5]);
        if (!requested_format) {
            fprintf(stderr, "Unsupported requested format: %s\n", argv[5]);
            return EXIT_FAILURE;
        }
    }
    if (argc > 6 && (parse_u32(argv[6], &frame_count) < 0 || frame_count == 0)) {
        fprintf(stderr, "Invalid frame count: %s\n", argv[6]);
        return EXIT_FAILURE;
    }

    fd = open(video_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open video");
        goto cleanup;
    }

    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto cleanup;
    }

    printf("Device: %s\n", video_path);
    printf("  driver   : %s\n", cap.driver);
    printf("  card     : %s\n", cap.card);
    printf("  bus_info : %s\n", cap.bus_info);
    printf("  version  : %u.%u.%u\n",
           (cap.version >> 16) & 0xff,
           (cap.version >> 8) & 0xff,
           cap.version & 0xff);

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support capture + streaming\n");
        goto cleanup;
    }

    enumerate_formats(fd);
    enumerate_frame_sizes(fd, requested_format);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) == 0)
        print_format("Format before S_FMT", &fmt);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = requested_format;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto cleanup;
    }
    print_format("Format returned by S_FMT", &fmt);

    if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0 ||
        fmt.fmt.pix.sizeimage == 0) {
        fprintf(stderr, "Driver returned an invalid capture format\n");
        goto cleanup;
    }

    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto cleanup;
    }
    if (req.count < 2) {
        fprintf(stderr, "Driver allocated only %u buffers\n", req.count);
        goto cleanup;
    }

    buffers = calloc(req.count, sizeof(*buffers));
    if (!buffers) {
        perror("calloc buffers");
        goto cleanup;
    }
    allocated_buffers = req.count;

    for (i = 0; i < allocated_buffers; i++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto cleanup;
        }

        buffers[i].length = buffer.length;
        buffers[i].start = mmap(NULL, buffer.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd, buffer.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            buffers[i].start = NULL;
            perror("mmap capture buffer");
            goto cleanup;
        }

        printf("Buffer %u: length=%u offset=%u\n", i,
               buffer.length, buffer.m.offset);
    }

    for (i = 0; i < allocated_buffers; i++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            perror("VIDIOC_QBUF initial");
            goto cleanup;
        }
    }

    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto cleanup;
    }
    streaming = 1;

    for (i = 0; i < frame_count; i++) {
        struct pollfd poll_fd;
        struct v4l2_buffer buffer;
        size_t used;
        int poll_result;

        memset(&poll_fd, 0, sizeof(poll_fd));
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;

        do {
            poll_result = poll(&poll_fd, 1, 2000);
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result == 0) {
            fprintf(stderr, "Timed out waiting for frame %u\n", i + 1);
            goto cleanup;
        }
        if (poll_result < 0) {
            perror("poll capture");
            goto cleanup;
        }

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            perror("VIDIOC_DQBUF");
            goto cleanup;
        }
        if (buffer.index >= allocated_buffers) {
            fprintf(stderr, "Driver returned invalid buffer index %u\n", buffer.index);
            goto cleanup;
        }

        used = buffer.bytesused;
        if (used == 0 || used > buffers[buffer.index].length)
            used = buffers[buffer.index].length;

        printf("Frame %u/%u: index=%u sequence=%u bytesused=%u flags=0x%x\n",
               i + 1, frame_count, buffer.index, buffer.sequence,
               buffer.bytesused, buffer.flags);

        if (i + 1 == frame_count) {
            print_first_bytes(buffers[buffer.index].start, used);
            print_byte_lane_statistics(buffers[buffer.index].start, used);
            if (save_frame(output_path, buffers[buffer.index].start, used) < 0)
                goto cleanup;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            perror("VIDIOC_QBUF capture");
            goto cleanup;
        }
    }

    result = EXIT_SUCCESS;

cleanup:
    if (streaming)
        xioctl(fd, VIDIOC_STREAMOFF, &type);

    if (buffers) {
        for (i = 0; i < allocated_buffers; i++) {
            if (buffers[i].start)
                munmap(buffers[i].start, buffers[i].length);
        }
        free(buffers);
    }

    if (fd >= 0)
        close(fd);

    return result;
}
