"""
adb_manager.py — ADB 设备管理

本模块封装了通过 ADB（Android Debug Bridge）管理设备的所有操作，主要功能包括：
1. ADB 命令执行：封装 adb 子进程调用，统一处理超时和错误
2. 设备检测：检测通过 USB 连接的 ADB 设备列表
3. xwebd 生命周期管理：部署、更新、移除、启动、停止、重启 xwebd 守护进程
4. 端口转发：设置 ADB forward，将本地端口映射到设备端口
5. 设备信息查询：获取设备 IP 地址、xwebd 安装/运行状态
6. 自启动配置：在设备端 test.sh 中添加/移除 xwebd 自启动项

关键概念：
- xwebd：设备端 HTTP 守护进程，部署在 /var/upgrade/xwebd
- sair：Assistant 的二进制文件名（平台约束不可改名），端口 8081
- ADB forward：通过 USB 将本地端口映射到设备端口，实现网络通信
"""

import subprocess
import shutil
import os
import time

ADB_TIMEOUT = 5

XWEBD_REMOTE_PATH = "/var/upgrade/xwebd"
XWEBD_LOG_PATH = "/var/upgrade/xwebd.log"
TEST_SH_PATH = "/var/upgrade/test.sh"
BOOT_WATCHDOG_PATH = "/var/upgrade/boot_watchdog.sh"


def _adb(args, serial=None, timeout=ADB_TIMEOUT):
    """执行 ADB 命令的底层封装

    Args:
        args: ADB 命令参数列表（如 ["devices", "-l"]）
        serial: 设备序列号，为 None 时不指定设备（适用于仅连接一台设备的情况）
        timeout: 命令超时时间（秒），默认 5 秒

    Returns:
        dict: {
            "ok": bool,          # 命令是否成功（returncode == 0）
            "stdout": str,       # 标准输出
            "stderr": str,       # 标准错误
            "returncode": int    # 退出码
        }
    """
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return {"ok": r.returncode == 0, "stdout": r.stdout.strip(), "stderr": r.stderr.strip(), "returncode": r.returncode}
    except subprocess.TimeoutExpired:
        return {"ok": False, "stdout": "", "stderr": "ADB command timed out", "returncode": -1}
    except FileNotFoundError:
        return {"ok": False, "stdout": "", "stderr": "ADB not found. Install Android SDK Platform Tools.", "returncode": -1}


def is_adb_available():
    """检查 ADB 是否已安装

    Returns:
        bool: True 表示 ADB 可用，False 表示未安装
    """
    return shutil.which("adb") is not None


def detect_devices():
    """检测通过 USB 连接的 ADB 设备列表

    执行 `adb devices -l` 命令，解析输出获取设备序列号、
    状态和型号信息。仅返回状态为 "device" 的已授权设备。

    Returns:
        list[dict]: 设备信息列表，每个元素包含：
            - "serial": 设备序列号
            - "state": 设备状态（"device"）
            - "model": 设备型号（可选）
            - "device": 设备名称（可选）
    """
    r = _adb(["devices", "-l"])
    if not r["ok"]:
        return []
    devices = []
    for line in r["stdout"].split("\n")[1:]:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        serial = parts[0]
        state = parts[1]
        if state != "device":
            continue
        info = {"serial": serial, "state": state}
        for p in parts[2:]:
            if p.startswith("model:"):
                info["model"] = p.split(":", 1)[1]
            elif p.startswith("device:"):
                info["device"] = p.split(":", 1)[1]
        devices.append(info)
    return devices


def is_xwebd_installed(serial=None):
    """检查设备上是否已安装 xwebd

    通过 `adb shell test -f` 检查 xwebd 二进制文件是否存在。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        bool: True 表示已安装，False 表示未安装或检查失败
    """
    r = _adb(["shell", f"test -f {XWEBD_REMOTE_PATH} && echo yes || echo no"], serial=serial)
    if not r["ok"]:
        return False
    return "yes" in r["stdout"]


def check_xwebd_status(serial=None):
    """检查设备上 xwebd 的运行状态

    通过 `adb shell pidof` 检查 xwebd 进程是否存在。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"running": True, "pid": "..."} 运行中
              {"running": False, "pid": None} 未运行
    """
    r = _adb(["shell", "pidof xwebd"], serial=serial)
    if r["ok"] and r["stdout"].strip():
        return {"running": True, "pid": r["stdout"].strip().split()[0]}
    return {"running": False, "pid": None}


