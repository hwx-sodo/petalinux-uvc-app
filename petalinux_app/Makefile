# Makefile for UVC Camera and Network Stream Applications
# PetaLinux/Yocto 环境专用版本
# 
# 重要说明：
# 1. 本Makefile不设置CFLAGS和LDFLAGS，完全使用Recipe通过环境变量传递的值
# 2. Recipe中的CFLAGS已经包含了系统头文件路径（如-I/usr/include等）
# 3. 编译选项（如-Wall -O2 -std=c99）在Recipe中追加到CFLAGS
#
# 目标程序：
#   uvc-camera-app     - USB UVC视频传输
#   network-stream-app - 网络视频流传输

# 目标程序名
TARGET_UVC = uvc-camera-app
TARGET_NET = network-stream-app

# 共享源文件
COMMON_SRCS = vpss_control.c vdma_control.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

# UVC应用源文件
UVC_SRCS = main.c
UVC_OBJS = $(UVC_SRCS:.c=.o)

# 网络应用源文件
NET_SRCS = network_stream.c
NET_OBJS = $(NET_SRCS:.c=.o)

# 默认目标：编译所有程序
all: $(TARGET_UVC) $(TARGET_NET)

# 仅编译UVC应用
uvc: $(TARGET_UVC)

# 仅编译网络应用
network: $(TARGET_NET)

# 链接UVC程序
$(TARGET_UVC): $(UVC_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "编译完成: $(TARGET_UVC)"

# 链接网络程序
$(TARGET_NET): $(NET_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "编译完成: $(TARGET_NET)"

# 编译源文件
# CFLAGS由Recipe通过环境变量传递，包含：
# - 系统头文件路径（-I/usr/include等）
# - 编译选项（-Wall -O2 -std=c99等）
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(TARGET_UVC) $(TARGET_NET) $(COMMON_OBJS) $(UVC_OBJS) $(NET_OBJS)
	@echo "清理完成"

# 安装所有程序
install: install-uvc install-network

# 安装UVC程序
install-uvc: $(TARGET_UVC)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET_UVC) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET_UVC)"

# 安装网络程序
install-network: $(TARGET_NET)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET_NET) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET_NET)"

.PHONY: all uvc network clean install install-uvc install-network