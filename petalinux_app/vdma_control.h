/**
 * @file vdma_control.h
 * @brief VDMA (Video DMA) 控制接口
 * 
 * 此模块提供对VDMA IP核的控制功能，包括：
 * - 通过UIO访问VDMA寄存器
 * - 初始化VDMA
 * - 配置帧缓冲地址
 * - 启动/停止DMA传输
 * - 获取当前写入的帧编号
 */

#ifndef VDMA_CONTROL_H
#define VDMA_CONTROL_H

#include <stdint.h>
#include <stddef.h>
/* VDMA基地址（从Vivado Address Editor获取） */
#define VDMA_BASE_ADDR    0x80020000
#define VDMA_ADDR_SIZE    0x10000

/* VDMA S2MM (Stream to Memory Mapped) 寄存器偏移 */
#define VDMA_S2MM_CONTROL       0x30
#define VDMA_S2MM_STATUS        0x34
#define VDMA_S2MM_VSIZE         0xA0
#define VDMA_S2MM_HSIZE         0xA4
#define VDMA_S2MM_STRIDE        0xA8
#define VDMA_S2MM_START_ADDR1   0xAC
#define VDMA_S2MM_START_ADDR2   0xB0
#define VDMA_S2MM_START_ADDR3   0xB4

/* Control Register位定义 */
#define VDMA_CTRL_RUN           (1 << 0)
#define VDMA_CTRL_CIRCULAR      (1 << 1)
#define VDMA_CTRL_RESET         (1 << 2)
#define VDMA_CTRL_GENLOCK       (1 << 3)

/* Status Register位定义 */
#define VDMA_STATUS_HALTED      (1 << 0)
#define VDMA_STATUS_IDLE        (1 << 1)

/**
 * VDMA控制结构
 */
typedef struct {
    void *base_addr;              /* 映射后的基地址 */
    int uio_fd;                   /* UIO设备文件描述符 */
    void *frame_buffer;           /* 帧缓冲映射地址 */
    uint32_t frame_buffer_phys;   /* 帧缓冲物理地址 */
    size_t frame_buffer_size;     /* 帧缓冲总大小 */
    int width;                    /* 视频宽度 */
    int height;                   /* 视频高度 */
    int bytes_per_pixel;          /* 每像素字节数 */
    int num_frames;               /* 帧缓冲数量 */
} vdma_control_t;

/**
 * 初始化VDMA控制器
 * 
 * @param vdma VDMA控制结构指针
 * @param width 视频宽度（像素）
 * @param height 视频高度（像素）
 * @param bytes_per_pixel 每像素字节数（本项目YUV422/YUYV为2）
 * @param num_frames 帧缓冲数量（建议3个，用于三缓冲）
 * @param frame_buffer_phys 帧缓冲物理地址（0x20000000，与设备树reserved-memory一致）
 * @return 0成功，-1失败
 */
int vdma_init(vdma_control_t *vdma, int width, int height, 
              int bytes_per_pixel, int num_frames,
              uint32_t frame_buffer_phys);

/**
 * 启动VDMA
 * 
 * @param vdma VDMA控制结构指针
 * @return 0成功，-1失败
 */
int vdma_start(vdma_control_t *vdma);

/**
 * 停止VDMA
 * 
 * @param vdma VDMA控制结构指针
 * @return 0成功，-1失败
 */
int vdma_stop(vdma_control_t *vdma);

/**
 * 获取当前VDMA正在写入的帧编号
 * 
 * @param vdma VDMA控制结构指针
 * @return 帧编号（0, 1, 2...），失败返回-1
 */
int vdma_get_current_frame(vdma_control_t *vdma);

/**
 * 清理VDMA资源
 * 
 * @param vdma VDMA控制结构指针
 */
void vdma_cleanup(vdma_control_t *vdma);

#endif /* VDMA_CONTROL_H */
