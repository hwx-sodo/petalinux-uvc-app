# ZynqMP UVC Camera Gadget

这是一个用于 ZynqMP 开发板的 UVC (USB Video Class) Camera Gadget 配置项目。

## 项目结构

```
/workspace/
├── setup_rgba_fixed.sh     # RGBA 格式配置脚本 (原版)
├── setup_rgba_fixed_v2.sh  # RGBA 格式配置脚本 (修复版 v2)
├── setup_uvc.sh            # 通用 UVC 配置脚本
├── cleanup_gadget.sh       # 清理 USB Gadget 配置
├── debug_uvc.sh            # 调试诊断工具
├── run_uvc.sh              # 一键启动脚本
└── petalinux_app/          # 用户空间应用程序源码
```

## 快速开始

### 1. 配置 USB Gadget

```bash
# 使用修复版脚本 (推荐)
sudo /setup_rgba_fixed_v2.sh

# 或使用原版脚本
sudo /setup_uvc.sh
```

### 2. 运行视频流

```bash
sudo /run_uvc.sh
```

## 常见问题

### 错误: `failed to start g1: -19`

**完整错误信息:**
```
[   85.452619] configfs-gadget gadget: uvc: uvc_function_bind()
[   85.458347] configfs-gadget fe200000.dwc3: failed to start g1: -19
/setup_rgba_fixed.sh: line 103: echo: write error: No such device
```

**原因分析:**

错误码 `-19` 对应 `ENODEV` (No such device)。这表示 USB Device Controller (UDC) 无法启动 gadget。

**可能的原因:**

1. **USB 控制器模式不正确**
   - USB 控制器配置为 Host 模式而非 Peripheral/Device 模式
   - 需要在设备树中设置 `dr_mode = "peripheral"`

2. **USB PHY 未正确初始化**
   - USB PHY 电源未开启
   - PHY 时钟配置错误

3. **OTG 模式冲突**
   - 如果使用 OTG 模式，可能 ID 引脚检测到 Host 模式

4. **硬件连接问题**
   - USB 线未连接
   - USB Type-C 方向问题

**解决方法:**

1. **运行调试脚本检查系统状态:**
   ```bash
   sudo /debug_uvc.sh
   ```

2. **检查设备树 USB 配置:**
   ```bash
   # 查看 dr_mode 设置
   cat /proc/device-tree/axi/usb0@*/dwc3@*/dr_mode
   # 或
   cat /sys/firmware/devicetree/base/axi/usb*/dwc3*/dr_mode
   ```
   
   正确的设置应该是 `peripheral` 或 `otg`

3. **检查 UDC 状态:**
   ```bash
   ls /sys/class/udc/
   cat /sys/class/udc/*/state
   ```

4. **查看内核日志:**
   ```bash
   dmesg | grep -iE "(dwc3|usb|udc|gadget)"
   ```

5. **如果使用 OTG 模式，确保 ID 引脚接地:**
   - 在 OTG 模式下，ID 引脚低电平 = Device 模式
   - ID 引脚高电平/浮空 = Host 模式

### 设备树修改示例

如果需要修改设备树，确保 USB 节点配置如下：

```dts
&dwc3_0 {
    status = "okay";
    dr_mode = "peripheral";  /* 或 "otg" */
    maximum-speed = "super-speed";
    snps,dis_u2_susphy_quirk;
    snps,dis_u3_susphy_quirk;
};
```

### 内核配置要求

确保内核启用了以下选项：

```
CONFIG_USB_GADGET=y
CONFIG_USB_CONFIGFS=y
CONFIG_USB_CONFIGFS_F_UVC=y
CONFIG_USB_F_UVC=m  # 或 =y
CONFIG_USB_LIBCOMPOSITE=y
```

## 脚本说明

### setup_rgba_fixed_v2.sh (推荐)

修复版配置脚本，相比原版改进：
- 添加了 High Speed (hs) 链接支持
- 增加了详细的调试信息
- 更好的错误处理和提示

### debug_uvc.sh

诊断工具，可检查：
- ConfigFS 状态
- UDC 控制器状态
- 内核模块加载情况
- 设备树配置
- 内核日志

### cleanup_gadget.sh

用于清理失败的 gadget 配置：
```bash
sudo /cleanup_gadget.sh
```

## 技术参数

- **视频格式**: RGBA (32-bit)
- **分辨率**: 640x480
- **帧率**: 60 fps
- **帧大小**: 1,228,800 bytes

## 版本历史

- v2.0: 添加 hs 链接支持，改进错误诊断
- v1.0: 初始 RGBA 格式支持
