#!/usr/bin/env python3
"""
设备诊断脚本 - 全面诊断设备状态
支持快速模式和完整模式

用法: python diagnose_device.py [--quick]
      --quick: 快速模式，仅检查sair状态和关键日志
"""
import telnetlib
import time
import sys
import argparse
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]

def run_cmd(tn, cmd, wait=1.0):
    tn.write((cmd + "\n").encode())
    time.sleep(wait)
    return tn.read_very_eager().decode('utf-8', errors='ignore')

def diagnose_quick(tn):
    print("=" * 60)
    print("设备快速诊断")
    print("=" * 60)

    print("\n[1] sair进程状态")
    output = run_cmd(tn, "ps | grep sair | grep -v grep")
    print(output if output.strip() else "sair 未运行")

    print("\n[2] 关键内核日志 (崩溃/音频)")
    output = run_cmd(tn, 'dmesg | grep -iE "SIG=|crash|audio_track|Opus|tts|write_pcm|open_track" | tail -20', wait=2)
    print(output if output.strip() else "(无相关日志)")

    print("\n[3] 持久化日志 (xiaozhi.log)")
    output = run_cmd(tn, "cat /var/upgrade/xiaozhi.log 2>/dev/null | tail -10", wait=1)
    print(output if output.strip() else "(无日志)")

    print("\n诊断完成")

def diagnose_full(tn):
    print("=" * 60)
    print("设备全面诊断")
    print("=" * 60)

    print("\n[1] 进程列表 - 相关进程")
    output = run_cmd(tn, "ps | grep -E 'sair|smart_player|audio_service|msg_server|manager|launcher' | grep -v grep", wait=1)
    print(output)

    print("\n[2] 内存使用")
    output = run_cmd(tn, "free -m", wait=1)
    print(output)

    print("\n[3] 存储使用 (/var/upgrade)")
    output = run_cmd(tn, "df -h /var/upgrade", wait=1)
    print(output)

    print("\n[4] 共享内存")
    output = run_cmd(tn, "ls -la /dev/shm/ | head -20", wait=1)
    print(output)

    print("\n[5] 信号量")
    output = run_cmd(tn, "ls -la /dev/shm/sem.* 2>/dev/null || echo '(无信号量)'", wait=1)
    print(output)

    print("\n[6] Socket文件")
    output = run_cmd(tn, "ls -la /tmp/service/ 2>/dev/null || echo '(无socket文件)'", wait=1)
    print(output)

    print("\n[7] 消息队列")
    output = run_cmd(tn, "ls -la /*_mq 2>/dev/null || echo '(无消息队列)'", wait=1)
    print(output)

    print("\n[8] sair进程详情")
    output = run_cmd(tn, "pidof sair", wait=1)
    sair_pid = output.strip().split()[0] if output.strip() else None
    if sair_pid:
        print(f"sair PID: {sair_pid}")
        output = run_cmd(tn, f"cat /proc/{sair_pid}/status | grep -E 'Name|State|VmRSS|Threads'", wait=1)
        print(output)
        output = run_cmd(tn, f"ls -la /proc/{sair_pid}/fd | head -10", wait=1)
        print("文件描述符:")
        print(output)
    else:
        print("sair 未运行")

    print("\n[9] 内核日志 (最近30行)")
    output = run_cmd(tn, "dmesg | tail -30", wait=1)
    print(output)

    print("\n[10] 持久化日志 (xiaozhi.log)")
    output = run_cmd(tn, "cat /var/upgrade/xiaozhi.log 2>/dev/null | tail -20", wait=1)
    print(output if output.strip() else "(无日志)")

    print("\n[11] 崩溃日志 (crash.log)")
    output = run_cmd(tn, "cat /var/upgrade/crash.log 2>/dev/null | tail -30", wait=1)
    print(output if output.strip() else "(无崩溃日志)")

    print("\n[12] 网络连接")
    output = run_cmd(tn, "netstat -an 2>/dev/null | grep -E 'ESTABLISHED|LISTEN' | head -15", wait=1)
    print(output)

    print("\n" + "=" * 60)
    print("诊断完成")
    print("=" * 60)

def main():
    parser = argparse.ArgumentParser(description="设备诊断脚本")
    parser.add_argument("--quick", "-q", action="store_true", help="快速模式")
    args = parser.parse_args()

    try:
        print(f"连接设备 {DEVICE_HOST}:{DEVICE_PORT}...")
        tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=10)
        time.sleep(0.5)
        tn.read_very_eager()

        if args.quick:
            diagnose_quick(tn)
        else:
            diagnose_full(tn)

        tn.close()
    except Exception as e:
        print(f"连接设备失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
