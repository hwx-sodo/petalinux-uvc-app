/**
 * @file vdma_control.h
 * @brief VDMA (Video DMA) 控制接口
 * 
 * 数据流架构：
 *   CameraLink(PL, portA[7:0]+portB[7:0] → 16-bit)
 *     → Video In to AXI4-Stream (16-bit)
 *     → AXI4-Stream Data Width Converter (16-bit → 32-bit)
 *     → VDMA S2MM (32-bit总线写入DDR)
 *     → DDR (YUV422/YUYV字节序存储)
 *     → 网络传输 → PC(OpenCV解码显示)
 * 
 * 视频格式：
 *   - 分辨率: 640x480 @ 60fps
 *   - 像素格式: YUV422 (YUYV), 2字节/像素
 *   - 帧大小: 640 * 480 * 2 = 614,400 bytes
 */

#ifndef VDMA_CONTROL_H
#define VDMA_CONTROL_H

#include <stdint.h>
#include <stddef.h>

/*============================================================================
 * 硬件地址配置 (根据Vivado Address Editor)
 *============================================================================*/

/** VDMA IP核基地址 */
#define VDMA_BASE_ADDR          0x80020000

/** VDMA寄存器空间大小 */
#define VDMA_REG_SIZE           0x10000

/** 帧缓冲物理地址 (对应设备树 reserved-memory) */
#define FRAME_BUFFER_BASE       0x20000000

/** 帧缓冲总空间 (512MB, 0x20000000 ~ 0x40000000) */
#define FRAME_BUFFER_TOTAL_SIZE 0x20000000

/*============================================================================
 * 视频参数配置
 *============================================================================*/

/** 视频宽度 (像素) */
#define VIDEO_WIDTH             640

/** 视频高度 (像素) */
#define VIDEO_HEIGHT            480

/** 每像素字节数 (YUV422 = 2) */
#define BYTES_PER_PIXEL         2

/** 帧缓冲数量 (单缓冲) */
#define NUM_FRAME_BUFFERS       1

/** 每帧字节数 */
#define FRAME_SIZE_BYTES        (VIDEO_WIDTH * VIDEO_HEIGHT * BYTES_PER_PIXEL)

/** 每行字节数 (HSize) */
#define LINE_SIZE_BYTES         (VIDEO_WIDTH * BYTES_PER_PIXEL)

/*============================================================================
 * VDMA S2MM (Stream to Memory Mapped) 寄存器偏移
 * 参考: Xilinx PG020 - AXI VDMA v6.3
 *============================================================================*/

/* S2MM 控制寄存器 */
#define VDMA_S2MM_DMACR         0x30    /* S2MM DMA Control Register */
#define VDMA_S2MM_DMASR         0x34    /* S2MM DMA Status Register */
#define VDMA_S2MM_REG_INDEX     0x44    /* S2MM Register Index */
#define VDMA_S2MM_FRMSTORE      0x48    /* S2MM Frame Store */
#define VDMA_S2MM_THRESHOLD     0x4C    /* S2MM Line Buffer Threshold */

/* S2MM 帧参数寄存器 */
#define VDMA_S2MM_VSIZE         0xA0    /* S2MM Vertical Size (行数) */
#define VDMA_S2MM_HSIZE         0xA4    /* S2MM Horizontal Size (每行字节数) */
#define VDMA_S2MM_FRMDLY_STRIDE 0xA8    /* S2MM Frame Delay / Stride */

/* S2MM 帧缓冲起始地址 (支持最多32个缓冲) */
#define VDMA_S2MM_START_ADDR_0  0xAC    /* 帧缓冲0起始地址 */
#define VDMA_S2MM_START_ADDR_1  0xB0    /* 帧缓冲1起始地址 */
#define VDMA_S2MM_START_ADDR_2  0xB4    /* 帧缓冲2起始地址 */
#define VDMA_S2MM_START_ADDR_3  0xB8    /* 帧缓冲3起始地址 */

/*============================================================================
 * VDMA S2MM 控制寄存器 (DMACR 0x30) 位定义
 *============================================================================*/

#define VDMA_DMACR_RS           (1 << 0)    /* Run/Stop: 1=运行, 0=停止 */
#define VDMA_DMACR_CIRCULAR     (1 << 1)    /* Circular Buffer Mode */
#define VDMA_DMACR_RESET        (1 << 2)    /* Soft Reset */
#define VDMA_DMACR_GENLOCK_EN   (1 << 3)    /* GenLock Enable */
#define VDMA_DMACR_FRMCNT_EN    (1 << 4)    /* Frame Count Enable */
#define VDMA_DMACR_SYNC_EN      (1 << 15)   /* GenLock Sync Enable */

/* IRQ使能位 */
#define VDMA_DMACR_FRMCNT_IRQ   (1 << 12)   /* Frame Count IRQ Enable */
#define VDMA_DMACR_DLY_IRQ      (1 << 13)   /* Delay Timer IRQ Enable */
#define VDMA_DMACR_ERR_IRQ      (1 << 14)   /* Error IRQ Enable */

/*============================================================================
 * VDMA S2MM 状态寄存器 (DMASR 0x34) 位定义
 *============================================================================*/

