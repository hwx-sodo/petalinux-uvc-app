#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网络视频流接收程序（PC端）

功能：
- 接收来自ZynqMP开发板的YUV422(YUYV)视频流
- 支持UDP和TCP两种协议
- 实时显示视频画面
- 可选保存为视频文件

依赖：
    pip install opencv-python numpy

使用方法：
    # UDP模式（默认）
    python receive_stream.py -p 5000
    
    # TCP模式
    python receive_stream.py -p 5000 -t
    
    # 保存视频
    python receive_stream.py -p 5000 -o output.avi
"""

import socket
import struct
import argparse
import time
import threading
import queue
import sys
from datetime import datetime

# 尝试导入OpenCV和NumPy
try:
    import cv2
    import numpy as np
    HAS_OPENCV = True
except ImportError:
    HAS_OPENCV = False
    print("警告: 未安装OpenCV，将只显示统计信息")
    print("安装方法: pip install opencv-python numpy")

# 帧头格式（与C程序一致）
FRAME_HEADER_FORMAT = '>IIIIIIII'  # 大端序，8个uint32
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FORMAT)
FRAME_MAGIC = 0x56494446  # "VIDF"

# UDP分片大小
UDP_CHUNK_SIZE = 1400

# 像素格式（与发送端 eth-camera-app 一致）
PIXFMT_YUYV = 1  # YUV422 packed (YUYV)


class FrameHeader:
    """帧头结构"""
    def __init__(self, data):
        fields = struct.unpack(FRAME_HEADER_FORMAT, data)
        self.magic = fields[0]
        self.frame_num = fields[1]
        self.width = fields[2]
        self.height = fields[3]
        self.format = fields[4]
        self.frame_size = fields[5]
        self.timestamp_sec = fields[6]
        self.timestamp_usec = fields[7]
    
    def is_valid(self):
        return self.magic == FRAME_MAGIC


class VideoReceiver:
    """视频流接收器"""
    
    def __init__(self, port, use_tcp=False, output_file=None, debug=False, color_mode='auto', yuv_order='yuyv'):
        self.port = port
        self.use_tcp = use_tcp
        self.output_file = output_file
        self.debug = debug
        # 解码模式:
        # - auto: 严格按帧头 header.format 判断（本项目只会发送YUV422/YUYV）
        # - yuyv/uyvy: 如果你怀疑相机字节序不一致，可强制切换
        self.color_mode = color_mode
        self.yuv_order = yuv_order
        
        self.running = False
        self.sock = None
        self.client_sock = None
        
        # 统计信息
        self.frame_count = 0
        self.start_time = None
        self.bytes_received = 0
        self.last_frame_num = -1
        self.dropped_frames = 0
        self.invalid_headers = 0  # 无效帧头计数
        self.partial_frames = 0   # 不完整帧计数
        
        # 帧队列（用于显示）
        self.frame_queue = queue.Queue(maxsize=5)
        
        # 视频写入器
        self.video_writer = None
        self._writer_initialized = False
        self._last_header = None
    
    def start(self):
        """启动接收器"""
        self.running = True
        
        if self.use_tcp:
            self._setup_tcp()
        else:
            self._setup_udp()
        
        # 启动接收线程
        recv_thread = threading.Thread(target=self._receive_loop)
        recv_thread.daemon = True
        recv_thread.start()
        
        # 显示循环
        self._display_loop()
    
    def _setup_udp(self):
        """设置UDP套接字"""
        print(f"创建UDP服务器，监听端口: {self.port}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
        self.sock.bind(('0.0.0.0', self.port))
        self.sock.settimeout(1.0)
        print(f"等待数据...")
    
    def _setup_tcp(self):
        """设置TCP套接字"""
        print(f"创建TCP服务器，监听端口: {self.port}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
        self.sock.bind(('0.0.0.0', self.port))
        self.sock.listen(1)
        self.sock.settimeout(1.0)
        print("等待连接...")
        
        while self.running:
            try:
                self.client_sock, addr = self.sock.accept()
                print(f"连接来自: {addr}")
                self.client_sock.settimeout(1.0)
                break
            except socket.timeout:
                continue
    
    def _receive_loop(self):
        """接收循环"""
        self.start_time = time.time()
        
        while self.running:
            try:
                if self.use_tcp:
                    header, frame = self._receive_frame_tcp()
                else:
                    header, frame = self._receive_frame_udp()
                
                if header is not None and frame is not None:
                    self._last_header = header
                    # 转换并放入队列
                    if HAS_OPENCV:
                        bgr = self._decode_frame_to_bgr(header, frame)

                        # 如果需要保存视频：首次拿到真实分辨率后再初始化writer
                        if self.output_file and (not self._writer_initialized):
                            fourcc = cv2.VideoWriter_fourcc(*'XVID')
                            self.video_writer = cv2.VideoWriter(
                                self.output_file, fourcc, 30, (header.width, header.height)
                            )
                            self._writer_initialized = True
                            print(f"视频将保存到: {self.output_file} (分辨率: {header.width}x{header.height})")
                        
                        try:
                            self.frame_queue.put_nowait(bgr)
                        except queue.Full:
                            pass  # 丢弃旧帧
                    
                    self.frame_count += 1
                    
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"接收错误: {e}")
                break
        
        print("接收线程退出")
    
    def _receive_frame_udp(self):
        """接收UDP帧 -> (header, frame_bytes)"""
        # 接收第一个数据包（应该是帧头）
        # UDP recvfrom会接收整个数据包，不管请求多少字节
        data, addr = self.sock.recvfrom(65535)
        self.bytes_received += len(data)
        
        if len(data) < FRAME_HEADER_SIZE:
            if self.debug:
                print(f"[DEBUG] 收到数据包太小: {len(data)} bytes")
            return None, None
        
        # 尝试解析帧头
        header = FrameHeader(data[:FRAME_HEADER_SIZE])
        if not header.is_valid():
            self.invalid_headers += 1
            if self.debug and self.invalid_headers <= 10:
                # 只打印前10次无效帧头
                print(f"[DEBUG] 无效帧头 #{self.invalid_headers}, magic=0x{header.magic:08X} (期望0x{FRAME_MAGIC:08X})")
                print(f"[DEBUG] 数据包前32字节: {data[:32].hex()}")
            return None, None
        
        if self.debug and self.frame_count == 0:
            print(f"[DEBUG] 收到第一个有效帧头: 帧号={header.frame_num}, 大小={header.frame_size}")
        
        # 检测丢帧
        if self.last_frame_num >= 0:
            expected = self.last_frame_num + 1
            if header.frame_num != expected:
                dropped = header.frame_num - expected
                if dropped > 0:
                    self.dropped_frames += dropped
        self.last_frame_num = header.frame_num
        
        # 如果帧头和部分数据在同一个包中
        frame_data = bytearray()
        if len(data) > FRAME_HEADER_SIZE:
            frame_data.extend(data[FRAME_HEADER_SIZE:])
        
        # 接收剩余帧数据
        while len(frame_data) < header.frame_size:
            try:
                chunk, _ = self.sock.recvfrom(65535)
                frame_data.extend(chunk)
                self.bytes_received += len(chunk)
            except socket.timeout:
                self.partial_frames += 1
                if self.debug:
                    print(f"[DEBUG] 帧 #{header.frame_num} 不完整: 收到 {len(frame_data)}/{header.frame_size} bytes")
                return None, None  # 帧不完整
        
        return header, bytes(frame_data[:header.frame_size])
    
    def _receive_frame_tcp(self):
        """接收TCP帧 -> (header, frame_bytes)"""
        if not self.client_sock:
            return None, None
        
        # 接收帧头
        data = self._recv_exact(self.client_sock, FRAME_HEADER_SIZE)
        if not data:
            return None, None
        
        header = FrameHeader(data)
        if not header.is_valid():
            return None, None
        
        # 检测丢帧
        if self.last_frame_num >= 0:
            expected = self.last_frame_num + 1
            if header.frame_num != expected:
                self.dropped_frames += header.frame_num - expected
        self.last_frame_num = header.frame_num
        
        # 接收帧数据
        frame_data = self._recv_exact(self.client_sock, header.frame_size)
        if frame_data:
            self.bytes_received += len(frame_data)
        return header, frame_data

    def _decode_frame_to_bgr(self, header, frame_bytes):
        """
        将一帧原始数据解码为OpenCV可显示的BGR图像。
        - 优先使用header.format自动判断
        - color_mode可强制覆盖（例如强制uyvy）
        """
        w, h = int(header.width), int(header.height)
        forced = (self.color_mode or 'auto').lower()

        # ---------- YUV422 (YUYV/UYVY) ----------
        # auto：严格按帧头决定；只有用户显式指定yuyv/uyvy才强制覆盖
        is_yuv_forced = forced in ('yuyv', 'uyvy', 'yuv', 'yuv422')
        is_yuv = (header.format == PIXFMT_YUYV) if forced == 'auto' else is_yuv_forced
        if not is_yuv:
            # 本项目只支持YUV422：如果帧头不是YUV422，就直接拒绝（避免误解码）
            raise ValueError(f"不支持的像素格式: header.format={header.format} (仅支持YUYV/YUV422)")

        if is_yuv:
            expected = w * h * 2
            if len(frame_bytes) < expected:
                raise ValueError(f"帧数据不足(YUV422): got={len(frame_bytes)}, expected>={expected}")
            yuv = np.frombuffer(frame_bytes[:expected], dtype=np.uint8).reshape((h, w, 2))

            order = self.yuv_order.lower()
            if forced == 'uyvy':
                order = 'uyvy'
            if forced == 'yuyv':
                order = 'yuyv'

            if order == 'uyvy':
                return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_UYVY)
            return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_YUY2)
    
    def _recv_exact(self, sock, size):
        """精确接收指定字节数"""
        data = bytearray()
        while len(data) < size:
            try:
                chunk = sock.recv(size - len(data))
                if not chunk:
                    return None
                data.extend(chunk)
            except socket.timeout:
                return None
        return bytes(data)
    
    def _display_loop(self):
        """显示循环"""
        print("\n开始接收视频流...")
        print("按 'q' 键退出\n")
        
        last_stats_time = time.time()
        
        try:
            while self.running:
                # 更新统计信息
                now = time.time()
                if now - last_stats_time >= 1.0:
                    self._print_stats()
                    last_stats_time = now
                
                if HAS_OPENCV:
                    try:
                        frame = self.frame_queue.get(timeout=0.1)
                        
                        # 显示帧
                        cv2.imshow('Video Stream', frame)
                        
                        # 保存视频
                        if self.video_writer:
                            self.video_writer.write(frame)
                        
                        # 检测按键
                        key = cv2.waitKey(1) & 0xFF
                        if key == ord('q'):
                            break
                        
                    except queue.Empty:
                        # 无新帧，检测按键
                        key = cv2.waitKey(1) & 0xFF
                        if key == ord('q'):
                            break
                else:
                    # 无OpenCV，只等待
                    time.sleep(0.1)
                    
        except KeyboardInterrupt:
            print("\n中断")
        
        self.running = False
        self._cleanup()
    
    def _print_stats(self):
        """打印统计信息"""
        if self.start_time is None:
            return
        
        elapsed = time.time() - self.start_time
        fps = self.frame_count / elapsed if elapsed > 0 else 0
        bitrate = self.bytes_received * 8 / elapsed / 1e6 if elapsed > 0 else 0
        
        stats = f"帧: {self.frame_count:6d} | FPS: {fps:5.1f} | 码率: {bitrate:6.1f} Mbps"
        if self.dropped_frames > 0:
            stats += f" | 丢帧: {self.dropped_frames}"
        if self.invalid_headers > 0:
            stats += f" | 无效头: {self.invalid_headers}"
        if self.partial_frames > 0:
            stats += f" | 不完整: {self.partial_frames}"
        print(stats)
    
    def _cleanup(self):
        """清理资源"""
        print("\n清理资源...")
        
        if self.video_writer:
            self.video_writer.release()
        
        if self.client_sock:
            self.client_sock.close()
        
        if self.sock:
            self.sock.close()
        
        if HAS_OPENCV:
            cv2.destroyAllWindows()
        
        # 最终统计
        if self.start_time:
            elapsed = time.time() - self.start_time
            print(f"\n统计:")
            print(f"  总帧数: {self.frame_count}")
            print(f"  运行时间: {elapsed:.1f} 秒")
            print(f"  平均FPS: {self.frame_count/elapsed:.1f}" if elapsed > 0 else "  平均FPS: 0")
            print(f"  接收数据: {self.bytes_received/1024/1024:.1f} MB")
            print(f"  丢帧: {self.dropped_frames}")
            print(f"  无效帧头: {self.invalid_headers}")
            print(f"  不完整帧: {self.partial_frames}")
            
            if self.frame_count == 0 and self.bytes_received > 0:
                print("\n[诊断] 收到数据但无有效帧，可能原因:")
                print("  1. 发送端与接收端协议不匹配")
                print("  2. 发送端没有正确发送帧头")
                print("  3. 使用 -d 参数启动可查看详细调试信息")


def main():
    parser = argparse.ArgumentParser(
        description='网络视频流接收程序',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
  %(prog)s -p 5000           # UDP模式监听5000端口
  %(prog)s -p 5000 -t        # TCP模式
  %(prog)s -p 5000 -o out.avi  # 保存视频
  %(prog)s -p 5000 -d        # 调试模式
        ''')
    
    parser.add_argument('-p', '--port', type=int, default=5000,
                        help='监听端口 (默认: 5000)')
    parser.add_argument('-t', '--tcp', action='store_true',
                        help='使用TCP协议 (默认: UDP)')
    parser.add_argument('-o', '--output', type=str, default=None,
                        help='输出视频文件 (如: output.avi)')
    parser.add_argument('-d', '--debug', action='store_true',
                        help='调试模式，打印详细信息')
    parser.add_argument('-c', '--color', type=str, default='auto',
                        choices=['auto', 'yuyv', 'uyvy'],
                        help='解码模式: auto(默认,按帧头自动), yuyv, uyvy')
    parser.add_argument('--yuv-order', type=str, default='uyvy',
                        choices=['yuyv', 'uyvy'],
                        help='当输入是YUV422时的字节序 (默认: uyvy)')
    
    args = parser.parse_args()
    
    print("=" * 50)
    print("网络视频流接收程序")
    print("=" * 50)
    print(f"协议: {'TCP' if args.tcp else 'UDP'}")
    print(f"端口: {args.port}")
    print(f"输出: {args.output if args.output else '无'}")
    print(f"调试: {'开启' if args.debug else '关闭'}")
    print(f"颜色: {args.color}")
    print(f"YUV顺序: {args.yuv_order}")
    print(f"OpenCV: {'已安装' if HAS_OPENCV else '未安装'}")
    print("=" * 50 + "\n")
    
    receiver = VideoReceiver(args.port, args.tcp, args.output, args.debug, args.color, args.yuv_order)
    
    receiver.start()
    print("程序退出")


if __name__ == '__main__':
    main()
