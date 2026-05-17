#!/usr/bin/env python3
"""
从设备下载文件 (NC反向方式)
PC监听端口，设备通过nc连接发送文件
支持智能端口选择，自动检测可用端口

用法: python download_nc_win.py <远程路径> <本地路径>
示例: python download_nc_win.py /usr/bin/sair 设备二进制/sair_original
"""
import telnetlib
import time
import socket
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]
DEFAULT_PORT = cfg["nc_download_port"]
PORT_RANGE = range(cfg["nc_download_port_range"][0], cfg["nc_download_port_range"][1])

def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip

def find_available_port():
    for port in PORT_RANGE:
        try:
            test_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            test_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            test_sock.bind(("0.0.0.0", port))
            test_sock.close()
            return port
        except OSError:
            continue
    return DEFAULT_PORT

def download_file(remote_path, local_path, port=None):
    pc_ip = get_lan_ip()
    nc_port = port or find_available_port()
    
    print(f"PC IP: {pc_ip}")
    print(f"使用端口: {nc_port}")

    os.makedirs(os.path.dirname(local_path) or ".", exist_ok=True)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", nc_port))
    server.listen(1)
    server.settimeout(30)
    print(f"监听端口 {nc_port}...")

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=5)
    time.sleep(0.5)
    tn.read_very_eager()

    tn.write(f"ls -la {remote_path}\n".encode())
    time.sleep(0.5)
    check_output = tn.read_very_eager().decode('utf-8', errors='ignore')
    if "No such file" in check_output:
        print(f"错误: 远程文件不存在: {remote_path}")
        server.close()
        tn.close()
        return False

    cmd = f"nc {pc_ip} {nc_port} < {remote_path} &\n"
    print(f"设备命令: {cmd.strip()}")
    tn.write(cmd.encode())

    try:
        print("等待设备连接...")
        conn, addr = server.accept()
        print(f"设备已连接: {addr}")

        data = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            print(f"\r  已接收: {len(data)} 字节", end="", flush=True)
        print()

        conn.close()

        with open(local_path, "wb") as f:
            f.write(data)

        print(f"保存到: {local_path}")
        print(f"文件大小: {len(data)} 字节")
        return True

    except socket.timeout:
        print("超时! 设备未能连接")
        return False
    finally:
        server.close()
        tn.close()

def main():
    if len(sys.argv) < 3:
        print("用法: python download_nc_win.py <远程路径> <本地路径>")
        print("示例: python download_nc_win.py /usr/bin/sair 设备二进制/sair_original")
        sys.exit(1)

    remote_path = sys.argv[1]
    local_path = sys.argv[2]

    success = download_file(remote_path, local_path)
    
    if success:
        print("\n下载完成!")
    else:
        print("\n下载失败!")
        sys.exit(1)

if __name__ == "__main__":
    main()
