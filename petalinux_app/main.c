/**
 * @file main.c
 * @brief USB UVC相机应用程序主程序
 * 
 * 功能：
 * 1. 初始化VPSS和VDMA
 * 2. 从DDR读取视频帧（RGBA格式，A固定为FF）
 * 3. 直接输出RGBA格式到UVC（不进行格式转换）
 * 4. 通过UVC Gadget发送到PC端（640x480@60fps，USB3.0）
 * 
 * 数据流：
 * CameraLink(PL) → VPSS(YUV422→RGB) → VDMA → DDR(RGBA) → 应用程序 → UVC(RGBA) → PC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <time.h>

#include "vpss_control.h"
#include "vdma_control.h"

/* 视频参数 - 640x480@60fps */
#define VIDEO_WIDTH     640
#define VIDEO_HEIGHT    480
#define BYTES_PER_PIXEL 4    /* RGBA格式：32位，A固定为FF */
#define NUM_FRAMES      3    /* 三缓冲（避免读写冲突） */
#define FRAME_SIZE      (VIDEO_WIDTH * VIDEO_HEIGHT * BYTES_PER_PIXEL)

/* 帧缓冲物理地址（必须与设备树中reserved-memory一致） */
#define FRAME_BUFFER_PHYS   0x10000000

/* UVC设备节点 */
#define UVC_DEVICE      "/dev/video0"

/* 目标帧率（fps）- 60fps for USB3.0 */
#define TARGET_FPS      60
#define FRAME_INTERVAL_US  (1000000 / TARGET_FPS)

/* 全局变量 */
static vpss_control_t vpss;
static vdma_control_t vdma;
static int uvc_fd = -1;
static volatile int running = 1;

/**
 * 信号处理函数
 */
void signal_handler(int signum)
{
    printf("\n接收到信号 %d，正在退出...\n", signum);
    running = 0;
}

/* 
 * 注意：不再需要格式转换函数
 * 直接发送RGBA数据到UVC设备
 */

/**
 * 初始化UVC设备
 * 
 * @param device UVC设备路径（通常是/dev/video0）
 * @return 0成功，-1失败
 */
int uvc_init(const char *device)
{
    struct v4l2_format fmt;
    
    printf("打开UVC设备: %s\n", device);
    
    uvc_fd = open(device, O_RDWR | O_NONBLOCK);
    if (uvc_fd < 0) {
        perror("打开UVC设备失败");
        fprintf(stderr, "提示: 请先运行 setup_uvc.sh 配置UVC Gadget\n");
        return -1;
    }
    
    /* 设置视频格式 - RGBA格式（不压缩） */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = VIDEO_WIDTH;
    fmt.fmt.pix.height = VIDEO_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_ABGR32;  /* RGBA格式 */
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.sizeimage = VIDEO_WIDTH * VIDEO_HEIGHT * 4;  /* RGBA: 4 bytes/pixel */
    
    if (ioctl(uvc_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置视频格式失败");
        close(uvc_fd);
        uvc_fd = -1;
        return -1;
    }
    
    printf("UVC格式设置完成: %dx%d RGBA\n", 
           fmt.fmt.pix.width, fmt.fmt.pix.height);
    
    return 0;
}

/**
 * 主循环：读取帧并发送到UVC
 */
int main_loop()
{
    /* 不再需要额外的缓冲区，直接从VDMA帧缓冲发送RGBA数据 */
    
    int frame_count = 0;
    int last_vdma_frame = -1;
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    printf("\n开始视频流传输...\n");
    printf("分辨率: %dx%d@%dfps (RGBA格式)\n", VIDEO_WIDTH, VIDEO_HEIGHT, TARGET_FPS);
    printf("按Ctrl+C退出\n\n");
    
    while (running) {
        /* 获取VDMA当前写入的帧 */
        int current_vdma_frame = vdma_get_current_frame(&vdma);
        
        /* 选择一个不同的帧读取（避免读写冲突） */
        int read_frame = (current_vdma_frame + 1) % NUM_FRAMES;
        
        /* 如果帧没有变化，跳过（避免重复读取） */
        if (current_vdma_frame == last_vdma_frame && frame_count > 0) {
            usleep(1000);  /* 短暂等待 */
            continue;
        }
        last_vdma_frame = current_vdma_frame;
        
        /* 从帧缓冲读取RGBA数据 */
        const uint8_t *rgba_frame = (uint8_t*)vdma.frame_buffer + (read_frame * FRAME_SIZE);
        
        /* 直接发送RGBA数据到UVC设备 */
        ssize_t written = write(uvc_fd, rgba_frame, FRAME_SIZE);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞写入，缓冲区满，稍后重试 */
                usleep(1000);
                continue;
            } else {
                perror("写入UVC设备失败");
                break;
            }
        }
        
        frame_count++;
        
        /* 每60帧打印一次统计信息 */
        if (frame_count % 60 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                           (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frame_count / elapsed;
            
            printf("已发送 %d 帧 (读取帧%d, VDMA写帧%d, 实际FPS: %.1f)\n", 
                   frame_count, read_frame, current_vdma_frame, fps);
        }
        
        /* 控制帧率（60fps = 16666us per frame） */
        usleep(FRAME_INTERVAL_US);
    }
    
    printf("\n总共发送 %d 帧\n", frame_count);
    
    return 0;
}

/**
 * 主函数
 */
int main(int argc, char **argv)
{
    int ret = 0;
    
    printf("========================================\n");
    printf("USB UVC Camera Application\n");
    printf("Xilinx Zynq UltraScale+ MPSoC\n");
    printf("IR Camera over USB3.0\n");
    printf("========================================\n\n");
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 初始化VPSS */
    printf("[1/4] 初始化VPSS...\n");
    if (vpss_init(&vpss, VIDEO_WIDTH, VIDEO_HEIGHT) < 0) {
        fprintf(stderr, "VPSS初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 初始化VDMA */
    printf("\n[2/4] 初始化VDMA...\n");
    if (vdma_init(&vdma, VIDEO_WIDTH, VIDEO_HEIGHT, 
                  BYTES_PER_PIXEL, NUM_FRAMES,
                  FRAME_BUFFER_PHYS) < 0) {
        fprintf(stderr, "VDMA初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 先启动VDMA（接收端） */
    printf("\n[3/4] 启动VDMA...\n");
    if (vdma_start(&vdma) < 0) {
        fprintf(stderr, "VDMA启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 再启动VPSS（发送端） */
    printf("\n[4/4] 启动VPSS...\n");
    usleep(10000);
    if (vpss_start(&vpss) < 0) {
        fprintf(stderr, "VPSS启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 等待数据流稳定 */
    printf("\n等待视频流稳定...\n");
    sleep(1);
    
    /* 初始化UVC */
    printf("\n初始化UVC设备...\n");
    if (uvc_init(UVC_DEVICE) < 0) {
        fprintf(stderr, "UVC初始化失败\n");
        fprintf(stderr, "提示: 请先运行 setup_uvc.sh 配置UVC Gadget\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 主循环 */
    ret = main_loop();
    
cleanup:
    printf("\n清理资源...\n");
    
    if (uvc_fd >= 0) {
        close(uvc_fd);
    }
    
    vpss_cleanup(&vpss);
    vdma_cleanup(&vdma);
    
    printf("程序退出\n");
    return ret;
}
