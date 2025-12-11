# Makefile for Ethernet Camera Application
# 支持 PetaLinux/Yocto 环境和本地编译
# 
# 使用方法：
#   make           - 编译所有程序
#   make all       - 编译所有程序
#   make diag      - 只编译诊断工具
#   make clean     - 清理编译产物
#   make install   - 安装到目标系统
#
# 目标程序：
#   eth-camera-app - 以太网视频流传输
#   video-diag     - 视频处理链路诊断工具

# 编译器设置（如果环境变量未定义，使用默认值）
CC ?= gcc
CFLAGS ?= -Wall -O2 -std=c99
LDFLAGS ?=

# 目标程序名
TARGET = eth-camera-app
DIAG_TARGET = video-diag

# 源文件
SRCS = network_stream.c vpss_control.c vdma_control.c
OBJS = $(SRCS:.c=.o)

# 诊断工具源文件
DIAG_SRCS = video_diag.c
DIAG_OBJS = $(DIAG_SRCS:.c=.o)

# 头文件依赖
DEPS = vpss_control.h vdma_control.h

# 默认目标
all: $(TARGET) $(DIAG_TARGET)

# 链接主程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "========================================="
	@echo "编译完成: $(TARGET)"
	@echo "========================================="

# 链接诊断工具
$(DIAG_TARGET): $(DIAG_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "========================================="
	@echo "编译完成: $(DIAG_TARGET)"
	@echo "========================================="

# 只编译诊断工具
diag: $(DIAG_TARGET)

# 编译源文件
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

video_diag.o: video_diag.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(TARGET) $(DIAG_TARGET) $(OBJS) $(DIAG_OBJS)
	@echo "清理完成"

# 安装程序
install: $(TARGET) $(DIAG_TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET) $(DESTDIR)/usr/bin/
	install -m 0755 $(DIAG_TARGET) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET)"
	@echo "安装完成: $(DESTDIR)/usr/bin/$(DIAG_TARGET)"

# 显示帮助信息
help:
	@echo "可用目标:"
	@echo "  all     - 编译所有程序 ($(TARGET), $(DIAG_TARGET))"
	@echo "  diag    - 只编译诊断工具 ($(DIAG_TARGET))"
	@echo "  clean   - 清理编译产物"
	@echo "  install - 安装到 \$$(DESTDIR)/usr/bin/"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "编译示例:"
	@echo "  make                    # 使用默认编译器"
	@echo "  make CC=aarch64-linux-gnu-gcc  # 使用交叉编译器"
	@echo ""
	@echo "诊断工具使用:"
	@echo "  ./video-diag -a         # 显示所有诊断信息"
	@echo "  ./video-diag -s frame.bin  # 保存帧数据到文件"
	@echo "  ./video-diag -w         # 持续监控VDMA状态"

.PHONY: all diag clean install help
