#!/usr/bin/env python3
"""
网络诊断脚本 - 全面诊断设备与PC的网络状态
支持设备端和PC端双向检测

用法: python diagnose_network.py [--quick]
      --quick: 快速模式，仅检查基本连通性
"""
import telnetlib
import time
import socket
import subprocess
import sys
import argparse
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]

def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip

def run_cmd(tn, cmd, wait=1.0):
    tn.write((cmd + "\n").encode())
    time.sleep(wait)
    return tn.read_very_eager().decode('utf-8', errors='ignore')

def diagnose_full(tn, pc_ip):
    print("=" * 60)
    print("网络全面诊断")
    print("=" * 60)

    print(f"\n[PC端]")
    print(f"  PC IP: {pc_ip}")
    print(f"  设备 IP: {DEVICE_HOST}")

    print(f"\n[设备端] 网络接口")
    print(run_cmd(tn, "ifconfig"))

    print(f"\n[设备端] ARP表")
    print(run_cmd(tn, "arp -a"))

    print(f"\n[设备端] 路由表")
    print(run_cmd(tn, "route -n"))

    print(f"\n[设备端] 监听端口")
    print(run_cmd(tn, "netstat -tln"))

    print(f"\n[设备端] DNS配置")
    print(run_cmd(tn, "cat /etc/resolv.conf"))

    print(f"\n[设备端] 测试外网连通性")
    print(run_cmd(tn, "ping -c 2 8.8.8.8 2>&1"))

    print(f"\n[设备端] 测试DNS解析")
    print(run_cmd(tn, "nslookup api.tenclass.net 2>&1 || echo 'nslookup not available'"))

    print(f"\n[设备端] 测试HTTPS连接")
    print(run_cmd(tn, "wget -T 5 -O /dev/null https://api.tenclass.net 2>&1 | head -5"))

    print(f"\n[设备端] 防火墙状态")
    print(run_cmd(tn, "iptables -L 2>/dev/null || echo 'no iptables'"))

    print(f"\n[PC端] ARP表")
    try:
        result = subprocess.run(['arp', '-a'], capture_output=True, text=True, timeout=5)
        print(result.stdout[:1000])
    except:
        print("  无法获取PC ARP表")

    print(f"\n[PC端] 测试到设备的连通性")
    try:
        result = subprocess.run(['ping', '-n', '-w', '2', DEVICE_HOST], 
                              capture_output=True, text=True, timeout=5)
        print(result.stdout[:500])
    except:
        print("  ping失败")

    print("\n" + "=" * 60)
    print("诊断完成")
    print("=" * 60)

def diagnose_quick(tn, pc_ip):
    print("=" * 60)
    print("网络快速诊断")
    print("=" * 60)

    print(f"\nPC IP: {pc_ip}, 设备 IP: {DEVICE_HOST}")

    print(f"\n[设备端] 网络接口")
    output = run_cmd(tn, "ifconfig | head -10")
    print(output)

    print(f"\n[设备端] 测试外网")
    output = run_cmd(tn, "ping -c 1 8.8.8.8 2>&1 | head -3")
    print(output)

    print(f"\n[设备端] DNS解析")
    output = run_cmd(tn, "nslookup api.tenclass.net 2>&1 | head -5")
    print(output)

    print("\n诊断完成")

def main():
    parser = argparse.ArgumentParser(description="网络诊断脚本")
    parser.add_argument("--quick", action="store_true", help="快速模式")
    args = parser.parse_args()

    pc_ip = get_lan_ip()
    
    try:
        print(f"连接设备 {DEVICE_HOST}:{DEVICE_PORT}...")
        tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=10)
        time.sleep(0.5)
        tn.read_very_eager()

        if args.quick:
            diagnose_quick(tn, pc_ip)
        else:
            diagnose_full(tn, pc_ip)

        tn.close()
    except Exception as e:
        print(f"连接设备失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