def _find_xwebd_binary():
    """在本地文件系统中查找 xwebd 二进制文件

    按优先级搜索以下路径：
    1. ../device/xwebd/prebuilt/xwebd（预编译版本）
    2. ../device/xwebd/build/xwebd（本地编译版本）
    3. device/prebuilt/xwebd
    4. device/build/xwebd

    Returns:
        str: 找到的二进制文件路径（已规范化），未找到返回 None
    """
    base_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(base_dir, "..", "device", "xwebd", "prebuilt", "xwebd"),
        os.path.join(base_dir, "..", "device", "xwebd", "build", "xwebd"),
        os.path.join(base_dir, "device", "prebuilt", "xwebd"),
        os.path.join(base_dir, "device", "build", "xwebd"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return os.path.normpath(path)
    return None


def _find_watchdog_script():
    """在本地文件系统中查找 boot_watchdog.sh 看门狗脚本

    按优先级搜索以下路径：
    1. ../device/xwebd/scripts/boot_watchdog.sh
    2. device/scripts/boot_watchdog.sh

    Returns:
        str: 找到的脚本路径（已规范化），未找到返回 None
    """
    base_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(base_dir, "..", "device", "xwebd", "scripts", "boot_watchdog.sh"),
        os.path.join(base_dir, "device", "scripts", "boot_watchdog.sh"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return os.path.normpath(path)
    return None


def deploy_xwebd(serial=None, binary_path=None):
    """部署 xwebd 到设备

    完整的部署流程：
    1. 查找或使用指定的 xwebd 二进制文件
    2. 通过 adb push 推送到设备
    3. 设置可执行权限（chmod 755）
    4. 推送看门狗脚本（如果存在）
    5. 配置自启动
    6. 启动 xwebd

    Args:
        serial: 设备序列号，为 None 时不指定设备
        binary_path: xwebd 二进制文件的本地路径，
                     为 None 时自动搜索

    Returns:
        dict: {"ok": True, "binary": "..."} 成功
              {"ok": False, "error": "..."} 失败
    """
    if binary_path is None:
        binary_path = _find_xwebd_binary()
        if not binary_path:
            return {"ok": False, "error": "xwebd binary not found (no prebuilt or build)"}

    if not os.path.isfile(binary_path):
        return {"ok": False, "error": f"Binary not found: {binary_path}"}

    r = _adb(["push", binary_path, XWEBD_REMOTE_PATH], serial=serial, timeout=30)
    if not r["ok"]:
        return {"ok": False, "error": f"adb push failed: {r['stderr']}"}

    r = _adb(["shell", f"chmod 755 {XWEBD_REMOTE_PATH}"], serial=serial)
    if not r["ok"]:
        return {"ok": False, "error": f"chmod failed: {r['stderr']}"}

    # 推送看门狗脚本（可选，失败不影响部署）
    watchdog_script = _find_watchdog_script()
    if watchdog_script:
        r = _adb(["push", watchdog_script, BOOT_WATCHDOG_PATH], serial=serial, timeout=10)
        if r["ok"]:
            _adb(["shell", f"chmod 755 {BOOT_WATCHDOG_PATH}"], serial=serial)
        else:
            pass

    r = _ensure_xwebd_autostart(serial)
    if not r["ok"]:
        return {"ok": False, "error": f"autostart config failed: {r['error']}"}

    r = start_xwebd(serial)
    if not r["ok"]:
        return {"ok": False, "error": f"start xwebd failed: {r['error']}"}

    return {"ok": True, "binary": binary_path}


def update_xwebd(serial=None, binary_path=None):
    """更新设备上的 xwebd

    更新流程：
    1. 停止当前运行的 xwebd
    2. 推送新版本二进制文件
    3. 设置可执行权限
    4. 重新启动 xwebd

    Args:
        serial: 设备序列号，为 None 时不指定设备
        binary_path: xwebd 二进制文件的本地路径，
                     为 None 时自动搜索

    Returns:
        dict: {"ok": True, "binary": "..."} 成功
              {"ok": False, "error": "..."} 失败
    """
    if binary_path is None:
        binary_path = _find_xwebd_binary()
        if not binary_path:
            return {"ok": False, "error": "xwebd binary not found"}

    if not os.path.isfile(binary_path):
        return {"ok": False, "error": f"Binary not found: {binary_path}"}

    r = stop_xwebd(serial)
    time.sleep(1)

    r = _adb(["push", binary_path, XWEBD_REMOTE_PATH], serial=serial, timeout=30)
    if not r["ok"]:
        return {"ok": False, "error": f"adb push failed: {r['stderr']}"}

    r = _adb(["shell", f"chmod 755 {XWEBD_REMOTE_PATH}"], serial=serial)
    if not r["ok"]:
        return {"ok": False, "error": f"chmod failed: {r['stderr']}"}

    r = start_xwebd(serial)
    if not r["ok"]:
        return {"ok": False, "error": f"start xwebd failed: {r['error']}"}

    return {"ok": True, "binary": binary_path}


def remove_xwebd(serial=None):
    """从设备上移除 xwebd

    移除流程：
    1. 停止 xwebd 进程
    2. 删除 xwebd 二进制文件和日志文件
    3. 移除自启动配置

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True} 始终返回成功（即使文件不存在）
    """
    stop_xwebd(serial)
    time.sleep(0.5)

    _adb(["shell", f"rm -f {XWEBD_REMOTE_PATH}"], serial=serial)
    _adb(["shell", f"rm -f {XWEBD_LOG_PATH}"], serial=serial)
    _remove_xwebd_autostart(serial)

    return {"ok": True}


def start_xwebd(serial=None):
    """启动设备上的 xwebd 守护进程

    通过 `adb shell` 在后台启动 xwebd（-d 参数表示守护进程模式），
    等待 1 秒后通过 pidof 验证进程是否成功启动。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True, "pid": "..."} 启动成功
              {"ok": False, "error": "..."} 启动失败
    """
    r = _adb(["shell", f"{XWEBD_REMOTE_PATH} -d"], serial=serial, timeout=10)
    if not r["ok"]:
        return {"ok": False, "error": f"start failed: {r['stderr']}"}
    time.sleep(1)
    status = check_xwebd_status(serial)
    if not status["running"]:
        return {"ok": False, "error": "xwebd started but pidof check failed"}
    return {"ok": True, "pid": status["pid"]}


def stop_xwebd(serial=None):
    """停止设备上的 xwebd 守护进程

    先尝试优雅停止（killall），等待 0.5 秒后检查进程是否已退出。
    如果进程仍在运行，则强制终止（killall -9）。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True} 始终返回成功
    """
    _adb(["shell", "killall xwebd 2>/dev/null"], serial=serial)
    time.sleep(0.5)
    status = check_xwebd_status(serial)
    if status["running"]:
        # 优雅停止失败，强制终止
        _adb(["shell", "killall -9 xwebd 2>/dev/null"], serial=serial)
        time.sleep(0.5)
    return {"ok": True}


def restart_xwebd(serial=None):
    """重启设备上的 xwebd 守护进程

    先停止再启动。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True, "pid": "..."} 重启成功
              {"ok": False, "error": "..."} 重启失败
    """
    stop_xwebd(serial)
    return start_xwebd(serial)


def setup_forward(serial=None, local_xwebd=8080, local_sair=8081):
    """设置 ADB 端口转发

    将本地 TCP 端口映射到设备端口，使本机可以通过 localhost 访问设备服务：
    - 本地 local_xwebd → 设备 8080（xwebd 服务）
    - 本地 local_sair → 设备 8081（sair 服务，sair 是 Assistant 的二进制文件名，平台约束不可改名）

    Args:
        serial: 设备序列号，为 None 时不指定设备
        local_xwebd: 本地 xwebd 转发端口，默认 8080
        local_sair: 本地 sair 转发端口，默认 8081

    Returns:
        dict: {"ok": bool, "errors": list} 转发结果和错误信息列表
    """
    r1 = _adb(["forward", f"tcp:{local_xwebd}", f"tcp:8080"], serial=serial)
    r2 = _adb(["forward", f"tcp:{local_sair}", f"tcp:8081"], serial=serial)
    ok = r1["ok"] and r2["ok"]
    errors = []
    if not r1["ok"]:
        errors.append(f"xwebd forward: {r1['stderr']}")
    if not r2["ok"]:
        errors.append(f"sair forward: {r2['stderr']}")
    return {"ok": ok, "errors": errors}


def get_device_ip(serial=None):
    """获取设备的 WiFi IP 地址

    通过 `adb shell ip addr show wlan0` 获取设备 wlan0 接口的 IP 地址。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        str: IP 地址字符串，获取失败返回 None
    """
    r = _adb(["shell", "ip addr show wlan0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d/ -f1"], serial=serial)
    if r["ok"] and r["stdout"].strip():
        return r["stdout"].strip()
    return None


def _ensure_xwebd_autostart(serial=None):
    """确保设备端 test.sh 中包含 xwebd 自启动配置

    检查 test.sh 中是否已包含 xwebd 启动命令，
    如果没有则尝试通过 sed 插入，sed 失败则追加到文件末尾。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True} 配置成功
              {"ok": False, "error": "..."} 配置失败
    """
    r = _adb(["shell", f"grep -q xwebd {TEST_SH_PATH} && echo found || echo missing"], serial=serial)
    if not r["ok"]:
        return {"ok": False, "error": "cannot read test.sh"}
    if "found" in r["stdout"]:
        return {"ok": True}
    # 尝试在 "# start manager" 注释行前插入自启动命令
    r = _adb(["shell", f"sed -i '/^#.*start manager/i\\\\n# Auto-start xwebd\\n/var/upgrade/xwebd -d\\n' {TEST_SH_PATH}"], serial=serial)
    if not r["ok"]:
        # sed 失败，改为追加到文件末尾
        r2 = _adb(["shell", f"echo '/var/upgrade/xwebd -d' >> {TEST_SH_PATH}"], serial=serial)
        if not r2["ok"]:
            return {"ok": False, "error": "cannot modify test.sh"}
    return {"ok": True}


def _remove_xwebd_autostart(serial=None):
    """从设备端 test.sh 中移除 xwebd 自启动配置

    通过 sed 删除 test.sh 中所有包含 "xwebd" 的行。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True} 始终返回成功
    """
    _adb(["shell", f"sed -i '/xwebd/d' {TEST_SH_PATH}"], serial=serial)
    return {"ok": True}
