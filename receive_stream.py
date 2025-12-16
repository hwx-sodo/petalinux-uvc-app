#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网络视频流接收程序 (PC端)

功能：
  - 接收来自ZynqMP开发板的YUV422视频流
  - 支持UDP和TCP协议
  - 实时显示视频画面 (OpenCV)
  - 可选保存为视频文件

数据流架构：
  CameraLink(16-bit) → Video In → Width Converter(32-bit) →
  VDMA S2MM → DDR(YUV422/YUYV) → 网络 → 本程序

依赖：
    pip install opencv-python numpy

使用方法：
    python receive_stream.py -p 5000           # UDP模式
    python receive_stream.py -p 5000 -t        # TCP模式
    python receive_stream.py -p 5000 -d        # 调试模式
    python receive_stream.py -p 5000 -o out.avi  # 保存视频
"""

import socket
import struct
import argparse
import time
import threading
import queue
import sys
from datetime import datetime

# 尝试导入OpenCV
try:
    import cv2
    import numpy as np
    HAS_OPENCV = True
except ImportError:
    HAS_OPENCV = False
    print("警告: 未安装OpenCV，将只显示统计信息")
    print("安装方法: pip install opencv-python numpy")


# ============================================================================
# 帧协议定义 (与C程序一致)
# ============================================================================

FRAME_HEADER_FORMAT = '>IIIIIIII'  # 大端序, 8个uint32
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FORMAT)
FRAME_MAGIC = 0x56494446  # "VIDF"

# 像素格式
FMT_YUYV = 1
FMT_UYVY = 2


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
    
    def __str__(self):
        fmt_str = "YUYV" if self.format == FMT_YUYV else "UYVY"
        return (f"Frame#{self.frame_num} {self.width}x{self.height} "
                f"{fmt_str} {self.frame_size}B")


# ============================================================================
# 视频接收器
# ============================================================================

class VideoReceiver:
    """视频流接收器"""
    
    def __init__(self, port, use_tcp=False, output_file=None, 
                 debug=False, force_format='auto'):
        self.port = port
        self.use_tcp = use_tcp
        self.output_file = output_file
        self.debug = debug
        self.force_format = force_format
        
        self.running = False
        self.sock = None
        self.client_sock = None
        
        # 统计信息
        self.frame_count = 0
        self.start_time = None
        self.bytes_received = 0
        self.last_frame_num = -1
        self.dropped_frames = 0
        self.invalid_headers = 0
        self.partial_frames = 0
        
        # 帧队列
        self.frame_queue = queue.Queue(maxsize=5)
        
        # 视频写入器
        self.video_writer = None
        self._writer_inited = False
    
    def _get_format(self, header_format):
        """获取使用的像素格式"""
        if self.force_format == 'yuyv':
            return FMT_YUYV
        elif self.force_format == 'uyvy':
            return FMT_UYVY
        elif header_format in (FMT_YUYV, FMT_UYVY):
            return header_format
        return FMT_YUYV  # 默认
    
    def _yuv422_to_bgr(self, data, width, height, fmt):
        """YUV422转BGR"""
        if not HAS_OPENCV:
            return None
        
        yuv = np.frombuffer(data, dtype=np.uint8).reshape((height, width, 2))
        
        if fmt == FMT_UYVY:
            return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_UYVY)
        else:  # YUYV
            return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_YUY2)
    
    def _init_writer(self, width, height, fps=60):
        """初始化视频写入器"""
        if not self.output_file or not HAS_OPENCV or self._writer_inited:
            return
        
        fourcc = cv2.VideoWriter_fourcc(*'XVID')
        self.video_writer = cv2.VideoWriter(
            self.output_file, fourcc, fps, (width, height))
        self._writer_inited = True
        print(f"[保存] 视频将保存到: {self.output_file}")
    
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
        """设置UDP"""
        print(f"[网络] 创建UDP服务器，端口: {self.port}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8*1024*1024)
        self.sock.bind(('0.0.0.0', self.port))
        self.sock.settimeout(1.0)
        print("[网络] 等待数据...")
    
    def _setup_tcp(self):
        """设置TCP"""
        print(f"[网络] 创建TCP服务器，端口: {self.port}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8*1024*1024)
        self.sock.bind(('0.0.0.0', self.port))
        self.sock.listen(1)
        self.sock.settimeout(1.0)
        print("[网络] 等待连接...")
        
        while self.running:
            try:
                self.client_sock, addr = self.sock.accept()
                print(f"[网络] 连接来自: {addr}")
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
                    result = self._receive_frame_tcp()
                else:
                    result = self._receive_frame_udp()
                
                if result:
                    header, payload = result
                    
                    if HAS_OPENCV:
                        fmt = self._get_format(header.format)
                        bgr = self._yuv422_to_bgr(
                            payload, header.width, header.height, fmt)
                        
                        if bgr is not None:
                            self._init_writer(header.width, header.height)
                            try:
                                self.frame_queue.put_nowait(bgr)
                            except queue.Full:
                                pass
                    
                    self.frame_count += 1
                    
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"[错误] 接收异常: {e}")
                break
        
        print("[接收] 线程退出")
    
    def _receive_frame_udp(self):
        """接收UDP帧"""
        data, addr = self.sock.recvfrom(65535)
        self.bytes_received += len(data)
        
        if len(data) < FRAME_HEADER_SIZE:
            if self.debug:
                print(f"[调试] 包太小: {len(data)}B")
            return None
        
        header = FrameHeader(data[:FRAME_HEADER_SIZE])
        
        if not header.is_valid():
            self.invalid_headers += 1
            if self.debug and self.invalid_headers <= 10:
                print(f"[调试] 无效帧头 #{self.invalid_headers}, "
                      f"magic=0x{header.magic:08X}")
            return None
        
        if self.debug and self.frame_count == 0:
            print(f"[调试] 首帧: {header}")
        
        # 丢帧检测
        if self.last_frame_num >= 0:
            expected = self.last_frame_num + 1
            if header.frame_num > expected:
                self.dropped_frames += header.frame_num - expected
        self.last_frame_num = header.frame_num
        
        # 收集帧数据
        frame_data = bytearray()
        if len(data) > FRAME_HEADER_SIZE:
            frame_data.extend(data[FRAME_HEADER_SIZE:])
        
        while len(frame_data) < header.frame_size:
            try:
                chunk, _ = self.sock.recvfrom(65535)
                frame_data.extend(chunk)
                self.bytes_received += len(chunk)
            except socket.timeout:
                self.partial_frames += 1
                if self.debug:
                    print(f"[调试] 帧不完整: {len(frame_data)}/{header.frame_size}")
                return None
        
        payload = bytes(frame_data[:header.frame_size])
        
        if self.debug and self.frame_count == 0:
            print(f"[调试] 首帧数据前32B: {payload[:32].hex()}")
        
        return header, payload
    
    def _receive_frame_tcp(self):
        """接收TCP帧"""
        if not self.client_sock:
            return None
        
        # 接收帧头
        data = self._recv_exact(self.client_sock, FRAME_HEADER_SIZE)
        if not data:
            return None
        
        header = FrameHeader(data)
        if not header.is_valid():
            return None
        
        # 丢帧检测
        if self.last_frame_num >= 0:
            expected = self.last_frame_num + 1
            if header.frame_num > expected:
                self.dropped_frames += header.frame_num - expected
        self.last_frame_num = header.frame_num
        
        # 接收帧数据
        frame_data = self._recv_exact(self.client_sock, header.frame_size)
        if frame_data:
            self.bytes_received += len(frame_data)
            if self.debug and self.frame_count == 0:
                print(f"[调试] 首帧数据前32B: {frame_data[:32].hex()}")
        
        return (header, frame_data) if frame_data else None
    
    def _recv_exact(self, sock, size):
        """精确接收指定字节"""
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
        print("\n========== 开始接收视频流 ==========")
        print("按 'q' 键退出")
        print("=====================================\n")
        
        last_stat_time = time.time()
        
        try:
            while self.running:
                now = time.time()
                
                # 每秒统计
                if now - last_stat_time >= 1.0:
                    self._print_stats()
                    last_stat_time = now
                
                if HAS_OPENCV:
                    try:
                        frame = self.frame_queue.get(timeout=0.1)
                        
                        cv2.imshow('Video Stream', frame)
                        
                        if self.video_writer:
                            self.video_writer.write(frame)
                        
                        if cv2.waitKey(1) & 0xFF == ord('q'):
                            break
                            
                    except queue.Empty:
                        if cv2.waitKey(1) & 0xFF == ord('q'):
                            break
                else:
                    time.sleep(0.1)
                    
        except KeyboardInterrupt:
            print("\n[中断]")
        
        self.running = False
        self._cleanup()
    
    def _print_stats(self):
        """打印统计"""
        if not self.start_time:
            return
        
        elapsed = time.time() - self.start_time
        fps = self.frame_count / elapsed if elapsed > 0 else 0
        mbps = self.bytes_received * 8 / elapsed / 1e6 if elapsed > 0 else 0
        
        stats = f"帧: {self.frame_count:6d} | FPS: {fps:5.1f} | 码率: {mbps:6.1f} Mbps"
        
        if self.dropped_frames > 0:
            stats += f" | 丢帧: {self.dropped_frames}"
        if self.invalid_headers > 0:
            stats += f" | 无效头: {self.invalid_headers}"
        if self.partial_frames > 0:
            stats += f" | 不完整: {self.partial_frames}"
        
        print(stats)
    
    def _cleanup(self):
        """清理资源"""
        print("\n[清理] 释放资源...")
        
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
            print(f"\n========== 统计 ==========")
            print(f"  总帧数:   {self.frame_count}")
            print(f"  运行时间: {elapsed:.1f} 秒")
            if elapsed > 0:
                print(f"  平均FPS:  {self.frame_count/elapsed:.1f}")
            print(f"  接收数据: {self.bytes_received/1024/1024:.1f} MB")
            print(f"  丢帧:     {self.dropped_frames}")
            print(f"  无效头:   {self.invalid_headers}")
            print(f"  不完整:   {self.partial_frames}")
            
            if self.frame_count == 0 and self.bytes_received > 0:
                print("\n[诊断] 收到数据但无有效帧:")
                print("  1. 检查发送端是否正确发送帧头")
                print("  2. 使用 -d 参数查看详细调试信息")
                print("  3. 检查网络连接和防火墙设置")


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='网络视频流接收程序',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
数据流架构:
  CameraLink(16-bit) → Video In → Width Converter(32-bit) →
  VDMA S2MM → DDR(YUV422/YUYV) → 网络 → 本程序

示例:
  %(prog)s -p 5000              # UDP模式
  %(prog)s -p 5000 -t           # TCP模式
  %(prog)s -p 5000 -o out.avi   # 保存视频
  %(prog)s -p 5000 -d           # 调试模式
        ''')
    
    parser.add_argument('-p', '--port', type=int, default=5000,
                        help='监听端口 (默认: 5000)')
    parser.add_argument('-t', '--tcp', action='store_true',
                        help='使用TCP协议')
    parser.add_argument('-o', '--output', type=str, default=None,
                        help='输出视频文件 (如: output.avi)')
    parser.add_argument('-d', '--debug', action='store_true',
                        help='调试模式')
    parser.add_argument('--force-format', type=str, default='auto',
                        choices=['auto', 'yuyv', 'uyvy'],
                        help='强制像素格式 (默认: auto)')
    
    args = parser.parse_args()
    
    print("================================================")
    print("    CameraLink 网络视频流接收")
    print("================================================")
    print(f"协议: {'TCP' if args.tcp else 'UDP'}")
    print(f"端口: {args.port}")
    print(f"输出: {args.output if args.output else '无'}")
    print(f"调试: {'开启' if args.debug else '关闭'}")
    print(f"格式: {args.force_format}")
    print(f"OpenCV: {'已安装' if HAS_OPENCV else '未安装'}")
    print("================================================\n")
    
    receiver = VideoReceiver(
        args.port, 
        args.tcp, 
        args.output, 
        args.debug,
        args.force_format
    )
    
    receiver.start()
    print("\n程序退出")


if __name__ == '__main__':
    main()
