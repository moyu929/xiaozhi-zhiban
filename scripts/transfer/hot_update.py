r"""
热更新脚本 - 编译+上传+信号重启一步到位

原理：
  1. 编译新版sair（可选）
  2. 上传到设备 /var/upgrade/sair_new
  3. 发送 SIGUSR2 信号给sair进程
  4. sair进程收到SIGUSR2后（在信号处理器中）：
     - rename sair_new -> sair
     - execvp 替换进程（PID不变，Manager不察觉，看门狗不超时）
  5. 新进程启动后检测热更新标志，清理残留IPC资源

相比冷更新（上传+reboot）的优势：
  - 无需重启设备，3秒内完成
  - PID不变，Manager不会触发reboot
  - SCHED_RR调度保持，设备不会卡顿
  - 看门狗不超时，不会自动重启

用法 (Windows PowerShell):
    # 完整流程：编译+上传+热更新
    python scripts\transfer\hot_update.py

    # 仅上传+热更新（跳过编译）
    python scripts\transfer\hot_update.py --no-build

    # 仅编译
    python scripts\transfer\hot_update.py --build-only
"""

import telnetlib
import time
import socket
import os
import sys
import subprocess

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE_HOST = cfg["device_ip"]
DEVICE_PORT = cfg["device_telnet_port"]
NC_PORT = cfg["nc_upload_port"]
BUILD_DIR = cfg["_build_path"]
SAIR_LOCAL = cfg["_sair_local"]


def get_lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip


def build_sair():
    print("=" * 50)
    print(f"步骤 1/4: 编译 {cfg['project_dir']}")
    print("=" * 50)
    process = subprocess.Popen(
        [
            "wsl",
            "-e",
            "bash",
            "-c",
            f"cd {cfg['_wsl_project_path']} && bash build.sh 2>&1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    success = False
    for line in process.stdout:
        stripped = line.rstrip()
        if stripped:
            print(f"  {stripped}")
        if "Build successful" in line:
            success = True
    process.wait()
    if process.returncode != 0:
        print(f"编译失败! (exit code={process.returncode})")
        return False
    if not success:
        print("编译可能失败，未检测到 'Build successful'")
        return False
    print("✅ 编译成功")
    return True


def upload_to_device(local_file, remote_path):
    print(f"\n{'=' * 50}")
    print(f"步骤 2/4: 上传到设备 {remote_path}")
    print(f"{'=' * 50}")

    if not os.path.exists(local_file):
        print(f"错误: 文件不存在: {local_file}")
        return False

    file_size = os.path.getsize(local_file)
    lan_ip = get_lan_ip()

    print(f"本机LAN IP: {lan_ip}")
    print(f"文件: {local_file} ({file_size/1024:.1f} KB)")

    with open(local_file, "rb") as f:
        data = f.read()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", NC_PORT))
    sock.listen(1)
    sock.settimeout(60)

    print(f"监听端口 {NC_PORT}...")

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT)
    time.sleep(0.5)

    def run(cmd, wait=1.0):
        tn.write((cmd + "\n").encode())
        time.sleep(wait)
        return tn.read_very_eager().decode("utf-8", errors="ignore")

    run(f"rm -f {remote_path}")

    cmd = f"nc {lan_ip} {NC_PORT} > {remote_path} &"
    print(f"设备执行: {cmd}")
    tn.write((cmd + "\n").encode())
    time.sleep(2)

    try:
        conn, addr = sock.accept()
        print(f"设备已连接: {addr}")

        sent = 0
        chunk_size = 8192
        while sent < len(data):
            chunk = data[sent : sent + chunk_size]
            conn.sendall(chunk)
            sent += len(chunk)
            pct = sent * 100 // len(data)
            if sent % (100 * 1024) < chunk_size or sent == len(data):
                print(f"  进度: {sent}/{len(data)} ({pct}%)")

        conn.close()
        print("数据发送完毕, 等待设备写入...")
        time.sleep(2)

        output = run(f"ls -lh {remote_path}")
        if "No such" not in output and file_size > 0:
            run(f"chmod +x {remote_path}")
            print("✅ 上传成功")
            return True
        else:
            print("❌ 上传失败")
            return False

    except socket.timeout:
        print("❌ 超时! 设备未能连接")
        return False
    finally:
        sock.close()
        tn.close()


def send_hot_update_signal():
    print(f"\n{'=' * 50}")
    print("步骤 3/4: 发送 SIGUSR2 热更新信号")
    print(f"{'=' * 50}")

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT)
    time.sleep(0.5)

    def run(cmd, wait=1.0):
        tn.write((cmd + "\n").encode())
        time.sleep(wait)
        return tn.read_very_eager().decode("utf-8", errors="ignore")

    output = run("ps | grep sair | grep -v grep")
    print(f"当前sair进程:\n{output}")

    pid = None
    for line in output.strip().split("\n"):
        parts = line.split()
        if len(parts) >= 2 and "sair" in line:
            try:
                pid = int(parts[0])
                break
            except ValueError:
                continue

    if not pid:
        print("❌ 未找到sair进程")
        tn.close()
        return False, None

    state = None
    for line in output.strip().split("\n"):
        parts = line.split()
        if len(parts) >= 5 and "sair" in line:
            state = parts[3]
            break

    if state == "Z":
        print(f"⚠️ sair进程(PID={pid})处于zombie状态，热更新无效!")
        print("  原因: 进程已死亡，无法接收信号")
        print("  建议: 使用 force_reboot.py 重启设备后重新部署")
        tn.close()
        return False, None

    print(f"sair PID: {pid} (state={state})")

    output = run("ls -la /var/upgrade/sair /var/upgrade/sair_new 2>&1")
    sair_ts = ""
    sair_new_size = 0
    for line in output.strip().split("\n"):
        if "/var/upgrade/sair_new" in line:
            parts = line.split()
            if len(parts) >= 5:
                try:
                    sair_new_size = int(parts[4])
                except ValueError:
                    pass
        if "/var/upgrade/sair " in line or line.strip().endswith("/var/upgrade/sair"):
            sair_ts = line.strip()

    if sair_new_size < 100000:
        print(f"⚠️ sair_new文件过小({sair_new_size} bytes)，上传可能不完整!")
        print("  热更新可能失败，建议重新上传")
        tn.close()
        return False, None

    print(f"当前sair: {sair_ts}")
    print(f"sair_new大小: {sair_new_size} bytes")

    output = run(f"kill -USR2 {pid}", wait=0.5)
    print(f"已发送 SIGUSR2 到 PID {pid}")

    tn.close()
    return True, pid


