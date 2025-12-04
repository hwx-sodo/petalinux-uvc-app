#!/bin/bash
# cleanup_gadget.sh - 清理 USB Gadget 配置
# 
# 用于在配置失败后清理 ConfigFS 中的 gadget
#

echo "========================================="
echo "       USB Gadget 清理工具"
echo "========================================="

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget/g1"

if [ ! -d "$GADGET" ]; then
    echo "没有找到 gadget 配置，无需清理"
    exit 0
fi

echo ""
echo "当前 Gadget 状态:"
echo "  路径: $GADGET"

# 显示当前 UDC 绑定
if [ -f "$GADGET/UDC" ]; then
    UDC=$(cat "$GADGET/UDC" 2>/dev/null)
    if [ -n "$UDC" ]; then
        echo "  UDC: $UDC (已绑定)"
    else
        echo "  UDC: (未绑定)"
    fi
fi

echo ""
echo "开始清理..."

# 1. 解绑 UDC
echo "[1/5] 解绑 UDC..."
if [ -f "$GADGET/UDC" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null
    sleep 1
fi

# 2. 删除配置中的链接
echo "[2/5] 删除配置链接..."
for link in "$GADGET"/configs/c.1/*; do
    if [ -L "$link" ]; then
        echo "  删除: $(basename $link)"
        rm -f "$link"
    fi
done

# 3. 删除 UVC streaming 中的链接
echo "[3/5] 删除 UVC streaming 链接..."
FUNC="$GADGET/functions/uvc.0"
if [ -d "$FUNC" ]; then
    # streaming header 链接
    for link in "$FUNC"/streaming/header/h/*; do
        [ -L "$link" ] && rm -f "$link"
    done
    
    # streaming class 链接
    for dir in fs hs ss; do
        for link in "$FUNC"/streaming/class/$dir/*; do
            [ -L "$link" ] && rm -f "$link"
        done
    done
    
    # control 链接
    for link in "$FUNC"/control/header/h/*; do
        [ -L "$link" ] && rm -f "$link"
    done
    for dir in fs ss; do
        for link in "$FUNC"/control/class/$dir/*; do
            [ -L "$link" ] && rm -f "$link"
        done
    done
fi

# 4. 删除帧格式目录
echo "[4/5] 删除帧格式目录..."
if [ -d "$FUNC/streaming/uncompressed" ]; then
    # 删除帧目录 (如 yuy2)
    for frame in "$FUNC"/streaming/uncompressed/*/*/; do
        [ -d "$frame" ] && rmdir "$frame" 2>/dev/null
    done
    # 删除格式目录 (如 u)
    for format in "$FUNC"/streaming/uncompressed/*/; do
        [ -d "$format" ] && rmdir "$format" 2>/dev/null
    done
fi

# 删除 header 目录
for h in "$FUNC"/streaming/header/*/; do
    [ -d "$h" ] && rmdir "$h" 2>/dev/null
done
for h in "$FUNC"/control/header/*/; do
    [ -d "$h" ] && rmdir "$h" 2>/dev/null
done

# 删除 function
[ -d "$FUNC" ] && rmdir "$FUNC" 2>/dev/null

# 5. 删除 configs 和 strings
echo "[5/5] 删除配置目录..."
# configs strings
for str in "$GADGET"/configs/c.1/strings/*/; do
    [ -d "$str" ] && rmdir "$str" 2>/dev/null
done
[ -d "$GADGET/configs/c.1/strings" ] && rmdir "$GADGET/configs/c.1/strings" 2>/dev/null
[ -d "$GADGET/configs/c.1" ] && rmdir "$GADGET/configs/c.1" 2>/dev/null

# gadget strings
for str in "$GADGET"/strings/*/; do
    [ -d "$str" ] && rmdir "$str" 2>/dev/null
done

# gadget 目录
rmdir "$GADGET" 2>/dev/null

# 检查结果
echo ""
if [ -d "$GADGET" ]; then
    echo "⚠️  警告: 无法完全清理 gadget"
    echo ""
    echo "剩余内容:"
    ls -la "$GADGET" 2>/dev/null
    echo ""
    echo "建议: 重启开发板以完全清理"
else
    echo "✅ Gadget 配置已完全清理"
fi
