# ZynqMP CameraLink 以太网视频传输系统

## 概述

这是一个用于 Xilinx Zynq UltraScale+ MPSoC 开发板的视频传输项目，将 CameraLink 相机数据通过以太网传输到 PC 端显示。

## 数据流架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           FPGA PL 端                                     │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  CameraLink Camera                                                       │
│       │                                                                  │
│       ▼                                                                  │
│  portA[7:0] + portB[7:0]  ─────────────► 16-bit YUV422                   │
│       │                                                                  │
│       ▼                                                                  │
│  Video In to AXI4-Stream  ─────────────► 16-bit AXI4-Stream              │
│       │                                                                  │
│       ▼                                                                  │
│  AXI4-Stream Data Width Converter ─────► 32-bit AXI4-Stream              │
│       │                                   (2像素打包)                     │
│       ▼                                                                  │
│  AXI VDMA (S2MM)  ─────────────────────► DDR (32-bit总线写入)            │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           DDR 存储                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  帧缓冲区 @ 0x20000000                                                    │
│  格式: YUV422 (YUYV) - 每像素2字节                                        │
│  布局: [Y0][U][Y1][V] [Y2][U][Y3][V] ...                                │
│                                                                          │
│  帧0: 0x20000000 ~ 0x20095FFF (614,400 bytes)                            │
│  (单帧缓冲模式)                                                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           PS 端 (Linux)                                  │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  eth-camera-app 程序:                                                    │
│    1. 通过 UIO 控制 VDMA                                                  │
│    2. 通过 /dev/mem 访问帧缓冲                                            │
│    3. 通过 UDP/TCP 发送到 PC                                              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                              以太网传输
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           PC 端                                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  receive_stream.py:                                                      │
│    1. 接收 UDP/TCP 数据                                                   │
│    2. 解析帧头                                                           │
│    3. YUV422 → BGR 转换                                                   │
│    4. OpenCV 显示                                                         │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## 项目结构

```
/workspace/
├── README.md                    # 本文档
├── receive_stream.py            # PC端接收程序 (Python/OpenCV)
├── run_network_stream.sh        # 开发板启动脚本
│
├── petalinux_app/               # 开发板应用程序源码
│   ├── Makefile                 # 编译配置
│   ├── network_stream.c         # 主程序 (网络传输)
│   ├── vdma_control.c           # VDMA控制实现
│   └── vdma_control.h           # VDMA控制接口
│
├── petalinux_config/            # PetaLinux配置
│   ├── README.md
│   └── system-user.dtsi         # 设备树配置
│
└── docs/                        # 文档
    └── petalinux_network_config.md
```

## 快速开始

### 1. 安装PC端依赖

```bash
pip install opencv-python numpy
```

### 2. 启动PC端接收程序（必须先启动）

```bash
python receive_stream.py -p 5000
```

### 3. 开发板配置和启动

```bash
# 配置网络 (首次需要)
ifconfig eth0 10.72.43.10 netmask 255.255.0.0 up

# 启动发送
./eth-camera-app -H 10.72.43.200 -p 5000
```

### 4. 查看视频

- PC上会弹出视频窗口
- 按 `q` 键退出

## 详细使用说明

### 开发板端 (eth-camera-app)

```bash
# UDP模式（默认，低延迟）
./eth-camera-app -H <PC_IP> [-p 端口]

# TCP模式（可靠传输）
./eth-camera-app -H <PC_IP> -p 5000 -t

# 调试模式（打印详细信息）
./eth-camera-app -H <PC_IP> -d

# 强制发送模式（忽略帧变化检测）
./eth-camera-app -H <PC_IP> -f

# 调试 + 强制发送
./eth-camera-app -H <PC_IP> -d -f

# 仅诊断模式（不发送网络数据）
./eth-camera-app -D

# 诊断并保存帧数据
./eth-camera-app -D -s frame.bin
```

### PC端 (receive_stream.py)

```bash
# UDP模式（默认）
python receive_stream.py -p 5000

# TCP模式
python receive_stream.py -p 5000 -t

# 保存视频
python receive_stream.py -p 5000 -o output.avi

# 调试模式
python receive_stream.py -p 5000 -d

# 强制YUYV格式（如果颜色不对）
python receive_stream.py -p 5000 --force-format yuyv
```

