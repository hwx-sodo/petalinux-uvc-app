#!/bin/bash
# setup_rgba_fixed_v2.sh - UVC Gadget RGBA 修复版配置脚本 v2
# 
# 修复内容：
# 1. 添加了 High Speed (hs) 链接
# 2. 增加了详细的调试信息
# 3. 检查 USB 控制器状态
# 4. 确保正确的 ConfigFS 目录结构

set -e  # 遇到错误立即停止

# ================= 变量定义 =================
CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget/g1"
FUNCTION="$GADGET/functions/uvc.0"
CONFIG="$GADGET/configs/c.1"

# USB ID (Linux Foundation)
VENDOR_ID="0x1d6b"
PRODUCT_ID="0x0104"

# 视频参数 (RGBA)
WIDTH=640
HEIGHT=480
FORMAT_NAME="u"       # 格式目录名（标准用 u）
FRAME_NAME="480p"     # 帧名
BPP=32
# 帧大小 = 640 * 480 * 4 = 1,228,800 bytes
FRAME_SIZE=$((WIDTH * HEIGHT * 4))
# ===========================================

print_step() {
    echo ""
    echo "[$1] $2"
}

print_info() {
    echo "  -> $1"
}

print_ok() {
    echo "  ✅ $1"
}

print_err() {
    echo "  ❌ $1"
}

echo "========================================="
echo "   UVC Gadget RGBA 修复版配置脚本 v2"
echo "========================================="

# 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    print_err "请使用 root 权限运行此脚本"
    echo "用法: sudo $0"
    exit 1
fi

# 0. 检查 ConfigFS
print_step "0/7" "检查 ConfigFS..."
if [ ! -d "$CONFIGFS" ]; then
    print_info "挂载 ConfigFS"
    mount -t configfs none $CONFIGFS
fi

if [ ! -d "$CONFIGFS/usb_gadget" ]; then
    print_err "ConfigFS usb_gadget 不可用"
    echo "请检查内核是否启用了 USB Gadget 支持"
    exit 1
fi
print_ok "ConfigFS 可用"

# 1. 检查 UDC 控制器
print_step "1/7" "检查 USB 控制器..."
if [ ! -d "/sys/class/udc" ]; then
    print_err "未找到 /sys/class/udc 目录"
    echo "内核可能没有启用 USB Device Controller"
    exit 1
fi

UDC_NAME=$(ls /sys/class/udc 2>/dev/null | head -n1)
if [ -z "$UDC_NAME" ]; then
    print_err "未找到可用的 UDC 控制器"
    echo ""
    echo "可能的原因:"
    echo "  1. Vivado 中 USB 未配置为 Device/Peripheral 模式"
    echo "  2. 设备树中 dwc3 的 dr_mode 不是 peripheral"
    echo "  3. USB 驱动未正确加载"
    exit 1
fi

print_ok "找到 UDC: $UDC_NAME"

# 显示 UDC 状态
UDC_STATE=$(cat /sys/class/udc/$UDC_NAME/state 2>/dev/null || echo "unknown")
print_info "UDC 状态: $UDC_STATE"

# 检查是否有 OTG 模式冲突
if [ -f "/sys/class/udc/$UDC_NAME/device/role" ]; then
    ROLE=$(cat "/sys/class/udc/$UDC_NAME/device/role" 2>/dev/null || echo "unknown")
    print_info "USB 角色: $ROLE"
fi

# 2. 环境清理
print_step "2/7" "清理旧配置..."
if [ -d "$GADGET" ]; then
    # 解绑 UDC
    if [ -f "$GADGET/UDC" ]; then
        CURRENT_UDC=$(cat "$GADGET/UDC" 2>/dev/null)
        if [ -n "$CURRENT_UDC" ]; then
            print_info "解绑现有 UDC: $CURRENT_UDC"
            echo "" > "$GADGET/UDC" 2>/dev/null || true
            sleep 1
        fi
    fi
    
    # 调用清理脚本
    if [ -f "/cleanup_gadget.sh" ]; then
        /cleanup_gadget.sh > /dev/null 2>&1 || true
    elif [ -f "$(dirname $0)/cleanup_gadget.sh" ]; then
        "$(dirname $0)/cleanup_gadget.sh" > /dev/null 2>&1 || true
    fi
    sleep 1
fi

if [ -d "$GADGET" ]; then
    print_err "无法完全清理旧配置，尝试继续..."
else
    print_ok "旧配置已清理"
fi

# 3. 创建 Gadget 基础结构
print_step "3/7" "创建 Gadget 基础结构..."
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
print_ok "基础结构创建完成"

# 4. 创建 Config
print_step "4/7" "创建 USB 配置..."
mkdir -p $CONFIG
mkdir -p $CONFIG/strings/0x409
echo "UVC Config" > $CONFIG/strings/0x409/configuration
echo 500 > $CONFIG/MaxPower
print_ok "配置创建完成"

# 5. 配置 UVC Function
print_step "5/7" "配置 UVC Function..."
mkdir -p $FUNCTION

# 5.1 控制接口 (Control Interface) - 必须先配置
print_info "配置 Control 接口..."
mkdir -p $FUNCTION/control/header/h
# Control 类链接 (fs 和 ss)
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/fs/h 2>/dev/null || true
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/ss/h 2>/dev/null || true

# 5.2 Streaming 接口
print_info "配置 Streaming 接口..."

# 创建格式目录 (使用标准名称 'u')
mkdir -p $FUNCTION/streaming/uncompressed/$FORMAT_NAME

