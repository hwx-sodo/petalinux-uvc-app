# ZynqMP 以太网视频传输系统

这是一个用于 ZynqMP 开发板的视频传输项目，通过以太网 (UDP/TCP) 传输视频流到PC端。

## 项目结构

```
/workspace/
├── run_network_stream.sh   # 网络视频传输启动脚本
├── receive_stream.py       # PC端接收程序 (Python)
└── petalinux_app/          # 用户空间应用程序源码
    ├── network_stream.c    # 网络传输应用主程序
    ├── vpss_control.c/h    # VPSS控制模块
    ├── vdma_control.c/h    # VDMA控制模块
    └── Makefile            # 编译配置
```

## 快速开始

### 软件依赖

| 端 | 需要安装 | 说明 |
|----|---------|------|
| **PC端** | ✅ OpenCV + NumPy | 用于显示和保存视频 |
| **开发板** | ❌ 不需要 | 纯C程序，无额外依赖 |

```bash
# 只在PC上安装
pip install opencv-python numpy
```

### 使用步骤（3步）

**第1步：PC端启动接收（⚠️ 必须先启动！）**
```bash
python receive_stream.py -p 5000
```

**第2步：开发板配置IP并启动发送**
```bash
# 配置开发板IP（首次需要）
ifconfig eth0 10.72.43.10 netmask 255.255.0.0 up

# 启动发送
sudo ./run_network_stream.sh 10.72.43.200
```

**第3步：查看视频**
- PC上会弹出视频窗口
- 按 'q' 键退出

### 其他选项

```bash
# TCP模式（可靠传输，两端都加-t）
python receive_stream.py -p 5000 -t                    # PC端
sudo ./run_network_stream.sh 10.72.43.200 5000 tcp     # 开发板

# 保存视频到文件
python receive_stream.py -p 5000 -o output.avi

# 调试模式（查看详细信息）
python receive_stream.py -p 5000 -d                    # PC端
sudo ./run_network_stream.sh 10.72.43.200 5000 udp debug  # 开发板

# 如果画面颜色不对/花屏：尝试强制YUV422打包格式
python receive_stream.py -p 5000 -d --force-format uyvy     # PC端强制按UYVY解析
sudo ./run_network_stream.sh 10.72.43.200 5000 udp debug uyvy  # 开发板强制按UYVY发送（帧头也会标记）

# 强制发送模式（忽略帧变化检测，用于测试网络）
sudo ./run_network_stream.sh 10.72.43.200 5000 udp force  # 开发板

# 调试+强制模式
sudo ./run_network_stream.sh 10.72.43.200 5000 udp debug force
```

## 编译说明

### 在PetaLinux SDK中编译

```bash
# 设置SDK环境
source /path/to/sdk/environment-setup-aarch64-xilinx-linux

# 进入源码目录
cd petalinux_app

# 编译
make

# 清理
make clean
```

### 手动交叉编译

```bash
# 使用aarch64交叉编译器
make CC=aarch64-linux-gnu-gcc
```

### Makefile 使用

```bash
make           # 编译eth-camera-app
make all       # 编译eth-camera-app
make clean     # 清理编译产物
make install   # 安装到目标系统
make help      # 显示帮助信息
```

## 技术参数

### 视频参数

| 参数 | 值 |
|------|-----|
| 视频格式 | YUV422（YUYV 或 UYVY，16-bit/像素） |
| 分辨率 | 640x480 |
| 帧率 | 60 fps |
| 帧大小 | 614,400 bytes |
| 带宽需求 | ~35 MB/s (~295 Mbps) |

### 硬件地址

| 参数 | 值 |
|------|-----|
| 帧缓冲区物理地址 | 0x20000000 - 0x40000000 |
| 帧缓冲区大小 | 0x20000000 (512 MB) |
| VPSS基地址 | 0x80000000 |
| VDMA基地址 | 0x80020000 |

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

## 常见问题

### 1. 接收端收到数据但帧数为0

**症状:**
- 接收端显示收到了数据（如1.1MB），但帧数始终为0
- 开发板端没有显示"已发送 X 帧"的统计

**原因分析:**
这通常是因为VDMA的帧号没有变化。代码中有帧变化检测逻辑，如果VDMA帧号不变，会跳过发送。

