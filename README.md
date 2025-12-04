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

---

### 🔴 您的具体问题诊断

根据您的调试输出，发现以下**两个关键错误**：

```
[6.329295] dwc3-of-simple ff9d0000.usb0: dwc3_simple_set_phydata: Can't find usb3-phy
[6.338009] dwc3 fe200000.dwc3: Failed to get clk 'ref': -2
```

**这些错误说明：**

| 错误 | 含义 | 影响 |
|------|------|------|
| `Can't find usb3-phy` | USB3 PHY 设备未找到 | USB 控制器无法初始化 |
| `Failed to get clk 'ref': -2` | 缺少参考时钟 | USB 控制器无法工作 |

同时，UDC 状态显示 `not attached`，这意味着 USB 控制器根本没有正确初始化。

---

### 🛠️ 解决方案

**问题根源：设备树或硬件设计配置不完整**

#### 方案 1: 修改设备树 (system-user.dtsi)

在 Petalinux 项目中，编辑 `project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`：

```dts
/include/ "system-conf.dtsi"
/ {
};

/* USB 0 配置 - Peripheral/Device 模式 */
&usb0 {
    status = "okay";
};

&dwc3_0 {
    status = "okay";
    dr_mode = "peripheral";      /* 关键：设置为 device 模式 */
    maximum-speed = "super-speed";
    snps,dis_u2_susphy_quirk;
    snps,dis_u3_susphy_quirk;
    /delete-property/ phys;       /* 如果不使用 USB3，删除 PHY 引用 */
    /delete-property/ phy-names;
};
```

**如果只需要 USB2.0 (不需要 USB3.0)：**

```dts
&dwc3_0 {
    status = "okay";
    dr_mode = "peripheral";
    maximum-speed = "high-speed";  /* 限制为 USB2.0 */
    snps,dis_u2_susphy_quirk;
    /delete-property/ phys;
    /delete-property/ phy-names;
};
```

#### 方案 2: 检查 Vivado 硬件设计

确保在 Vivado Block Design 中：

1. **Zynq UltraScale+ MPSoC** 的 USB 配置:
   - USB0 启用
   - 选择正确的 PHY 接口 (ULPI 或 UTMI)
   - MIO 引脚分配正确

2. **如果使用 GT PHY (USB3.0)：**
   - 需要配置 PS-GTR 通道
   - 连接正确的 Lane

3. **如果只使用 USB2.0：**
   - 可以禁用 USB3 PHY，只使用 USB2 ULPI/UTMI

#### 方案 3: 运行深度诊断

```bash
sudo /diagnose_usb_issue.sh
```

这个脚本会检查：
- USB PHY 状态
- dr_mode 配置
- 时钟配置
- 设备树详情

---

### 📋 完整修复步骤

1. **修改设备树**
   ```bash
   cd <petalinux-project>
   vi project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
   ```

2. **重新构建**
   ```bash
   petalinux-build -c device-tree
   petalinux-build
   ```

3. **打包部署**
   ```bash
   petalinux-package --boot --fsbl --pmufw --u-boot --fpga
   # 将 BOOT.BIN 和 image.ub 复制到 SD 卡
   ```

4. **重启并测试**
   ```bash
   # 重启后运行
   /debug_uvc.sh
   # 确认 UDC 状态不再是 "not attached"
   /setup_rgba_fixed.sh
   ```

---

**可能的原因总结:**

1. **USB 控制器模式不正确**
   - USB 控制器配置为 Host 模式而非 Peripheral/Device 模式
   - 需要在设备树中设置 `dr_mode = "peripheral"`

2. **USB PHY 未正确初始化** ⬅️ **您的问题很可能在这里**
   - USB3 PHY 未在 Vivado 中正确配置
   - 设备树中 PHY 引用错误

3. **时钟配置错误** ⬅️ **您的问题也涉及这里**
   - USB 参考时钟未正确配置

4. **OTG 模式冲突**
   - 如果使用 OTG 模式，可能 ID 引脚检测到 Host 模式

5. **硬件连接问题**
   - USB 线未连接
   - USB Type-C 方向问题

**调试命令:**

1. **运行深度诊断脚本:**
   ```bash
   sudo /diagnose_usb_issue.sh
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
   dmesg | grep -iE "(dwc3|usb|udc|gadget|phy)"
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
