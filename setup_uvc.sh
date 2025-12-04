#!/bin/bash
# setup_uvc.sh - UVC Gadget 配置脚本 (修正版)
# 这个脚本需要在开发板上运行

echo "========================================="
echo "       UVC Gadget 配置工具 v2.0"
echo "========================================="

# 1. 基础配置参数
VENDOR_ID="0x1d6b"    # Linux Foundation
PRODUCT_ID="0x0104"   # Multifunction Composite Gadget
MANUFACTURER="Xilinx"
PRODUCT="ZynqMP UVC Camera"
SERIAL="0123456789"

WIDTH=640
HEIGHT=480
FORMAT="rgba"         # RGBA 32位格式

# ConfigFS 路径
CONFIGFS="/sys/kernel/config"
GADGET_NAME="g1"
GADGET="$CONFIGFS/usb_gadget/$GADGET_NAME"
FUNCTION="$GADGET/functions/uvc.0"
CONFIG="$GADGET/configs/c.1"

# ============================================
# 清理函数 - 正确清理 ConfigFS gadget
# ============================================
cleanup_gadget() {
    local gadget_path="$1"
    
    if [ ! -d "$gadget_path" ]; then
        return 0
    fi
    
    echo "  -> 正在清理旧的 Gadget 配置..."
    
    # 1. 先解绑 UDC
    if [ -f "$gadget_path/UDC" ]; then
        local current_udc=$(cat "$gadget_path/UDC" 2>/dev/null)
        if [ -n "$current_udc" ]; then
            echo "  -> 解绑 UDC: $current_udc"
            echo "" > "$gadget_path/UDC" 2>/dev/null || true
            sleep 1
        fi
    fi
    
    # 2. 删除配置中的 function 链接
    for config_dir in "$gadget_path"/configs/*/; do
        if [ -d "$config_dir" ]; then
            # 删除所有 function 符号链接
            for link in "$config_dir"*; do
                if [ -L "$link" ]; then
                    echo "  -> 删除链接: $(basename $link)"
                    rm -f "$link" 2>/dev/null || true
                fi
            done
            
            # 删除 strings 目录
            for str_dir in "$config_dir"strings/*/; do
                if [ -d "$str_dir" ]; then
                    rmdir "$str_dir" 2>/dev/null || true
                fi
            done
            if [ -d "${config_dir}strings" ]; then
                rmdir "${config_dir}strings" 2>/dev/null || true
            fi
            
            # 删除 config 目录
            rmdir "$config_dir" 2>/dev/null || true
        fi
    done
    
    # 3. 删除 functions
    for func_dir in "$gadget_path"/functions/*/; do
        if [ -d "$func_dir" ]; then
            local func_name=$(basename "$func_dir")
            echo "  -> 删除 function: $func_name"
            
            # 对于 UVC，需要先删除 streaming/header 中的链接
            if [[ "$func_name" == uvc.* ]]; then
                # 删除 streaming header 中的链接
                for link in "$func_dir"streaming/header/h/*; do
                    [ -L "$link" ] && rm -f "$link" 2>/dev/null
                done
                # 删除 streaming class 中的链接
                for link in "$func_dir"streaming/class/*/*; do
                    [ -L "$link" ] && rm -f "$link" 2>/dev/null
                done
                # 删除 control class 中的链接
                for link in "$func_dir"control/class/*/*; do
                    [ -L "$link" ] && rm -f "$link" 2>/dev/null
                done
                # 删除 control header 中的链接
                for link in "$func_dir"control/header/h/*; do
                    [ -L "$link" ] && rm -f "$link" 2>/dev/null
                done
                
                # 删除帧格式目录
                for frame_dir in "$func_dir"streaming/uncompressed/*/*/; do
                    [ -d "$frame_dir" ] && rmdir "$frame_dir" 2>/dev/null
                done
                for format_dir in "$func_dir"streaming/uncompressed/*/; do
                    [ -d "$format_dir" ] && rmdir "$format_dir" 2>/dev/null
                done
                
                # 删除 header 目录
                for h_dir in "$func_dir"streaming/header/*/; do
                    [ -d "$h_dir" ] && rmdir "$h_dir" 2>/dev/null
                done
                for h_dir in "$func_dir"control/header/*/; do
                    [ -d "$h_dir" ] && rmdir "$h_dir" 2>/dev/null
                done
            fi
            
            rmdir "$func_dir" 2>/dev/null || true
        fi
    done
    
    # 4. 删除 gadget strings
    for str_dir in "$gadget_path"/strings/*/; do
        if [ -d "$str_dir" ]; then
            rmdir "$str_dir" 2>/dev/null || true
        fi
    done
    
    # 5. 删除 gadget 目录
    rmdir "$gadget_path" 2>/dev/null || true
    
    if [ -d "$gadget_path" ]; then
        echo "  ⚠️  警告: 无法完全清理，建议重启开发板"
        return 1
    else
        echo "  ✅ 旧配置已清理"
        return 0
    fi
}

