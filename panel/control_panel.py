"""
control_panel.py — 控制面板启动入口

本模块是小智值班控制面板的命令行启动入口，主要功能：
1. 解析命令行参数（监听地址、端口、设备 IP、模式选择等）
2. 调用 server.start_server() 启动 HTTP 服务器

使用方式：
    python control_panel.py                          # 默认配置启动
    python control_panel.py --device-host 10.0.0.1   # 指定设备 IP
    python control_panel.py --live                   # 强制连接真实设备
    python control_panel.py --no-browser             # 不自动打开浏览器
    python control_panel.py --port 8080              # 指定监听端口
"""

import sys
import os
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from server import start_server, XWEBD_PORT, SAIR_PORT


def main():
    """主函数：解析命令行参数并启动控制面板服务器

    支持的命令行参数：
    --host: 服务器监听地址（默认 0.0.0.0，即所有网卡）
    --port: 服务器监听端口（默认 3000）
    --device-host: 设备 IP 地址（默认 192.168.1.96）
    --no-browser: 不自动打开浏览器
    --live: 强制 LIVE 模式（连接真实设备，不使用 Mock 模式）
    """
    parser = argparse.ArgumentParser(description="xiaozhi-zhiban 控制面板")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址 (默认: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=3000, help="监听端口 (默认: 3000)")
    parser.add_argument("--device-host", default="192.168.1.96", help="设备IP地址 (默认: 192.168.1.96)")
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    parser.add_argument("--live", action="store_true", help="强制Live模式（连接真实设备）")
    args = parser.parse_args()

    start_server(
        host=args.host,
        port=args.port,
        device_host=args.device_host,
        open_browser=not args.no_browser,
        force_live=args.live,
    )


if __name__ == "__main__":
    main()
