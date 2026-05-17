import telnetlib
import time
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]

def run(tn, cmd, wait=1.0):
    tn.write((cmd + "\n").encode())
    time.sleep(wait)
    return tn.read_very_eager().decode("utf-8", errors="ignore")

def force_reboot(tn):
    print("尝试强制重启设备...")
    methods = [
        ("sync + reboot -f", "sync; sync; reboot -f"),
        ("sysrq-trigger", "echo b > /proc/sysrq-trigger"),
        ("reboot syscall", "reboot"),
    ]
    for name, cmd in methods:
        try:
            print(f"  方法: {name} -> {cmd}")
            tn.write((cmd + "\n").encode())
            time.sleep(0.5)
            try:
                output = tn.read_very_eager().decode("utf-8", errors="ignore")
                if output.strip():
                    print(f"  响应: {output.strip()[:200]}")
            except:
                pass
        except Exception as e:
            print(f"  方法 {name} 失败: {e}")
            break

def wait_for_boot(timeout=60):
    print(f"等待设备重启 (最多{timeout}秒)...")
    start = time.time()
    connected = False

    while time.time() - start < timeout:
        try:
            tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=5)
            time.sleep(0.5)
            result = run(tn, "echo ALIVE", 1.0)
            if "ALIVE" in result:
                elapsed = int(time.time() - start)
                print(f"设备已启动! (耗时 {elapsed} 秒)")
                connected = True
                return tn
            tn.close()
        except:
            pass
        if not connected:
            sys.stdout.write(".")
            sys.stdout.flush()
            time.sleep(3)

    print(f"\n超时! 设备在{timeout}秒内未启动")
    return None

def check_logs(tn):
    print("\n=== 检查启动日志 ===")

    result = run(tn, "dmesg | grep -iE 'xiaozhi|sair|CRASH|signal|dispatch|get_msg|feed_data|asr_engine' | tail -80", 3.0)
    print("dmesg日志:")
    print(result)

    result = run(tn, "ps | grep sair", 1.0)
    print("sair进程:")
    print(result)

    result = run(tn, "cat /tmp/xiaozhi_asr.log 2>/dev/null | tail -20", 1.0)
    print("ASR日志:")
    print(result if result.strip() else "(空)")

    result = run(tn, "cat /tmp/xiaozhi_audio.log 2>/dev/null | tail -5", 1.0)
    print("音频日志(最后5行):")
    print(result if result.strip() else "(空)")

    result = run(tn, "cat /tmp/xiaozhi_feed.log 2>/dev/null | tail -10", 1.0)
    print("Feed日志:")
    print(result if result.strip() else "(空)")

def main():
    print("=== 设备强制重启工具 ===")

    try:
        tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=10)
        time.sleep(0.3)

        result = run(tn, "uptime", 1.0)
        print(f"设备当前状态: {result.strip()}")

        force_reboot(tn)

        try:
            tn.close()
        except:
            pass
    except Exception as e:
        print(f"连接失败: {e}")
        print("设备可能已离线，尝试等待启动...")

    tn = wait_for_boot(timeout=60)

    if tn:
        time.sleep(2)
        check_logs(tn)
        tn.close()
    else:
        print("无法连接设备，请手动检查")

if __name__ == "__main__":
    main()
