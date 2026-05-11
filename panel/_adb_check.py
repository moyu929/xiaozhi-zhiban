import subprocess, os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_adb_result.txt")

def run(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return r.stdout.strip(), r.stderr.strip(), r.returncode
    except Exception as e:
        return "", str(e), -1

lines = []

lines.append("=== 1. ADB kill-server ===")
out, err, rc = run(["adb", "kill-server"])
lines.append(f"rc={rc} out={out} err={err}")

lines.append("\n=== 2. ADB start-server ===")
out, err, rc = run(["adb", "start-server"])
lines.append(f"rc={rc} out={out} err={err}")

lines.append("\n=== 3. ADB devices ===")
out, err, rc = run(["adb", "devices", "-l"])
lines.append(f"rc={rc}\n{out}")
if err: lines.append(f"STDERR: {err}")

lines.append("\n=== 4. 尝试USB检测 ===")
out, err, rc = run(["adb", "devices"])
lines.append(f"rc={rc}\n{out}")

lines.append("\n=== 5. 尝试无线连接 ===")
device_host = os.environ.get("XIAOZHI_DEVICE_HOST", "")
if device_host:
    out, err, rc = run(["adb", "connect", f"{device_host}:5555"])
    lines.append(f"adb connect {device_host}:5555 -> rc={rc} out={out} err={err}")
else:
    lines.append("跳过: 未设置 XIAOZHI_DEVICE_HOST 环境变量")

import time
time.sleep(2)

lines.append("\n=== 6. 再次检查设备 ===")
out, err, rc = run(["adb", "devices", "-l"])
lines.append(f"rc={rc}\n{out}")

lines.append("\n=== 7. 检查USB设备列表 ===")
out, err, rc = run(["powershell", "-Command", "Get-PnpDevice | Where-Object {$_.Class -eq 'AndroidUsbDeviceClass' -or $_.FriendlyName -like '*Android*' -or $_.FriendlyName -like '*ADB*'} | Format-Table -AutoSize"])
lines.append(f"rc={rc}\n{out}")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print("DONE")
