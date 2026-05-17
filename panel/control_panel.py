"""
control_panel.py — 控制面板启动入口

本模块是小智智伴控制面板的命令行启动入口，主要功能：
1. 解析命令行参数（监听地址、端口、设备 IP 等）
2. 调用 server.start_server() 启动 HTTP 服务器

使用方式：
    python control_panel.py                          # 默认配置启动
    python control_panel.py --device-host 10.0.0.1   # 指定设备 IP
    python control_panel.py --no-browser             # 不自动打开浏览器
    python control_panel.py --port 3000              # 指定监听端口
"""

import sys
import os
import argparse
import socket
import subprocess
import time
import logging

logger = logging.getLogger("panel.control_panel")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from server import start_server, XWEBD_PORT
from config import DEFAULT_DEVICE_HOST, DEFAULT_PANEL_PORT


def _kill_port_owner(port):
    """查找并终止占用指定端口的进程

    通过 netstat 查找占用端口的 PID，然后强制终止该进程。
    支持 Windows 和 Linux/macOS 平台。

    Args:
        port: 要检查的端口号
    """
    try:
        if sys.platform == "win32":
            r = subprocess.run(
                f'netstat -ano | findstr ":{port} " | findstr "LISTENING"',
                capture_output=True, text=True, timeout=5, shell=True,
            )
            for line in r.stdout.strip().split("\n"):
                parts = line.strip().split()
                if len(parts) >= 5:
                    pid = parts[-1]
                    subprocess.run(f"taskkill /PID {pid} /F", capture_output=True, timeout=5, shell=True)
                    logger.info("终止占用端口 %d 的进程 (PID: %s)", port, pid)
                    print(f"  已终止占用端口 {port} 的进程 (PID: {pid})")
                    time.sleep(0.5)
        else:
            r = subprocess.run(
                f"lsof -ti:{port}", capture_output=True, text=True, timeout=5, shell=True,
            )
            for pid in r.stdout.strip().split("\n"):
                if pid.strip():
                    subprocess.run(f"kill -9 {pid.strip()}", capture_output=True, timeout=5, shell=True)
                    logger.info("终止占用端口 %d 的进程 (PID: %s)", port, pid.strip())
                    print(f"  已终止占用端口 {port} 的进程 (PID: {pid.strip()})")
                    time.sleep(0.5)
    except Exception as e:
        logger.debug("终止端口占用进程异常: %s", e)


def _is_port_in_use(port):
    """检测指定端口是否被占用

    Args:
        port: 要检查的端口号

    Returns:
        bool: True 表示端口被占用，False 表示端口空闲
    """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1)
            result = s.connect_ex(("127.0.0.1", port))
            logger.debug("端口 %d 检查: %s", port, "占用" if result == 0 else "空闲")
            return result == 0
    except Exception:
        return False


def main():
    """主函数：解析命令行参数并启动控制面板服务器

    支持的命令行参数：
    --host: 服务器监听地址（默认 0.0.0.0，即所有网卡）
    --port: 服务器监听端口（默认 3000）
    --device-host: 设备 IP 地址（默认从环境变量 XIAOZHI_DEVICE_HOST 读取，未设置则需手动指定）
    --no-browser: 不自动打开浏览器
    """
    from log_config import setup_logging
    setup_logging()

    parser = argparse.ArgumentParser(description="xiaozhi-zhiban 控制面板")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址 (默认: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=DEFAULT_PANEL_PORT, help=f"监听端口 (默认: {DEFAULT_PANEL_PORT})")
    parser.add_argument("--device-host", default=DEFAULT_DEVICE_HOST, help="设备IP地址 (环境变量: XIAOZHI_DEVICE_HOST)")
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = parser.parse_args()

    logger.info("启动参数: host=%s, port=%d, device=%s, browser=%s", args.host, args.port, args.device_host, not args.no_browser)

    if _is_port_in_use(args.port):
        logger.warning("端口 %d 已被占用, 正在关闭已有服务器", args.port)
        print(f"检测到端口 {args.port} 已被占用，正在关闭已有服务器...")
        _kill_port_owner(args.port)
        time.sleep(1)
        if _is_port_in_use(args.port):
            logger.warning("端口 %d 仍被占用, 尝试强制启动", args.port)
            print(f"警告：端口 {args.port} 仍被占用，尝试强制启动")

    start_server(
        host=args.host,
        port=args.port,
        device_host=args.device_host,
        open_browser=not args.no_browser,
    )


if __name__ == "__main__":
    main()
