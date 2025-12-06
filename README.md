# ZynqMP 视频传输系统

这是一个用于 ZynqMP 开发板的视频传输项目，支持两种传输方式：
- **USB传输** - 通过UVC (USB Video Class) Gadget传输视频
- **网络传输** - 通过以太网 (UDP/TCP) 传输视频流

## 项目结构

```
/workspace/
├── setup_rgba_fixed.sh     # RGBA 格式配置脚本 (原版)
├── setup_rgba_fixed_v2.sh  # RGBA 格式配置脚本 (修复版 v2)
├── setup_uvc.sh            # 通用 UVC 配置脚本
├── cleanup_gadget.sh       # 清理 USB Gadget 配置
├── debug_uvc.sh            # 调试诊断工具
├── run_uvc.sh              # USB视频传输启动脚本
├── run_network_stream.sh   # 网络视频传输启动脚本
├── receive_stream.py       # PC端接收程序 (Python)
└── petalinux_app/          # 用户空间应用程序源码
    ├── main.c              # USB UVC应用主程序
    ├── network_stream.c    # 网络传输应用主程序
    ├── vpss_control.c/h    # VPSS控制模块
    ├── vdma_control.c/h    # VDMA控制模块
    └── Makefile            # 编译配置
```

## 快速开始

### 方式一：USB传输 (UVC)

#### 1. 配置 USB Gadget

```bash
# 使用修复版脚本 (推荐)
sudo /setup_rgba_fixed_v2.sh

# 或使用原版脚本
sudo /setup_uvc.sh
```

#### 2. 运行视频流

```bash
sudo /run_uvc.sh
```

---

### 方式二：网络传输 (推荐用于调试)

网络传输更加稳定，不需要特殊的USB配置，适合开发调试阶段。

> 📖 **详细教程请查看**: [NETWORK_STREAMING_GUIDE.md](NETWORK_STREAMING_GUIDE.md)

#### 软件依赖

| 端 | 需要安装 | 说明 |
|----|---------|------|
| **PC端** | ✅ OpenCV + NumPy | 用于显示和保存视频 |
| **开发板** | ❌ 不需要 | 纯C程序，无额外依赖 |

```bash
# 只在PC上安装
pip install opencv-python numpy
```

#### 快速开始（3步）

**第1步：PC端启动接收（⚠️ 必须先启动！）**
```bash
python receive_stream.py -p 5000
```

**第2步：开发板配置IP并启动发送**
```bash
# 配置开发板IP（首次需要）
ifconfig eth0 10.72.43.10 netmask 255.255.0.0 up

# 启动发送
sudo ./run_network_stream.sh 10.72.43.219
```

**第3步：查看视频**
- PC上会弹出视频窗口
- 按 'q' 键退出

#### 其他选项

```bash
# TCP模式（可靠传输，两端都加-t）
python receive_stream.py -p 5000 -t                    # PC端
sudo ./run_network_stream.sh 10.72.43.219 5000 tcp     # 开发板

# 保存视频到文件
python receive_stream.py -p 5000 -o output.avi
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

### 视频参数

| 参数 | 值 |
|------|-----|
| 视频格式 | RGBA (32-bit) |
| 分辨率 | 640x480 |
| 帧率 | 60 fps |
| 帧大小 | 1,228,800 bytes |
| 带宽需求 | ~70 MB/s (~560 Mbps) |

### 网络传输参数

| 参数 | UDP模式 | TCP模式 |
|------|---------|---------|
| 默认端口 | 5000 | 5000 |
| 分片大小 | 1400 bytes | 无分片 |
| 延迟 | 低 | 中等 |
| 可靠性 | 可能丢帧 | 可靠 |
| 推荐场景 | 实时预览 | 录制保存 |

### 网络带宽要求

- **理论带宽**: 640 × 480 × 4 × 60 = 73.7 MB/s ≈ 590 Mbps
- **推荐使用千兆以太网** (1 Gbps)
- 百兆网络可能出现丢帧

## 网络传输常见问题

### 1. 接收端收不到数据

**排查步骤:**
```bash
# 1. 检查网络连通性
ping <开发板IP>

# 2. 检查防火墙设置（Windows）
# 打开 Windows Defender 防火墙 → 高级设置 → 入站规则
# 添加允许端口5000的规则

# 3. 检查防火墙设置（Linux）
sudo ufw allow 5000/udp
sudo ufw allow 5000/tcp
```

### 2. 视频卡顿或延迟高

**可能原因:**
- 网络带宽不足
- CPU占用过高
- 使用了WiFi而非有线

**解决方法:**
- 使用有线千兆网络连接
- 关闭其他占用网络的程序
- 降低视频分辨率或帧率

### 3. UDP模式丢帧严重

**解决方法:**
```bash
# 方法1: 增加系统接收缓冲区
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.rmem_default=8388608

# 方法2: 切换到TCP模式
python receive_stream.py -p 5000 -t
```

### 4. 编译错误

```bash
# 确保安装了必要的开发工具
# 在PetaLinux SDK环境中编译
source /path/to/sdk/environment-setup-xxx

# 编译
cd petalinux_app
make network  # 仅编译网络应用
make all      # 编译所有应用
```

## 编译说明

### 在PetaLinux SDK中编译

```bash
# 设置SDK环境
source /path/to/sdk/environment-setup-aarch64-xilinx-linux

# 进入源码目录
cd petalinux_app

# 编译所有应用
make all

# 仅编译网络传输应用
make network

# 仅编译USB UVC应用
make uvc

# 清理
make clean
```

### 手动交叉编译

```bash
# 使用aarch64交叉编译器
aarch64-linux-gnu-gcc -O2 -Wall network_stream.c vpss_control.c vdma_control.c \
    -o network-stream-app
```

## 版本历史

- **v3.0** (2024-12): 
  - 新增网络传输功能 (UDP/TCP)
  - 添加PC端Python接收程序
  - 支持视频录制保存

- **v2.0**: 
  - 添加 hs 链接支持
  - 改进错误诊断

- **v1.0**: 
  - 初始 RGBA 格式支持
  - USB UVC基础功能
