
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <poll.h>


/**
 * V4L2 摄像头采集基本流程：
 *
 *  1. open 打开摄像头设备节点，例如 /dev/video0、/dev/video1。
 *  2. VIDIOC_QUERYCAP 查询设备能力，确认是否支持视频采集和 streaming I/O。
 *  3. VIDIOC_ENUM_FMT 枚举摄像头支持的像素格式，例如 YUYV、MJPEG。
 *  4. VIDIOC_ENUM_FRAMESIZES 枚举每种像素格式支持的分辨率。
 *  5. VIDIOC_S_FMT 设置本程序希望使用的采集格式、宽高和 field。
 *     注意：驱动可能会把不支持的参数调整成最接近的可用参数，
 *     所以调用成功后要重新读取 struct v4l2_format 中的实际结果。
 *  6. VIDIOC_REQBUFS 向驱动申请若干个视频缓冲区。
 *  7. VIDIOC_QUERYBUF 查询每个缓冲区的长度和 offset。
 *  8. mmap 把驱动缓冲区映射到用户空间，应用程序后续可以直接访问帧数据。
 *  9. VIDIOC_QBUF 把所有空闲缓冲区放入驱动队列，等待驱动填充图像数据。
 * 10. VIDIOC_STREAMON 启动视频流采集。
 * 11. poll 等待摄像头产生一帧数据。
 * 12. VIDIOC_DQBUF 从驱动队列取出已经填充好的缓冲区。
 * 13. 处理这一帧数据，例如保存为文件、转换格式或显示到 LCD。
 * 14. VIDIOC_QBUF 把处理完的缓冲区重新放回队列，供驱动继续采集。
 * 15. 重复 11 到 14，持续采集视频帧。
 * 16. VIDIOC_STREAMOFF 停止采集，最后 munmap/close 释放资源。
 */

int main(int argc, char **argv) {

    int fd;

    // 设置数据格式的结构体
    struct v4l2_fmtdesc fmtdesc;

    // 枚举摄像头支持的格式
    struct v4l2_frmsizeenum fsenum;

    int fmt_index = 0;
    int frame_index = 0;

    if (argc != 2) {
        printf("Usage: %s </dev/videoX>, print format detail for video device \n", argv[0]);
        return -1;
    }

    // 1. open dev
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("can not open %s device.\n", argv[1]);
    }

    // 2. VIDIOC_QUERYCAP Query Capability
    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(struct v4l2_capability));

    // check capability
    if (0 == ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        if ((capability.capabilities && V4L2_CAP_VIDEO_CAPTURE) == 0) {
            printf("Device %s can not supoorted capture.\n", argv[1]);
            return -1;
        }

        if (!(capability.capabilities & V4L2_CAP_STREAMING)) {
            printf("Device %d can not supported streaming i/o \n", argv[1]);
            return -1;
        }
    }
    else {
        printf("can not get capability \n");
        return -1;
    }


    // 3. enum format
    while (1) {
        fmtdesc.index = fmt_index;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (0 != ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
            break;
        }

        frame_index = 0;
        while (1) {
            memset(&fsenum, 0, sizeof(struct v4l2_frmsizeenum));
            fsenum.pixel_format = fmtdesc.pixelformat;
            fsenum.index = fmtdesc.index;

            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsenum) == 0) {
                 printf("format %s,%d, framesize %d: %d x %d\n", fmtdesc.description, fmtdesc.pixelformat, frame_index, fsenum.discrete.width, fsenum.discrete.height);
            } else {
                break;
            }
            frame_index++;
        }
        fmt_index++;
    }

    // 4.  set format
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1920;
    fmt.fmt.pix.height = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (0 == ioctl(fd, VIDIOC_S_FMT, &fmt)) {
        printf("set format ok; %d x %d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
    } else {
        printf("set format error\n");
        return -1;
    }

    // 5. 申请 buffer
    struct v4l2_requestbuffers  rb;
    memset(&rb, 0, sizeof(struct v4l2_requestbuffers));
    rb.count = 32;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;

    int buf_cnt;
    void *bufs[32];
    if (0 == ioctl(fd, VIDIOC_REQBUFS, &rb)) {
        buf_cnt = rb.count;
        for (int i = 0; i < rb.count; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(struct v4l2_buffer));
            buf.index = i;
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (0 == ioctl(fd, VIDIOC_QUERYBUF, &buf)) {
                bufs[i] = mmap(0, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
                if (bufs[i] == MAP_FAILED) {
                    printf("Error Unable to map buffer \n");
                    return -1;
                }
            } else {
                printf("Can not query buffer \n");
                return -1;
            }
        }
        printf(" map %d buffers ok \n", buf_cnt);
    }
    else {
        printf("can not request buffers \n");
        return -1;
    }

    for (int i =0; i < buf_cnt; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(struct v4l2_buffer));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (0 != ioctl(fd, VIDIOC_QBUF, &buf))
        {
            perror("Unable to queue buffer");
            return -1;
        }
    }

    printf("queue buffers ok\n");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* 启动摄像头 */
    if (0 != ioctl(fd, VIDIOC_STREAMON, &type))
    {
        perror("Unable to start capture");
        return -1;
    }
    printf("start capture ok\n");

    // 循环获取数据
    struct pollfd fds[1];
    char filename[32];
    int file_cnt = 0;
    while (1) {
        memset(fds, 0, sizeof(fds));
        fds[0].fd = fd;
        fds[0].events = POLLIN;

        if (1 == poll(fds, 1, -1)) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(struct v4l2_buffer));

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (0 != ioctl(fd, VIDIOC_DQBUF, &buf)) {
                printf("Unable to dequeue buffer");
                return -1;
            }

            /* 把buffer的数据存为文件 */
            sprintf(filename, "video_raw_data_%04d.jpg", file_cnt++);
            int fd_file = open(filename, O_RDWR | O_CREAT, 0666);
            if (fd_file < 0)
            {
                printf("can not create file : %s\n", filename);
            }
            printf("capture to %s\n", filename);
            write(fd_file, bufs[buf.index], buf.bytesused);
            close(fd_file);

            /* 把buffer放入队列 */
            if (0 != ioctl(fd, VIDIOC_QBUF, &buf))
            {
                perror("Unable to queue buffer");
                return -1;
            }
        }

    }

    if (0 != ioctl(fd, VIDIOC_STREAMOFF, &type))
    {
        perror("Unable to stop capture");
        return -1;
    }
    printf("stop capture ok\n");
    close(fd);

    return 0;
}
