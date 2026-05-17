#!/usr/bin/env python3
"""
设备循环重启紧急修复脚本

两种循环重启场景：
1. 快速重启（几秒内）：wrapper脚本崩溃/程序秒退，ADB窗口极短(2-5秒)
   → 需要快速轮询(0.2秒间隔)抓住短暂窗口
2. 慢速重启（约50秒）：程序启动但未进入消息循环，看门狗超时，ADB窗口较长(30-50秒)
   → 可以慢速轮询(2秒间隔)，更稳定

本脚本采用自适应策略：先快速轮询（针对场景1），
如果检测到设备连接窗口较长（场景2），自动切换到慢速轮询。

使用方法：
    python scripts/device_check/emergency_fix.py           # 自适应模式（推荐）
    python scripts/device_check/emergency_fix.py --fast    # 快速重启专用（0.2秒间隔）
    python scripts/device_check/emergency_fix.py --slow    # 慢速重启专用（2秒间隔）
    python scripts/device_check/emergency_fix.py --all     # 全面修复：删除sair+test.sh+禁止看门狗

注意：设备必须通过 USB 连接到电脑！
"""

import subprocess
import time
import sys
import os
import argparse

ADB = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
                   "platform-tools", "adb.exe")

if not os.path.exists(ADB):
    ADB = "adb"

FAST_INTERVAL = 0.2
SLOW_INTERVAL = 2.0
FAST_MAX_RETRIES = 500
SLOW_MAX_RETRIES = 90
CMD_TIMEOUT_FAST = 3
CMD_TIMEOUT_SLOW = 5


def adb_shell(cmd, timeout=5):
    try:
        result = subprocess.run(
            [ADB, "shell", cmd],
            capture_output=True, text=True, timeout=timeout
        )
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return False, "", "timeout"
    except Exception as e:
        return False, "", str(e)


def adb_devices():
    try:
        result = subprocess.run(
            [ADB, "devices"],
            capture_output=True, text=True, timeout=3
        )
        return "\tdevice" in result.stdout
    except:
        return False


def try_remove_file(filepath, timeout=5):
    ok, out, err = adb_shell(f"rm -f {filepath}", timeout=timeout)
    if ok:
        ok2, out2, _ = adb_shell(f"ls {filepath} 2>&1", timeout=timeout)
        if "No such file" in out2 or "cannot access" in out2:
            return True
        else:
            print(f"  文件仍存在: {out2}")
    else:
        print(f"  命令失败: {err[:60]}")
    return False


def try_fix_watchdog_forbid():
    ok, out, err = adb_shell(
        "if [ -f /dev/shm/app_running_list ]; then "
        "offset=$(hexdump -C /dev/shm/app_running_list | grep -m1 '7361 6972' | awk '{print $1}'); "
        "if [ -n \"$offset\" ]; then "
        "dec_offset=$((16#$offset)); "
        "forbid_offset=$((dec_offset + 0x78)); "
        "printf '\\x01' | dd of=/dev/shm/app_running_list bs=1 seek=$forbid_offset conv=notrunc 2>/dev/null; "
        "echo 'forbidden'; "
        "else echo 'no_sair_entry'; fi; "
        "else echo 'no_shm'; fi"
    )
    if "forbidden" in out:
        return True
    return False


def do_fix(mode):
    if mode == "all":
        fixed = []
        if try_remove_file("/var/upgrade/sair"):
            fixed.append("sair wrapper")
        if try_remove_file("/var/upgrade/test.sh"):
            fixed.append("test.sh")
        if try_fix_watchdog_forbid():
            fixed.append("看门狗已禁止")
        return fixed
    else:
        if try_remove_file("/var/upgrade/sair"):
            return ["sair wrapper"]
        return []


