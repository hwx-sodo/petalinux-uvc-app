#!/bin/bash
set -e # 遇到错误立即停止

# ================= 变量定义 =================
CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget/g1"
FUNCTION="$GADGET/functions/uvc.0"
CONFIG="$GADGET/configs/c.1"

# USB ID (Xilinx)
VENDOR_ID="0x1d6b"
PRODUCT_ID="0x0104"

# 视频参数 (RGBA)
WIDTH=640
HEIGHT=480
FORMAT_NAME="rgba"   # 格式名
FRAME_NAME="480p"    # 帧名
BPP=32
# 帧大小 = 640 * 480 * 4 = 1,228,800 bytes
FRAME_SIZE=$((WIDTH * HEIGHT * 4))
# ===========================================

echo "========================================="
echo "   UVC Gadget RGBA 修复版配置脚本"
echo "========================================="

# 0. 环境清理
if [ -d "$GADGET" ]; then
    echo "清理旧配置..."
    /cleanup_gadget.sh > /dev/null 2>&1 || true
    sleep 1
fi

echo "[1/6] 创建 Gadget 基础结构..."
mkdir -p $GADGET
echo $VENDOR_ID > $GADGET/idVendor
echo $PRODUCT_ID > $GADGET/idProduct
echo 0x0200 > $GADGET/bcdUSB
echo 0xef > $GADGET/bDeviceClass
echo 0x02 > $GADGET/bDeviceSubClass
echo 0x01 > $GADGET/bDeviceProtocol

mkdir -p $GADGET/strings/0x409
echo "Xilinx" > $GADGET/strings/0x409/manufacturer
echo "ZynqMP UVC Camera" > $GADGET/strings/0x409/product
echo "0001" > $GADGET/strings/0x409/serialnumber

echo "[2/6] 创建 Config (这是之前漏掉的)..."
mkdir -p $CONFIG
mkdir -p $CONFIG/strings/0x409
echo "UVC Config" > $CONFIG/strings/0x409/configuration
echo 500 > $CONFIG/MaxPower

echo "[3/6] 配置 UVC Function (关键顺序)..."
mkdir -p $FUNCTION

# 3.1 创建格式目录 (Format) - 只创建这一层！
mkdir -p $FUNCTION/streaming/uncompressed/$FORMAT_NAME

# 3.2 立即配置 Format 属性 (绕过 Permission denied)
echo "{ba81eb33-49c3-4f3e-9b5d-ba1d5e004344}" > $FUNCTION/streaming/uncompressed/$FORMAT_NAME/guidFormat
echo $BPP > $FUNCTION/streaming/uncompressed/$FORMAT_NAME/bBitsPerPixel
echo "  -> RGBA GUID 和 BPP($BPP) 设置成功"

# 3.3 创建帧目录 (Frame)
mkdir -p $FUNCTION/streaming/uncompressed/$FORMAT_NAME/$FRAME_NAME

# 3.4 配置帧参数
FRAME_DIR="$FUNCTION/streaming/uncompressed/$FORMAT_NAME/$FRAME_NAME"
echo $WIDTH > $FRAME_DIR/wWidth
echo $HEIGHT > $FRAME_DIR/wHeight
echo $FRAME_SIZE > $FRAME_DIR/dwMaxVideoFrameBufferSize
echo 166666 > $FRAME_DIR/dwDefaultFrameInterval
echo $((FRAME_SIZE * 8 * 60)) > $FRAME_DIR/dwMaxBitRate
echo $((FRAME_SIZE * 8 * 60)) > $FRAME_DIR/dwMinBitRate
echo 166666 > $FRAME_DIR/dwFrameInterval

echo "[4/6] 链接 Header..."
mkdir -p $FUNCTION/streaming/header/h
# 注意：链接的是 Format 目录
ln -s $FUNCTION/streaming/uncompressed/$FORMAT_NAME $FUNCTION/streaming/header/h/$FORMAT_NAME
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/fs/h
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/hs/h
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/ss/h

# 修复 Control Header (原脚本也有这部分)
mkdir -p $FUNCTION/control/header/h
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/fs/h
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/ss/h

echo "[5/6] 绑定 Function 到 Config..."
# 这一步非常重要，没有它就会报 Need at least one configuration
ln -s $FUNCTION $CONFIG/uvc.0

echo "[6/6] 启动 USB Gadget..."
# 查找并绑定 UDC
UDC_NAME=$(ls /sys/class/udc | head -n1)
if [ -z "$UDC_NAME" ]; then
    echo "❌ 错误: 未找到 UDC 控制器"
    exit 1
fi

echo $UDC_NAME > $GADGET/UDC
echo "✅ 成功绑定到 UDC: $UDC_NAME"