#define VDMA_DMASR_HALTED       (1 << 0)    /* Channel Halted */
#define VDMA_DMASR_IDLE         (1 << 1)    /* Channel Idle */
#define VDMA_DMASR_SGINCLD      (1 << 3)    /* Scatter Gather Included */
#define VDMA_DMASR_DMA_INT_ERR  (1 << 4)    /* DMA Internal Error */
#define VDMA_DMASR_DMA_SLV_ERR  (1 << 5)    /* DMA Slave Error */
#define VDMA_DMASR_DMA_DEC_ERR  (1 << 6)    /* DMA Decode Error */
#define VDMA_DMASR_SOF_EARLY    (1 << 7)    /* Start of Frame Early Error */
#define VDMA_DMASR_EOL_EARLY    (1 << 8)    /* End of Line Early Error */
#define VDMA_DMASR_SOF_LATE     (1 << 11)   /* Start of Frame Late Error */
#define VDMA_DMASR_EOL_LATE     (1 << 12)   /* End of Line Late Error */
#define VDMA_DMASR_FRMCNT_IRQ   (1 << 12)   /* Frame Count IRQ */
#define VDMA_DMASR_DLY_IRQ      (1 << 13)   /* Delay Timer IRQ */
#define VDMA_DMASR_ERR_IRQ      (1 << 14)   /* Error IRQ */

/* Frame Count: bits [23:16] */
#define VDMA_DMASR_FRMCNT_MASK  0x00FF0000
#define VDMA_DMASR_FRMCNT_SHIFT 16

/* Delay Count: bits [31:24] */
#define VDMA_DMASR_DLYCNT_MASK  0xFF000000
#define VDMA_DMASR_DLYCNT_SHIFT 24

/* 错误位掩码 */
#define VDMA_DMASR_ERR_MASK     (VDMA_DMASR_DMA_INT_ERR | VDMA_DMASR_DMA_SLV_ERR | \
                                 VDMA_DMASR_DMA_DEC_ERR | VDMA_DMASR_SOF_EARLY | \
                                 VDMA_DMASR_EOL_EARLY | VDMA_DMASR_SOF_LATE | \
                                 VDMA_DMASR_EOL_LATE)

/*============================================================================
 * 数据结构
 *============================================================================*/

/**
 * VDMA控制器上下文结构
 */
typedef struct {
    /* 映射后的虚拟地址 */
    volatile void *regs;            /* VDMA寄存器基址 (mmap后) */
    void *frame_buffers;            /* 帧缓冲区基址 (mmap后) */
    
    /* 物理地址 */
    uint32_t frame_buffer_phys;     /* 帧缓冲物理基址 */
    
    /* 文件描述符 */
    int uio_fd;                     /* UIO设备文件描述符 */
    int mem_fd;                     /* /dev/mem 文件描述符 */
    
    /* 视频参数 */
    int width;                      /* 图像宽度 (像素) */
    int height;                     /* 图像高度 (像素) */
    int bytes_per_pixel;            /* 每像素字节数 */
    int num_buffers;                /* 帧缓冲数量 */
    size_t frame_size;              /* 单帧大小 (字节) */
    size_t line_stride;             /* 行跨度 (字节) */
    
    /* 运行状态 */
    int is_running;                 /* VDMA是否正在运行 */
    int current_frame;              /* 当前帧索引 */
} vdma_context_t;

/*============================================================================
 * API函数声明
 *============================================================================*/

/**
 * 初始化VDMA控制器
 * 
 * @param ctx       VDMA上下文指针
 * @param width     图像宽度 (像素)
 * @param height    图像高度 (像素)
 * @param bpp       每像素字节数 (YUV422=2)
 * @param num_bufs  帧缓冲数量 (当前为1)
 * @param phys_addr 帧缓冲物理地址
 * @return 0成功, -1失败
 */
int vdma_init(vdma_context_t *ctx, 
              int width, int height, int bpp, 
              int num_bufs, uint32_t phys_addr);

/**
 * 启动VDMA传输
 * 
 * @param ctx VDMA上下文指针
 * @return 0成功, -1失败
 */
int vdma_start(vdma_context_t *ctx);

/**
 * 停止VDMA传输
 * 
 * @param ctx VDMA上下文指针
 * @return 0成功, -1失败
 */
int vdma_stop(vdma_context_t *ctx);

/**
 * 复位VDMA
 * 
 * @param ctx VDMA上下文指针
 * @return 0成功, -1失败
 */
int vdma_reset(vdma_context_t *ctx);

/**
 * 获取当前VDMA正在写入的帧索引
 * 
 * @param ctx VDMA上下文指针
 * @return 帧索引 (0 ~ num_bufs-1), -1表示错误
 */
int vdma_get_write_frame(vdma_context_t *ctx);

/**
 * 获取可安全读取的帧缓冲指针
 * (返回非当前写入帧)
 * 
 * @param ctx VDMA上下文指针
 * @param frame_index 输出：实际读取的帧索引
 * @return 帧数据指针, NULL表示错误
 */
const uint8_t* vdma_get_read_buffer(vdma_context_t *ctx, int *frame_index);

/**
 * 获取指定帧缓冲的指针
 * 
 * @param ctx VDMA上下文指针
 * @param index 帧索引
 * @return 帧数据指针, NULL表示错误
 */
const uint8_t* vdma_get_frame_buffer(vdma_context_t *ctx, int index);

/**
 * 打印VDMA寄存器状态 (用于调试)
 * 
 * @param ctx VDMA上下文指针
 */
void vdma_dump_registers(vdma_context_t *ctx);

/**
 * 打印帧缓冲内容摘要 (用于调试)
 * 
 * @param ctx VDMA上下文指针
 * @param frame_index 帧索引
 */
void vdma_dump_frame_info(vdma_context_t *ctx, int frame_index);

/**
 * 释放VDMA资源
 * 
 * @param ctx VDMA上下文指针
 */
void vdma_cleanup(vdma_context_t *ctx);

/**
 * 获取VDMA状态字符串
 * 
 * @param ctx VDMA上下文指针
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return buf指针
 */
char* vdma_get_status_string(vdma_context_t *ctx, char *buf, size_t buf_size);

#endif /* VDMA_CONTROL_H */
