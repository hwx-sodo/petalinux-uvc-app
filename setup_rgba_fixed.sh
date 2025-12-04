#!/bin/bash
set -e  # 遇到错误立即停止，绝不带病上线

# 重新定义变量
GADGET="/sys/kernel/config/usb_gadget/g1"
FUNCTION="$GADGET/functions/uvc.0"
WIDTH=640
HEIGHT=480
FORMAT="rgba"
FRAME_SIZE=$((WIDTH * HEIGHT * 4)) # 640*480*4 = 1228800

# 先清理环境
/cleanup_gadget.sh || true
sleep 1

echo "正在配置 RGBA 模式 (修复版)..."

# 1. 创建基础目录
mkdir -p $FUNCTION

# 2. 配置流接口 (关键顺序调整！)
mkdir -p $FUNCTION/streaming/uncompressed/u/$FORMAT

# 【关键步骤 A】先写入 GUID
echo "{ba81eb33-49c3-4f3e-9b5d-ba1d5e004344}" > $FUNCTION/streaming/uncompressed/u/$FORMAT/guidFormat

# 【关键步骤 B】在创建分辨率子目录之前，立刻写入 bBitsPerPixel！
# 只要没创建子目录，这里是可以写入的
echo 32 > $FUNCTION/streaming/uncompressed/u/$FORMAT/bBitsPerPixel

# 【关键步骤 C】然后再创建分辨率目录
mkdir -p $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p

# 写入其他参数
echo $WIDTH > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/wWidth
echo $HEIGHT > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/wHeight
echo 166666 > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/dwDefaultFrameInterval
echo $FRAME_SIZE > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/dwMaxVideoFrameBufferSize
echo $((FRAME_SIZE * 8 * 60)) > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/dwMaxBitRate
echo $((FRAME_SIZE * 8 * 60)) > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/dwMinBitRate
echo 166666 > $FUNCTION/streaming/uncompressed/u/$FORMAT/${HEIGHT}p/dwFrameInterval

# 3. 链接 Header (标准流程)
mkdir -p $FUNCTION/streaming/header/h
ln -s $FUNCTION/streaming/uncompressed/u $FUNCTION/streaming/header/h/u
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/fs/h
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/ss/h

# 4. 绑定 UDC
echo "绑定 UDC..."
UDC_NAME=$(ls /sys/class/udc | head -n1)
echo $UDC_NAME > $GADGET/UDC

echo "✅ 配置完成，bBitsPerPixel 已强制设为 32"