## 编译说明

### 在PetaLinux SDK中编译

```bash
# 设置SDK环境
source /path/to/sdk/environment-setup-aarch64-xilinx-linux

# 编译
cd petalinux_app
make

# 清理
make clean
```

### 手动交叉编译

```bash
make CC=aarch64-linux-gnu-gcc
```

## 技术参数

### 视频参数

| 参数 | 值 |
|------|-----|
| 分辨率 | 640 × 480 |
| 帧率 | 30 fps |
| 像素格式 | YUV422 (YUYV) |
| 每像素字节数 | 2 |
| 帧大小 | 614,400 bytes |
| 带宽需求 | ~17.5 MB/s (~148 Mbps) |

### 硬件地址

| 参数 | 值 |
|------|-----|
| VDMA基地址 | 0x80020000 |
| 帧缓冲物理地址 | 0x20000000 |
| 帧缓冲大小 | 512 MB (0x20000000) |

### 网络传输

| 参数 | UDP模式 | TCP模式 |
|------|---------|---------|
| 默认端口 | 5000 | 5000 |
| 分片大小 | 1400 bytes | 无分片 |
| 延迟 | 低 | 中等 |
| 可靠性 | 可能丢帧 | 可靠 |
| 推荐场景 | 实时预览 | 录制保存 |

## VDMA 配置说明

### S2MM 通道配置

```
控制寄存器 (0x30):
  - RS = 1 (运行)
  - Circular = 1 (循环缓冲模式)

帧参数:
  - VSize (0xA0) = 480 (行数)
  - HSize (0xA4) = 1280 (每行字节数 = 640 × 2)
  - Stride (0xA8) = 1280 (行跨度)

帧缓冲地址 (单帧模式):
  - Addr0 (0xAC) = 0x20000000
```

### 数据宽度转换

```
输入: 16-bit AXI4-Stream (每个传输 = 1像素)
输出: 32-bit AXI4-Stream (每个传输 = 2像素)

打包方式:
  32-bit = [像素1(16-bit)] [像素0(16-bit)]
         = [Y1][V] [Y0][U]
  
存储顺序 (小端):
  地址 +0: Y0
  地址 +1: U
  地址 +2: Y1  
  地址 +3: V
```

## 常见问题

### 1. VDMA 处于 HALTED 状态

**可能原因：**
- 视频输入源未连接
- CameraLink 时序不匹配
- Video In to AXI4-Stream 配置错误

**排查步骤：**
```bash
# 运行诊断模式
./eth-camera-app -D

# 检查VDMA状态寄存器中的错误位
# SOF/EOL Early/Late 错误表示时序问题
```

### 2. 接收端收到数据但帧数为0

**可能原因：**
- VDMA帧号不变化
- 帧头格式不匹配

**解决方法：**
```bash
# 开发板使用强制发送
./eth-camera-app -H <PC_IP> -f

# PC端使用调试模式
python receive_stream.py -p 5000 -d
```

### 3. 画面颜色不正确

**可能原因：**
- YUV422打包格式不匹配 (YUYV vs UYVY)

**解决方法：**
```bash
# 尝试不同的格式
python receive_stream.py -p 5000 --force-format uyvy
```

### 4. 画面撕裂或错位

**可能原因：**
- Stride 配置不正确
- 帧缓冲对齐问题

**解决方法：**
确保 Stride = HSize (无填充情况下)

### 5. 网络连接失败

**排查步骤：**
```bash
# 检查网络连通性
ping <PC_IP>

# 检查防火墙 (Linux)
sudo ufw allow 5000/udp
sudo ufw allow 5000/tcp

# 检查防火墙 (Windows)
# 打开 Windows Defender 防火墙 → 高级设置 → 添加入站规则
```

## 版本历史

- **v5.0** (2024-12): 
  - 重写VDMA控制逻辑
  - 简化数据流架构（移除VPSS）
  - 改进代码结构和注释

- **v4.0** (2024-12): 
  - 移除USB UVC功能
  - 专注以太网传输

- **v3.0**: 
  - 新增网络传输功能 (UDP/TCP)
  - 添加PC端Python接收程序

## 许可证

MIT License