# 设置 RGBA 格式 GUID
# BA81EB33-49C3-4F3E-9B5D-BA1D5E004344 = RGB32 (RGBX/RGBA)
echo "{ba81eb33-49c3-4f3e-9b5d-ba1d5e004344}" > $FUNCTION/streaming/uncompressed/$FORMAT_NAME/guidFormat 2>/dev/null || {
    print_info "GUID 设置跳过 (可能是只读)"
}

# 设置 BPP (位每像素)
echo $BPP > $FUNCTION/streaming/uncompressed/$FORMAT_NAME/bBitsPerPixel 2>/dev/null || {
    print_info "BPP 设置跳过 (可能是只读)"
}

# 创建帧配置
mkdir -p $FUNCTION/streaming/uncompressed/$FORMAT_NAME/$FRAME_NAME

# 设置帧参数
FRAME_DIR="$FUNCTION/streaming/uncompressed/$FORMAT_NAME/$FRAME_NAME"
echo $WIDTH > $FRAME_DIR/wWidth
echo $HEIGHT > $FRAME_DIR/wHeight
echo $FRAME_SIZE > $FRAME_DIR/dwMaxVideoFrameBufferSize
echo 166666 > $FRAME_DIR/dwDefaultFrameInterval  # 60fps

# 比特率计算
BIT_RATE=$((FRAME_SIZE * 8 * 60))
MIN_BIT_RATE=$((FRAME_SIZE * 8 * 15))
echo $MIN_BIT_RATE > $FRAME_DIR/dwMinBitRate
echo $BIT_RATE > $FRAME_DIR/dwMaxBitRate

# 设置支持的帧率
echo 166666 > $FRAME_DIR/dwFrameInterval  # 60fps

print_info "格式: RGBA (32-bit)"
print_info "分辨率: ${WIDTH}x${HEIGHT}"
print_info "帧大小: $FRAME_SIZE bytes"
print_ok "UVC Function 配置完成"

# 6. 链接 Header
print_step "6/7" "链接 Header..."

# 创建 streaming header
mkdir -p $FUNCTION/streaming/header/h

# 链接格式到 header
ln -s $FUNCTION/streaming/uncompressed/$FORMAT_NAME $FUNCTION/streaming/header/h/$FORMAT_NAME 2>/dev/null || true

# 链接到各个速度类 (fs=Full Speed, hs=High Speed, ss=Super Speed)
# 关键修复：添加 hs 链接！
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/fs/h 2>/dev/null || true
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/hs/h 2>/dev/null || true
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/ss/h 2>/dev/null || true

print_ok "Header 链接完成 (fs/hs/ss)"

# 7. 绑定 Function 到 Config
print_step "7/7" "绑定并启动..."

# 链接 function 到 config
ln -s $FUNCTION $CONFIG/uvc.0 2>/dev/null || {
    print_info "Function 链接已存在"
}

# 启动 Gadget (绑定 UDC)
print_info "绑定 UDC: $UDC_NAME"

# 添加调试：检查当前配置
echo ""
echo "调试信息："
echo "  Gadget 目录: $GADGET"
echo "  Function: $(ls -la $CONFIG/uvc.0 2>/dev/null | awk '{print $NF}' || echo 'not linked')"
echo "  UDC 列表: $(ls /sys/class/udc)"
echo ""

# 尝试绑定
if echo $UDC_NAME > $GADGET/UDC 2>&1; then
    print_ok "成功绑定到 UDC: $UDC_NAME"
    
    echo ""
    echo "========================================="
    echo "✅ UVC Gadget 配置成功!"
    echo "========================================="
    echo ""
    echo "配置摘要:"
    echo "  分辨率: ${WIDTH}x${HEIGHT}"
    echo "  格式: RGBA (32-bit)"
    echo "  帧大小: $FRAME_SIZE bytes"
    echo "  UDC: $UDC_NAME"
    echo ""
    
    # 检查视频设备
    sleep 1
    if [ -e /dev/video0 ]; then
        echo "视频设备: /dev/video0 已创建"
    else
        echo "⚠️  注意: /dev/video0 尚未创建"
        echo "   可能需要将 USB 连接到主机"
    fi
    echo ""
    echo "下一步:"
    echo "  1. 用 USB 线连接开发板到电脑"
    echo "  2. 运行视频流应用程序"
    echo ""
else
    print_err "绑定 UDC 失败"
    
    echo ""
    echo "========================================="
    echo "❌ UVC Gadget 配置失败"
    echo "========================================="
    echo ""
    echo "请检查以下内容:"
    echo ""
    echo "1. 查看内核日志获取详细错误:"
    echo "   dmesg | tail -30"
    echo ""
    echo "2. 检查 UDC 状态:"
    echo "   cat /sys/class/udc/$UDC_NAME/state"
    echo ""
    echo "3. 检查是否有 USB OTG 模式冲突:"
    echo "   - 确保设备树中 dr_mode = \"peripheral\""
    echo "   - 检查 USB PHY 是否正确初始化"
    echo ""
    echo "4. 验证 ConfigFS 配置:"
    echo "   ls -la $GADGET/"
    echo "   ls -la $FUNCTION/streaming/"
    echo ""
    echo "5. 常见解决方法:"
    echo "   - 重启开发板"
    echo "   - 检查 USB 控制器是否被其他驱动占用"
    echo "   - 确认固件中 USB 配置正确"
    echo ""
    
    # 自动显示 dmesg
    echo "最近的内核日志:"
    echo "----------------------------------------"
    dmesg | tail -20
    echo "----------------------------------------"
    
    exit 1
fi
