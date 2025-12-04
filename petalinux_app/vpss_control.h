/**
 * @file vpss_control.h
 * @brief VPSS (Video Processing Subsystem) 控制接口
 * 
 * 此模块提供对VPSS IP核的控制功能，包括：
 * - 通过UIO访问VPSS寄存器
 * - 初始化VPSS
 * - 启动/停止视频处理
 * - 配置颜色空间转换（YUV422 -> RGB）
 */

#ifndef VPSS_CONTROL_H
#define VPSS_CONTROL_H

#include <stdint.h>

/* VPSS基地址（从Vivado Address Editor获取） */
#define VPSS_BASE_ADDR    0x80000000
#define VPSS_ADDR_SIZE    0x10000

/* VPSS寄存器偏移（根据Xilinx VPSS IP核文档） */
#define VPSS_CTRL_REG           0x0000  /* Control Register */
#define VPSS_STATUS_REG         0x0004  /* Status Register */
#define VPSS_ERROR_REG          0x0008  /* Error Register */
#define VPSS_VERSION_REG        0x0010  /* Version Register */

/* Control Register位定义 */
#define VPSS_CTRL_START         (1 << 0)  /* Start processing */
#define VPSS_CTRL_AUTO_RESTART  (1 << 7)  /* Auto restart */

/**
 * VPSS控制结构
 */
typedef struct {
    void *base_addr;      /* 映射后的基地址 */
    int uio_fd;           /* UIO设备文件描述符 */
    int width;            /* 视频宽度 */
    int height;           /* 视频高度 */
} vpss_control_t;

/**
 * 初始化VPSS控制器
 * 
 * @param vpss VPSS控制结构指针
 * @param width 视频宽度（像素）
 * @param height 视频高度（像素）
 * @return 0成功，-1失败
 */
int vpss_init(vpss_control_t *vpss, int width, int height);

/**
 * 启动VPSS处理
 * 
 * @param vpss VPSS控制结构指针
 * @return 0成功，-1失败
 */
int vpss_start(vpss_control_t *vpss);

/**
 * 停止VPSS处理
 * 
 * @param vpss VPSS控制结构指针
 * @return 0成功，-1失败
 */
int vpss_stop(vpss_control_t *vpss);

/**
 * 清理VPSS资源
 * 
 * @param vpss VPSS控制结构指针
 */
void vpss_cleanup(vpss_control_t *vpss);

#endif /* VPSS_CONTROL_H */
