import subprocess, os

OUT = r"d:\小智ai\xiaozhi-zhiban-develop\panel\_adb_result.txt"

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

lines.append("\n=== 5. 尝试无线连接 192.168.1.96:5555 ===")
out, err, rc = run(["adb", "connect", "192.168.1.96:5555"])
lines.append(f"rc={rc} out={out} err={err}")

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