def main():
    parser = argparse.ArgumentParser(description="设备循环重启紧急修复")
    parser.add_argument("--fast", action="store_true",
                        help="快速重启专用（0.2秒间隔，针对几秒内重启的场景）")
    parser.add_argument("--slow", action="store_true",
                        help="慢速重启专用（2秒间隔，针对约50秒看门狗超时重启的场景）")
    parser.add_argument("--all", action="store_true",
                        help="全面修复：删除sair+test.sh+禁止看门狗")
    args = parser.parse_args()

    print("=" * 58)
    print("  设备循环重启紧急修复")
    print("=" * 58)
    print()
    print("两种循环重启场景：")
    print("  场景1(快速): 设备启动几秒内就重启 → ADB窗口极短")
    print("  场景2(慢速): 设备启动约50秒后重启 → ADB窗口较长")
    print()
    print("请确保设备已通过 USB 连接到电脑！按 Ctrl+C 取消")
    print()

    if args.fast:
        strategy = "fast"
        interval = FAST_INTERVAL
        max_retries = FAST_MAX_RETRIES
        cmd_timeout = CMD_TIMEOUT_FAST
        print(f"策略: 快速轮询（间隔{interval}秒，{max_retries}次重试）")
    elif args.slow:
        strategy = "slow"
        interval = SLOW_INTERVAL
        max_retries = SLOW_MAX_RETRIES
        cmd_timeout = CMD_TIMEOUT_SLOW
        print(f"策略: 慢速轮询（间隔{interval}秒，{max_retries}次重试）")
    else:
        strategy = "adaptive"
        interval = FAST_INTERVAL
        max_retries = FAST_MAX_RETRIES
        cmd_timeout = CMD_TIMEOUT_FAST
        print(f"策略: 自适应（先快速轮询，检测到长窗口后自动切换慢速）")

    mode = "all" if args.all else "sair"
    if mode == "all":
        print("修复目标: sair wrapper + test.sh + 看门狗")
    else:
        print("修复目标: /var/upgrade/sair (删除后回退到原版sair)")
    print()

    success = False
    last_connect_time = 0
    connect_count = 0
    consecutive_disconnects = 0
    switched_to_slow = False

    for i in range(max_retries):
        try:
            if not adb_devices():
                consecutive_disconnects += 1
                if strategy == "adaptive" and not switched_to_slow and consecutive_disconnects > 30:
                    print(f"\n[{i+1}] 设备长时间未连接，可能不是快速重启场景")
                    print("    切换到慢速轮询模式...")
                    strategy = "slow"
                    interval = SLOW_INTERVAL
                    cmd_timeout = CMD_TIMEOUT_SLOW
                    switched_to_slow = True
                elif i % 50 == 0 and strategy == "fast":
                    print(f"[{i+1}] 等待设备... (快速轮询中)")
                elif i % 10 == 0 and strategy != "fast":
                    print(f"[{i+1}] 等待设备... (设备重启中)")
                time.sleep(interval)
                continue

            consecutive_disconnects = 0
            connect_count += 1
            now = time.time()
            window_duration = now - last_connect_time if last_connect_time > 0 else 0

            if strategy == "adaptive" and not switched_to_slow and window_duration > 30:
                print(f"\n[{i+1}] 检测到长连接窗口({window_duration:.0f}秒)，切换到慢速轮询")
                strategy = "slow"
                interval = SLOW_INTERVAL
                cmd_timeout = CMD_TIMEOUT_SLOW
                switched_to_slow = True

            if connect_count == 1:
                print(f"[{i+1}] 设备已连接！(第1次窗口)")
            else:
                print(f"[{i+1}] 设备已连接 (第{connect_count}次窗口)")

            fixed = do_fix(mode)
            if fixed:
                print()
                print("=" * 58)
                print(f"  修复成功！已处理: {', '.join(fixed)}")
                if "sair wrapper" in fixed:
                    print("  设备将使用原版 /usr/bin/sair 启动")
                if "看门狗已禁止" in fixed:
                    print("  看门狗已禁止，设备不会因超时而重启")
                print("=" * 58)
                success = True
                break
            else:
                if now - last_connect_time < 2 and strategy != "slow":
                    time.sleep(0.5)

            last_connect_time = now

        except KeyboardInterrupt:
            print("\n用户取消")
            return 1
        except Exception as e:
            if i % 20 == 0:
                print(f"[{i+1}] 错误: {str(e)[:60]}")

        time.sleep(interval)

    if success:
        print("\n正在重启设备到正常状态...")
        adb_shell("reboot", timeout=cmd_timeout)
        print("重启命令已发送，等待设备正常启动...")
        time.sleep(10)
        print("\n设备应已恢复正常。如仍未恢复，请尝试 --all 模式。")
        return 0
    else:
        print()
        print("=" * 58)
        print("  修复失败！")
        print("=" * 58)
        print()
        print("请尝试以下手动操作：")
        print(f"  1. {ADB} devices  (检查设备是否可见)")
        print(f"  2. {ADB} shell \"rm -f /var/upgrade/sair; reboot\"")
        print(f"  3. python {__file__} --fast  (快速重启场景)")
        print(f"  4. python {__file__} --slow  (慢速重启场景)")
        print(f"  5. python {__file__} --all   (全面修复)")
        print()
        print("如果以上都无效，可能需要通过串口连接设备。")
        return 1


if __name__ == "__main__":
    sys.exit(main())