def verify_restart(expected_pid=None):
    print(f"\n{'=' * 50}")
    print("步骤 4/4: 验证热更新结果")
    print(f"{'=' * 50}")

    print("等待新进程初始化 (5秒)...")
    time.sleep(5)

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT)
    time.sleep(0.5)

    def run(cmd, wait=1.0):
        tn.write((cmd + "\n").encode())
        time.sleep(wait)
        return tn.read_very_eager().decode("utf-8", errors="ignore")

    output = run("ps | grep sair | grep -v grep")
    print(f"sair进程状态:\n{output}")

    if "sair" not in output:
        print("❌ sair进程未找到")
        tn.close()
        return False

    pid_after = None
    state_after = None
    for line in output.strip().split("\n"):
        parts = line.split()
        if len(parts) >= 2 and "sair" in line:
            try:
                pid_after = int(parts[0])
                if len(parts) >= 5:
                    state_after = parts[3]
            except ValueError:
                pass

    if expected_pid and pid_after:
        if pid_after == expected_pid:
            print(f"✅ PID={pid_after} 不变，execvp热更新成功")
        else:
            print(f"❌ PID变化: {expected_pid} -> {pid_after}，热更新失败(进程被重启)")
            print("  可能原因: 上传不完整导致execvp失败，或进程在热更新后崩溃")
            tn.close()
            return False
    else:
        print("✅ sair进程运行中")

    if state_after == "Z":
        print("❌ sair进程处于zombie状态!")
        tn.close()
        return False

    output = run(
        "tail -30 /var/upgrade/xiaozhi.log | grep -E 'HOTUPDATE|BOOT|Hot.update'",
        wait=1.0,
    )
    if output.strip():
        print(f"\n热更新日志:\n{output}")
        if "Hot update detected" in output:
            print("✅ 日志确认热更新成功")
        elif "Normal boot" in output:
            print("⚠️ 日志显示正常启动(非热更新)，可能PID被复用")

    output = run(
        "ls -lh /var/upgrade/sair_new 2>/dev/null; ls -lh /var/upgrade/sair_old 2>/dev/null",
        wait=0.5,
    )
    if "sair_new" in output or "sair_old" in output:
        print(f"\n残留文件:\n{output}")
        print("提示: 残留文件会在下次启动时自动清理")
    else:
        print("\n无残留文件，热更新清理完成")

    output = run("ls -la /var/upgrade/sair", wait=0.5)
    print(f"当前sair文件:\n{output.strip()}")

    tn.close()
    return True


def main():
    no_build = "--no-build" in sys.argv
    build_only = "--build-only" in sys.argv
    no_clear = "--no-clear" in sys.argv

    print("╔══════════════════════════════════════════╗")
    print("║     XIAOZHI 热更新工具 v1.0            ║")
    print("╚══════════════════════════════════════════╝")
    print()

    if not no_build:
        if not build_sair():
            if not build_only:
                print("\n编译失败，是否继续上传旧版? (y/n)")
                ans = input().strip().lower()
                if ans != "y":
                    sys.exit(1)
            else:
                sys.exit(1)

    if build_only:
        print("\n仅编译模式，跳过上传和热更新")
        sys.exit(0)

    if not upload_to_device(SAIR_LOCAL, cfg["remote_sair_new_path"]):
        print("\n上传失败，终止热更新")
        sys.exit(1)

    signal_ok, pid = send_hot_update_signal()
    if not signal_ok:
        print("\n发送信号失败，终止热更新")
        sys.exit(1)

    if not verify_restart(expected_pid=pid):
        print("\n验证失败，请手动检查设备状态")
        sys.exit(1)

    if not no_clear:
        clear_device_log()

    print(f"\n{'=' * 50}")
    print("🎉 热更新完成！")
    print(f"{'=' * 50}")


def clear_device_log():
    print(f"\n{'=' * 50}")
    print("步骤 5/5: 清除设备日志")
    print(f"{'=' * 50}")

    tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT)
    time.sleep(0.5)

    def run(cmd, wait=1.0):
        tn.write((cmd + "\n").encode())
        time.sleep(wait)
        return tn.read_very_eager().decode("utf-8", errors="ignore")

    output = run("wc -c /var/upgrade/xiaozhi.log 2>/dev/null", wait=0.5)
    print(f"当前日志大小: {output.strip()}")

    run("echo -n '' > /var/upgrade/xiaozhi.log", wait=0.5)

    output = run("wc -c /var/upgrade/xiaozhi.log 2>/dev/null", wait=0.5)
    print(f"清除后大小: {output.strip()}")
    print("✅ 日志已清除")

    tn.close()


if __name__ == "__main__":
    main()