**解决方法:**
```bash
# 方法1: 使用调试模式查看详细信息
./run_network_stream.sh 10.72.43.200 5000 udp debug

# 方法2: 使用强制发送模式（忽略帧变化检测）
./run_network_stream.sh 10.72.43.200 5000 udp force

# 方法3: 同时使用调试和强制模式
./run_network_stream.sh 10.72.43.200 5000 udp debug force

# PC端使用调试模式
python receive_stream.py -p 5000 -d
```

**调试输出说明:**
- `[DEBUG] 帧号未变化，已跳过 X 次` - VDMA没有产生新帧
- `[DEBUG] 无效帧头` - 收到的数据不是有效帧格式
- `[DEBUG] 帧缓冲前16字节` - 显示实际帧数据内容

### 2. 接收端收不到数据

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

### 3. VPSS或VDMA错误

**症状:**
```
警告: VPSS错误寄存器: 0x00000003
VDMA状态: 0x00010000
```

**可能原因:**
- 视频输入源未连接或未启动
- 时钟配置问题
- 设备树配置错误

**排查步骤:**
```bash
# 检查UIO设备
ls -la /dev/uio*

# 检查VPSS和VDMA物理地址
cat /sys/class/uio/uio*/maps/map0/addr
```

### 4. 视频卡顿或延迟高

**可能原因:**
- 网络带宽不足
- CPU占用过高
- 使用了WiFi而非有线

**解决方法:**
- 使用有线千兆网络连接
- 关闭其他占用网络的程序
- 降低视频分辨率或帧率

### 5. UDP模式丢帧严重

**解决方法:**
```bash
# 方法1: 增加系统接收缓冲区
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.rmem_default=8388608

# 方法2: 切换到TCP模式
python receive_stream.py -p 5000 -t
```

### 6. 编译错误

```bash
# 确保安装了必要的开发工具
# 在PetaLinux SDK环境中编译
source /path/to/sdk/environment-setup-xxx

# 编译
cd petalinux_app
make
```

### 7. 保存文件时系统崩溃 (mmc1: Timeout)

**症状:**
在运行 `eth-camera-app -D -s frame.bin` 时，系统卡死或打印 `mmc1: Timeout waiting for hardware interrupt` 错误。

**原因:**
VDMA 高速写入 DDR 占用了大量总线带宽，导致 SD 卡控制器 (MMC) 在尝试写入文件时无法及时获得总线控制权或中断响应，从而超时。

**解决方法:**
- 最新版本已修复此问题：在保存文件前会自动停止 VDMA 传输，并使用分块写入+同步策略来减轻 MMC 压力。
- 请重新编译程序并部署。

### 8. 程序退出时卡住

**症状:**
运行 `eth-camera-app -D` 或正常退出时，程序卡在"清理资源..."或"停止VDMA..."阶段。

**原因:**
旧版本的 `vdma_stop()` 函数只是简单地写 0 到控制寄存器，没有等待 VDMA 真正停止（HALTED 位变为 1）。如果 VDMA 还在进行 DMA 传输，后续的 `munmap` 帧缓冲区操作会导致系统挂起。

**解决方法:**
- 最新版本已修复此问题：`vdma_stop()` 会等待 HALTED 位变为 1，确保 VDMA 完全停止后再继续清理。
- 如果仍然超时，会自动尝试软复位 VDMA。
- 请重新编译程序并部署。

## 版本历史

- **v4.1** (2024-12):
  - 修复程序退出时卡住的问题（vdma_stop 等待 HALTED 位）
  - 增加 VDMA 停止超时后的软复位机制

- **v4.0** (2024-12): 
  - 移除USB UVC功能，专注以太网传输
  - 简化项目结构
  - 目标程序重命名为 eth-camera-app

- **v3.0**: 
  - 新增网络传输功能 (UDP/TCP)
  - 添加PC端Python接收程序
  - 支持视频录制保存

- **v2.0**: 
  - 添加 hs 链接支持
  - 改进错误诊断

- **v1.0**:
  - 初始版本（历史上使用过RGBA，当前已切换为YUV422）
  - USB UVC基础功能