# ============================================
# 主程序开始
# ============================================

# 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    echo "错误: 请使用 root 权限运行此脚本"
    echo "用法: sudo $0"
    exit 1
fi

# 2. 环境检查
echo ""
echo "[1/7] 检查 ConfigFS..."
if [ ! -d "$CONFIGFS" ]; then
    echo "  -> 挂载 ConfigFS"
    mount -t configfs none $CONFIGFS
fi

if [ ! -d "$CONFIGFS/usb_gadget" ]; then
    echo "❌ 错误: ConfigFS usb_gadget 不可用"
    echo "请检查内核是否启用了 USB Gadget 支持"
    exit 1
fi
echo "  ✅ ConfigFS 可用"

# 3. 清理旧配置
if [ -d "$GADGET" ]; then
    cleanup_gadget "$GADGET"
    sleep 1
fi

# 4. 检查 UDC 是否可用
echo ""
echo "[2/7] 检查 USB 控制器..."
UDC_NAME=$(ls /sys/class/udc 2>/dev/null | head -n1)
if [ -z "$UDC_NAME" ]; then
    echo "❌ 错误: 未找到可用的 USB 控制器 (UDC)！"
    echo "   请检查:"
    echo "   1. Vivado 中 USB 是否配置为 Device 模式"
    echo "   2. 设备树中 dwc3 的 dr_mode 是否为 peripheral"
    echo "   3. 内核驱动是否正确加载"
    exit 1
fi
echo "  ✅ 找到 UDC: $UDC_NAME"

# 检查 UDC 是否已被占用
UDC_STATE=$(cat /sys/class/udc/$UDC_NAME/state 2>/dev/null)
echo "  状态: $UDC_STATE"

# 5. 创建 Gadget 结构
echo ""
echo "[3/7] 创建 Gadget 目录结构..."
mkdir -p $GADGET
cd $GADGET

# 6. 设置 USB 描述符
echo ""
echo "[4/7] 设置 USB 描述符..."
echo $VENDOR_ID > idVendor
echo $PRODUCT_ID > idProduct
echo 0x0200 > bcdUSB
echo 0xef > bDeviceClass
echo 0x02 > bDeviceSubClass
echo 0x01 > bDeviceProtocol

mkdir -p strings/0x409
echo "$MANUFACTURER" > strings/0x409/manufacturer
echo "$PRODUCT" > strings/0x409/product
echo "$SERIAL" > strings/0x409/serialnumber
echo "  ✅ 描述符设置完成"

# 7. 创建配置 (Configuration)
echo ""
echo "[5/7] 创建 USB 配置..."
mkdir -p configs/c.1/strings/0x409
echo "UVC Config" > configs/c.1/strings/0x409/configuration
echo 500 > configs/c.1/MaxPower
echo "  ✅ 配置创建完成"

# 8. 配置 UVC 功能 (Function)
echo ""
echo "[6/7] 配置 UVC 参数..."
mkdir -p $FUNCTION

# 8.1 控制接口 (Control Interface)
mkdir -p $FUNCTION/control/header/h
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/fs/h 2>/dev/null || true
ln -s $FUNCTION/control/header/h $FUNCTION/control/class/ss/h 2>/dev/null || true

# 8.2 流接口 (Streaming Interface)
# 设置格式: RGBA (Uncompressed 32-bit)
mkdir -p $FUNCTION/streaming/uncompressed/u/$FORMAT

