/**
 * @file vpss_control.c
 * @brief VPSS (Video Processing Subsystem) 控制实现
 */

#include "vpss_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

/**
 * 打开UIO设备并映射VPSS寄存器
 * 
 * @param vpss VPSS控制结构指针
 * @return 0成功，-1失败
 */
static int vpss_open_uio(vpss_control_t *vpss)
{
    char uio_name[64];
    char uio_path[128];
    int i;
    
    /* 查找VPSS对应的UIO设备 */
    for (i = 0; i < 10; i++) {
        snprintf(uio_path, sizeof(uio_path), "/sys/class/uio/uio%d/name", i);
        
        int fd = open(uio_path, O_RDONLY);
        if (fd < 0) continue;
        
        ssize_t len = read(fd, uio_name, sizeof(uio_name) - 1);
        close(fd);
        
        if (len > 0) {
            uio_name[len - 1] = '\0';  /* 去掉换行符 */
            
            /* 检查是否是VPSS（支持多种命名方式） */
            if (strstr(uio_name, "v_proc_ss") || 
                strstr(uio_name, "vpss") ||
                strstr(uio_name, "VPSS") ||
                strstr(uio_name, "video_proc")) {
                snprintf(uio_path, sizeof(uio_path), "/dev/uio%d", i);
                vpss->uio_fd = open(uio_path, O_RDWR);
                
                if (vpss->uio_fd < 0) {
                    fprintf(stderr, "打开%s失败: %s\n", uio_path, strerror(errno));
                    return -1;
                }
                
                printf("找到VPSS UIO设备: %s (uio%d)\n", uio_name, i);
                return 0;
            }
        }
    }
    
    fprintf(stderr, "未找到VPSS UIO设备\n");
    fprintf(stderr, "请检查设备树配置和UIO驱动\n");
    fprintf(stderr, "提示: 运行 check_uio.sh 脚本检查UIO设备\n");
    return -1;
}

/**
 * 初始化VPSS
 */
int vpss_init(vpss_control_t *vpss, int width, int height)
{
    printf("初始化VPSS控制器...\n");
    
    memset(vpss, 0, sizeof(vpss_control_t));
    vpss->width = width;
    vpss->height = height;
    vpss->uio_fd = -1;
    
    /* 打开UIO设备 */
    if (vpss_open_uio(vpss) < 0) {
        return -1;
    }
    
    /* 映射VPSS寄存器空间 */
    vpss->base_addr = mmap(NULL, VPSS_ADDR_SIZE, 
                           PROT_READ | PROT_WRITE, 
                           MAP_SHARED, 
                           vpss->uio_fd, 0);
    
    if (vpss->base_addr == MAP_FAILED) {
        fprintf(stderr, "映射VPSS寄存器失败: %s\n", strerror(errno));
        close(vpss->uio_fd);
        return -1;
    }
    
    printf("VPSS寄存器映射成功: %p\n", vpss->base_addr);
    
    /* 读取版本信息 */
    uint32_t version = *(volatile uint32_t*)(vpss->base_addr + VPSS_VERSION_REG);
    printf("VPSS版本: 0x%08X\n", version);
    
    /* 复位VPSS */
    printf("复位VPSS...\n");
    *(volatile uint32_t*)(vpss->base_addr + VPSS_CTRL_REG) = 0;
    usleep(10000);
    
    /* 清除错误寄存器 */
    *(volatile uint32_t*)(vpss->base_addr + VPSS_ERROR_REG) = 0xFFFFFFFF;
    
    printf("VPSS初始化完成\n");
    printf("  分辨率: %dx%d\n", width, height);
    printf("  色彩转换: YUV422 → RGB888\n");
    
    return 0;
}

/**
 * 启动VPSS
 */
int vpss_start(vpss_control_t *vpss)
{
    if (!vpss || !vpss->base_addr) {
        fprintf(stderr, "VPSS未初始化\n");
        return -1;
    }
    
    printf("启动VPSS处理...\n");
    
    /* 设置Control寄存器：启动 + 自动重启 */
    uint32_t ctrl = VPSS_CTRL_START | VPSS_CTRL_AUTO_RESTART;
    *(volatile uint32_t*)(vpss->base_addr + VPSS_CTRL_REG) = ctrl;
    
    /* 等待启动完成 */
    usleep(10000);
    
    /* 检查状态 */
    uint32_t status = *(volatile uint32_t*)(vpss->base_addr + VPSS_STATUS_REG);
    printf("VPSS状态: 0x%08X\n", status);
    
    /* 检查错误 */
    uint32_t error = *(volatile uint32_t*)(vpss->base_addr + VPSS_ERROR_REG);
    if (error != 0) {
        fprintf(stderr, "警告: VPSS错误寄存器: 0x%08X\n", error);
    }
    
    printf("VPSS启动成功\n");
    return 0;
}

/**
 * 停止VPSS
 */
int vpss_stop(vpss_control_t *vpss)
{
    if (!vpss || !vpss->base_addr) {
        return -1;
    }
    
    printf("停止VPSS处理...\n");
    
    /* 清除Start位 */
    *(volatile uint32_t*)(vpss->base_addr + VPSS_CTRL_REG) = 0;
    usleep(10000);
    
    return 0;
}

/**
 * 清理VPSS资源
 */
void vpss_cleanup(vpss_control_t *vpss)
{
    if (!vpss) return;
    
    printf("清理VPSS资源...\n");
    
    vpss_stop(vpss);
    
    if (vpss->base_addr && vpss->base_addr != MAP_FAILED) {
        munmap(vpss->base_addr, VPSS_ADDR_SIZE);
        vpss->base_addr = NULL;
    }
    
    if (vpss->uio_fd >= 0) {
        close(vpss->uio_fd);
        vpss->uio_fd = -1;
    }
}