# 设置 RGBA 格式 GUID (使用标准的 RGB4/ABGR 格式)
# Linux UVC 使用 'BA81EB33-49C3-4F3E-9B5D-BA1D5E004344' 表示 RGB32
echo "{ba81eb33-49c3-4f3e-9b5d-ba1d5e004344}" > $FUNCTION/streaming/uncompressed/u/guidFormat

# 创建帧格式目录(使用分辨率作为目录名)
mkdir -p $FUNCTION/streaming/uncompressed/u/480p

echo $WIDTH > $FUNCTION/streaming/uncompressed/u/$FORMAT/wWidth
echo $HEIGHT > $FUNCTION/streaming/uncompressed/u/$FORMAT/wHeight
echo 166666 > $FUNCTION/streaming/uncompressed/u/$FORMAT/dwDefaultFrameInterval
echo 32 > $FUNCTION/streaming/uncompressed/u/$FORMAT/bBitsPerPixel

# 计算缓冲区大小: W * H * 4 bytes (RGBA 32-bit)
FRAME_SIZE=$((WIDTH * HEIGHT * 4))
echo $FRAME_SIZE > $FUNCTION/streaming/uncompressed/u/$FORMAT/dwMaxVideoFrameBufferSize

# 比特率 (bps) = 帧大小 * 8 * fps
BIT_RATE=$((FRAME_SIZE * 8 * 60))
echo $((FRAME_SIZE * 8 * 30)) > $FUNCTION/streaming/uncompressed/u/$FORMAT/dwMinBitRate
echo $BIT_RATE > $FUNCTION/streaming/uncompressed/u/$FORMAT/dwMaxBitRate

# 支持的帧率 (以 100ns 为单位的帧间隔)
# 60fps = 166666, 30fps = 333333, 25fps = 400000, 20fps = 500000, 15fps = 666666
cat <<EOF > $FUNCTION/streaming/uncompressed/u/$FORMAT/dwFrameInterval
166666
333333
400000
500000
666666
EOF

# 链接流接口头部
mkdir -p $FUNCTION/streaming/header/h
ln -s $FUNCTION/streaming/uncompressed/u $FUNCTION/streaming/header/h/u 2>/dev/null || true
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/fs/h 2>/dev/null || true
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/hs/h 2>/dev/null || true
ln -s $FUNCTION/streaming/header/h $FUNCTION/streaming/class/ss/h 2>/dev/null || true

echo "  分辨率: ${WIDTH}x${HEIGHT}"
echo "  格式: RGBA (32-bit)"
echo "  帧大小: $FRAME_SIZE bytes"
echo "  ✅ UVC 参数配置完成"

# 9. 绑定功能到配置
echo ""
echo "[7/7] 激活并绑定..."

# 链接 function 到 config
ln -s $FUNCTION $CONFIG/uvc.0 2>/dev/null || {
    echo "⚠️  链接已存在，跳过"
}

# 10. 启用 Gadget (绑定 UDC)
echo "  绑定 UDC: $UDC_NAME"
echo $UDC_NAME > $GADGET/UDC 2>&1

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================="
    echo "✅ UVC Gadget 配置成功！"
    echo "========================================="
    echo ""
    echo "配置信息:"
    echo "  分辨率: ${WIDTH}x${HEIGHT}"
    echo "  格式: RGBA (32-bit)"
    echo "  UDC: $UDC_NAME"
    echo ""
    echo "下一步:"
    echo "  1. 将开发板 USB 连接到电脑"
    echo "  2. 运行 ./uvc_stream --test 测试"
    echo "  3. 在电脑上打开摄像头应用查看"
    echo ""
else
    echo ""
    echo "========================================="
    echo "❌ 绑定 UDC 失败！"
    echo "========================================="
    echo ""
    echo "可能的原因:"
    echo "  1. USB 控制器正被其他 gadget 使用"
    echo "  2. USB 线未正确连接"
    echo "  3. 驱动问题"
    echo ""
    echo "尝试以下步骤:"
    echo "  1. 重启开发板"
    echo "  2. 确保没有其他 gadget 程序运行"
    echo "  3. 检查 dmesg 日志: dmesg | tail -20"
    echo ""
    exit 1
fi
