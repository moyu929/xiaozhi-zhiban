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
import logging
import sys

logger = logging.getLogger("panel.adb_manager")

ADB_TIMEOUT = 5

SAIR_REMOTE_PATH = "/var/upgrade/sair"
SAIR_LOG_PATH = "/var/upgrade/xiaozhi.log"
XWEBD_REMOTE_PATH = "/var/upgrade/xwebd"
XWEBD_LOG_PATH = "/var/upgrade/xwebd.log"
TEST_SH_PATH = "/var/upgrade/test.sh"
BOOT_WATCHDOG_PATH = "/var/upgrade/boot_watchdog.sh"
WATCHDOG_GUARD_PATH = "/var/upgrade/watchdog_guard.sh"

_ADB_PATH = None


def _find_adb():
    global _ADB_PATH
    if _ADB_PATH is not None:
        return _ADB_PATH
    candidates = []
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    candidates.append(os.path.join(project_root, "platform-tools", "adb.exe"))
    candidates.append(os.path.join(project_root, "platform-tools", "adb"))
    panel_dir = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(panel_dir, "..", "..", "platform-tools", "adb.exe"))
    candidates.append(os.path.join(panel_dir, "..", "..", "platform-tools", "adb"))
    for path in candidates:
        norm = os.path.normpath(path)
        logger.info("检查ADB路径: %s (exists=%s)", norm, os.path.isfile(norm))
        if os.path.isfile(norm):
            _ADB_PATH = norm
            logger.info("ADB路径: %s", _ADB_PATH)
            return _ADB_PATH
    which_result = shutil.which("adb")
    logger.info("shutil.which('adb') = %s", which_result)
    if which_result:
        _ADB_PATH = which_result
        logger.info("ADB路径(PATH): %s", _ADB_PATH)
        return _ADB_PATH
    _ADB_PATH = "adb"
    logger.warning("ADB未找到，使用默认'adb'")
    return _ADB_PATH


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
    cmd = [_find_adb()]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    logger.debug("ADB: %s", " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          encoding='utf-8', errors='replace')
        if r.returncode != 0:
            logger.debug("ADB失败(%d): %s %s", r.returncode, " ".join(args), r.stderr[:100] if r.stderr else "")
        return {"ok": r.returncode == 0, "stdout": (r.stdout or "").strip(), "stderr": (r.stderr or "").strip(), "returncode": r.returncode}
    except subprocess.TimeoutExpired:
        logger.warning("ADB命令超时: %s", " ".join(args))
        return {"ok": False, "stdout": "", "stderr": "ADB command timed out", "returncode": -1}
    except FileNotFoundError:
        logger.warning("ADB未安装")
        return {"ok": False, "stdout": "", "stderr": "ADB not found. Install Android SDK Platform Tools.", "returncode": -1}


def _find_file(candidates):
    for path in candidates:
        norm = os.path.normpath(path)
        if os.path.isfile(norm):
            return norm
    return None


def _push_binary(local_path, remote_path, serial, timeout=30):
    max_retries = 3
    for attempt in range(max_retries):
        r = _adb(["push", local_path, remote_path], serial=serial, timeout=timeout)
        if r["ok"]:
            r = _adb(["shell", f"chmod 755 {remote_path}"], serial=serial)
            if r["ok"]:
                return {"ok": True}
            return {"ok": False, "error": f"chmod failed: {r['stderr']}"}
        if attempt < max_retries - 1:
            logger.warning("adb push失败(第%d次), 1秒后重试: %s", attempt + 1, r['stderr'])
            time.sleep(1)
    return {"ok": False, "error": f"adb push failed after {max_retries} retries: {r['stderr']}"}


def _shell_test(path, serial):
    r = _adb(["shell", f"test -f {path} && echo yes || echo no"], serial=serial)
    return r["ok"] and "yes" in r["stdout"]


def is_adb_available():
    adb_path = _find_adb()
    if adb_path == "adb":
        result = shutil.which("adb") is not None
        logger.info("is_adb_available: path='adb', which=%s", result)
        return result
    result = os.path.isfile(adb_path)
    logger.info("is_adb_available: path='%s', exists=%s", adb_path, result)
    if result:
        try:
            r = subprocess.run([adb_path, "version"], capture_output=True, text=True, timeout=5)
            logger.info("adb version rc=%d", r.returncode)
            return r.returncode == 0
        except Exception as e:
            logger.warning("adb version failed: %s", e)
            return False
    return False


def connect_adb_wifi(ip, port=5555):
    """通过 WiFi 连接 ADB 设备

    先尝试 adb connect，然后验证设备是否出现在 devices 列表中。

    Args:
        ip: 设备 IP 地址
        port: ADB 端口，默认 5555

    Returns:
        dict: {"ok": True, "serial": "..."} 连接成功
              {"ok": False, "error": "..."} 连接失败
    """
    target = f"{ip}:{port}"
    logger.info("无线ADB连接: %s", target)
    r = _adb(["connect", target], timeout=10)
    if not r["ok"]:
        return {"ok": False, "error": f"adb connect failed: {r['stderr']}"}
    time.sleep(1)
    devices = detect_devices()
    for d in devices:
        if d["serial"] == target or d["serial"].startswith(ip):
            logger.info("无线ADB连接成功: %s", d["serial"])
            return {"ok": True, "serial": d["serial"]}
    connected_serials = [d["serial"] for d in devices]
    logger.warning("无线ADB连接后未找到设备, 已连接设备: %s", connected_serials)
    return {"ok": False, "error": f"adb connect sent but device not found in list. Connected: {connected_serials}"}


def ensure_adb_connection(ip=None, serial=None):
    """确保 ADB 连接可用

    如果指定了 IP 但没有 USB 连接的设备，尝试通过 WiFi 连接。
    如果已有设备连接则直接返回。

    Args:
        ip: 设备 IP 地址（用于无线 ADB 连接）
        serial: 指定的设备序列号

    Returns:
        dict: {"ok": True, "serial": "..."} 连接可用
              {"ok": False, "error": "..."} 无可用连接
    """
    devices = detect_devices()
    if serial:
        for d in devices:
            if d["serial"] == serial:
                return {"ok": True, "serial": serial}
    if devices:
        return {"ok": True, "serial": devices[0]["serial"]}
    if ip:
        return connect_adb_wifi(ip)
    return {"ok": False, "error": "未检测到ADB设备，请通过USB连接设备或输入设备IP"}


def detect_devices():
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
        info = {"serial": serial, "state": state}
        for p in parts[2:]:
            if p.startswith("model:"):
                info["model"] = p.split(":", 1)[1]
            elif p.startswith("device:"):
                info["device"] = p.split(":", 1)[1]
        devices.append(info)
    logger.info("检测ADB设备: 发现 %d 台", len(devices))
    return devices


def is_xwebd_installed(serial=None):
    """检查设备上是否已安装 xwebd

    通过 `adb shell test -f` 检查 xwebd 二进制文件是否存在。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        bool: True 表示已安装，False 表示未安装或检查失败
    """
    return _shell_test(XWEBD_REMOTE_PATH, serial)


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
    return _find_file([
        os.path.join(base_dir, "..", "device", "xwebd", "prebuilt", "xwebd"),
        os.path.join(base_dir, "..", "device", "xwebd", "build", "xwebd"),
        os.path.join(base_dir, "device", "prebuilt", "xwebd"),
        os.path.join(base_dir, "device", "build", "xwebd"),
    ])


def _find_watchdog_script():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    return _find_file([
        os.path.join(base_dir, "..", "device", "xwebd", "scripts", "boot_watchdog.sh"),
        os.path.join(base_dir, "device", "scripts", "boot_watchdog.sh"),
    ])


def _find_watchdog_guard_script():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    return _find_file([
        os.path.join(base_dir, "..", "device", "xwebd", "scripts", "watchdog_guard.sh"),
    ])


def is_device_initialized(serial=None):
    return {
        "initialized": _shell_test(TEST_SH_PATH, serial),
        "test_sh_exists": _shell_test(TEST_SH_PATH, serial),
        "boot_watchdog_exists": _shell_test(BOOT_WATCHDOG_PATH, serial),
    }


def init_device(serial=None):
    result = {"ok": True, "error": "", "created_test_sh": False, "pushed_watchdog": False, "pushed_guard": False}

    if not _shell_test(TEST_SH_PATH, serial):
        test_sh_content = (
            '#!/bin/sh\n'
            '# 小智·智伴 开机自启脚本\n'
            '# 由Panel控制面板自动创建\n'
            '\n'
            '# Telnet服务（远程调试）\n'
            'busybox telnetd -p 23 -l /bin/sh\n'
            '\n'
            '# USB ADB功能\n'
            'sleep 2\n'
            'if [ -w /sys/class/android_usb/android0/functions ]; then\n'
            '    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null\n'
            '    echo adb > /sys/class/android_usb/android0/functions\n'
            '    echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null\n'
            'fi\n'
            '\n'
            '# 开机频率检测与自动回退\n'
            '/var/upgrade/boot_watchdog.sh\n'
        )
        escaped = test_sh_content.replace("'", "'\\''")
        r = _adb(["shell", f"printf '%s' '{escaped}' > {TEST_SH_PATH}"], serial=serial, timeout=10)
        if not r["ok"]:
            result["ok"] = False
            result["error"] = f"create test.sh failed: {r['stderr']}"
            return result
        _adb(["shell", f"chmod 755 {TEST_SH_PATH}"], serial=serial)
        result["created_test_sh"] = True
        logger.info("test.sh已创建: serial=%s", serial)

    if not _shell_test(BOOT_WATCHDOG_PATH, serial):
        watchdog_script = _find_watchdog_script()
        if watchdog_script:
            r = _adb(["push", watchdog_script, BOOT_WATCHDOG_PATH], serial=serial, timeout=10)
            if r["ok"]:
                _adb(["shell", f"chmod 755 {BOOT_WATCHDOG_PATH}"], serial=serial)
                result["pushed_watchdog"] = True
                logger.info("boot_watchdog.sh已推送: serial=%s", serial)

    if not _shell_test(WATCHDOG_GUARD_PATH, serial):
        guard_script = _find_watchdog_guard_script()
        if guard_script:
            r = _adb(["push", guard_script, WATCHDOG_GUARD_PATH], serial=serial, timeout=10)
            if r["ok"]:
                _adb(["shell", f"chmod 755 {WATCHDOG_GUARD_PATH}"], serial=serial)
                result["pushed_guard"] = True
                logger.info("watchdog_guard.sh已推送: serial=%s", serial)

    return result


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
            logger.error("xwebd部署失败: xwebd binary not found (no prebuilt or build)")
            return {"ok": False, "error": "xwebd binary not found (no prebuilt or build)"}

    if not os.path.isfile(binary_path):
        logger.error("xwebd部署失败: Binary not found: %s", binary_path)
        return {"ok": False, "error": f"Binary not found: {binary_path}"}

    logger.info("部署xwebd: serial=%s, binary=%s", serial, binary_path)

    init_result = init_device(serial)
    if not init_result["ok"]:
        logger.warning("设备初始化失败(继续部署): %s", init_result["error"])

    r = _push_binary(binary_path, XWEBD_REMOTE_PATH, serial)
    if not r["ok"]:
        logger.error("xwebd部署失败: %s", r['error'])
        return {"ok": False, "error": r['error']}

    if not init_result["pushed_watchdog"]:
        watchdog_script = _find_watchdog_script()
        if watchdog_script:
            logger.debug("推送看门狗脚本: %s", watchdog_script)
            r = _adb(["push", watchdog_script, BOOT_WATCHDOG_PATH], serial=serial, timeout=10)
            if r["ok"]:
                _adb(["shell", f"chmod 755 {BOOT_WATCHDOG_PATH}"], serial=serial)

    logger.debug("配置自启动")
    r = _ensure_xwebd_autostart(serial)
    if not r["ok"]:
        logger.error("xwebd部署失败: autostart config failed: %s", r['error'])
        return {"ok": False, "error": f"autostart config failed: {r['error']}"}

    r = start_xwebd(serial)
    if not r["ok"]:
        logger.error("xwebd部署失败: start xwebd failed: %s", r['error'])
        return {"ok": False, "error": f"start xwebd failed: {r['error']}"}

    logger.info("xwebd部署完成: serial=%s", serial)
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
            logger.error("xwebd更新失败: xwebd binary not found")
            return {"ok": False, "error": "xwebd binary not found"}

    if not os.path.isfile(binary_path):
        logger.error("xwebd更新失败: Binary not found: %s", binary_path)
        return {"ok": False, "error": f"Binary not found: {binary_path}"}

    logger.info("更新xwebd: serial=%s, binary=%s", serial, binary_path)

    stop_xwebd(serial)
    time.sleep(1)

    r = _push_binary(binary_path, XWEBD_REMOTE_PATH, serial)
    if not r["ok"]:
        logger.error("xwebd更新失败: %s", r['error'])
        return {"ok": False, "error": r['error']}

    r = start_xwebd(serial)
    if not r["ok"]:
        logger.error("xwebd更新失败: start xwebd failed: %s", r['error'])
        return {"ok": False, "error": f"start xwebd failed: {r['error']}"}

    logger.info("xwebd更新完成: serial=%s", serial)
    return {"ok": True, "binary": binary_path}


def remove_xwebd(serial=None):
    logger.info("移除xwebd: serial=%s", serial)
    stop_xwebd(serial)
    time.sleep(0.5)

    _adb(["shell", f"rm -f {XWEBD_REMOTE_PATH}"], serial=serial)
    _adb(["shell", f"rm -f {XWEBD_LOG_PATH}"], serial=serial)
    _remove_xwebd_autostart(serial)

    return {"ok": True, "error": ""}


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
    logger.info("启动xwebd: serial=%s", serial)
    r = _adb(["shell", f"{XWEBD_REMOTE_PATH} -d"], serial=serial, timeout=10)
    if not r["ok"]:
        logger.error("xwebd启动失败: %s", r['stderr'])
        return {"ok": False, "error": f"start failed: {r['stderr']}"}
    time.sleep(1)
    status = check_xwebd_status(serial)
    if not status["running"]:
        logger.error("xwebd启动失败: xwebd started but pidof check failed")
        return {"ok": False, "error": "xwebd started but pidof check failed"}
    logger.info("xwebd已启动: pid=%s", status["pid"])
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
    logger.info("停止xwebd: serial=%s", serial)
    _adb(["shell", "killall xwebd 2>/dev/null"], serial=serial)
    time.sleep(0.5)
    status = check_xwebd_status(serial)
    if status["running"]:
        logger.warning("xwebd未响应killall, 使用kill -9")
        _adb(["shell", "killall -9 xwebd 2>/dev/null"], serial=serial)
        time.sleep(0.5)
    return {"ok": True, "error": ""}


def restart_xwebd(serial=None):
    """重启设备上的 xwebd 守护进程

    先停止再启动。

    Args:
        serial: 设备序列号，为 None 时不指定设备

    Returns:
        dict: {"ok": True, "pid": "..."} 重启成功
              {"ok": False, "error": "..."} 重启失败
    """
    logger.info("重启xwebd: serial=%s", serial)
    stop_xwebd(serial)
    return start_xwebd(serial)


def setup_forward(serial=None, local_xwebd=8080):
    """设置 ADB 端口转发

    将本地 TCP 端口映射到设备端口，使本机可以通过 localhost 访问设备服务：
    - 本地 local_xwebd → 设备 8080（xwebd 服务）

    Args:
        serial: 设备序列号，为 None 时不指定设备
        local_xwebd: 本地 xwebd 转发端口，默认 8080

    Returns:
        dict: {"ok": bool, "errors": list} 转发结果和错误信息列表
    """
    logger.info("设置ADB端口转发: serial=%s, xwebd=%d", serial, local_xwebd)
    r1 = _adb(["forward", f"tcp:{local_xwebd}", f"tcp:8080"], serial=serial)
    ok = r1["ok"]
    errors = []
    if not r1["ok"]:
        errors.append(f"xwebd forward: {r1['stderr']}")
    if ok:
        logger.info("ADB端口转发设置成功")
    else:
        logger.warning("ADB端口转发失败: %s", errors)
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
        ip = r["stdout"].strip()
        logger.debug("设备IP: %s", ip)
        return ip
    logger.debug("获取设备IP失败")
    return None


def _ensure_xwebd_autostart(serial=None):
    if not _shell_test(TEST_SH_PATH, serial):
        init_result = init_device(serial)
        if not init_result["ok"] and not init_result["created_test_sh"]:
            return {"ok": False, "error": "test.sh not found and init_device failed"}

    r = _adb(["shell", f"grep -q xwebd {TEST_SH_PATH} && echo found || echo missing"], serial=serial)
    if not r["ok"]:
        return {"ok": False, "error": "cannot read test.sh"}
    if "found" in r["stdout"]:
        return {"ok": True, "error": ""}
    logger.info("添加xwebd自启动到test.sh")
    r = _adb(["shell", f"echo '/var/upgrade/xwebd -d' >> {TEST_SH_PATH}"], serial=serial)
    if not r["ok"]:
        return {"ok": False, "error": "cannot modify test.sh"}
    return {"ok": True, "error": ""}


def _remove_xwebd_autostart(serial=None):
    logger.info("移除xwebd自启动")
    _adb(["shell", f"sed -i '/xwebd/d' {TEST_SH_PATH}"], serial=serial)
    return {"ok": True, "error": ""}


_INFO_SECTION_DELIMITER = "---SECTION---"

_INFO_SECTIONS = [
    ("cpu_info", "cat /proc/cpuinfo | grep -iE 'Hardware|model name' | tail -2"),
    ("kernel", "uname -r"),
    ("uptime", "cat /proc/uptime"),
    ("meminfo", "cat /proc/meminfo | head -3"),
    ("network", "ifconfig wlan0 2>/dev/null"),
    ("disk", "df -k /var/upgrade 2>/dev/null | tail -1"),
    ("model", "cat /proc/device-tree/model 2>/dev/null || echo unknown"),
]


def _parse_cpu_info(text):
    hardware = ""
    processor = ""
    for line in text.strip().split("\n"):
        line = line.strip()
        lower = line.lower()
        if lower.startswith("hardware"):
            hardware = line.split(":", 1)[1].strip() if ":" in line else ""
        elif lower.startswith("model name"):
            processor = line.split(":", 1)[1].strip() if ":" in line else ""
    return {"cpu": processor or hardware or "unknown", "model": hardware or processor or "unknown"}


def _parse_kernel(text):
    return {"kernel": text.strip() or "unknown"}


def _parse_uptime(text):
    try:
        first_val = text.strip().split()[0]
        return {"uptime_s": float(first_val)}
    except (ValueError, IndexError):
        return {"uptime_s": 0}


def _parse_meminfo(text):
    result = {}
    for line in text.strip().split("\n"):
        parts = line.split()
        if len(parts) >= 2:
            key = parts[0].rstrip(":")
            try:
                val = int(parts[1])
            except ValueError:
                continue
            if key == "MemTotal":
                result["mem_total_kb"] = val
            elif key == "MemFree":
                result["mem_free_kb"] = val
            elif key == "Cached":
                result["mem_cached_kb"] = val
    return result


def _parse_network(text):
    ifconfig_out = text.strip()
    if not ifconfig_out:
        return {"wifi_ip": None, "wifi_connected": False}
    wifi_ip = None
    interface_up = False
    if "inet addr:" in ifconfig_out:
        wifi_ip = ifconfig_out.split("inet addr:")[1].split()[0]
    elif "inet " in ifconfig_out:
        for line in ifconfig_out.split("\n"):
            stripped = line.strip()
            if stripped.startswith("inet "):
                parts = stripped.split()
                for i, p in enumerate(parts):
                    if p == "inet" and i + 1 < len(parts):
                        wifi_ip = parts[i + 1]
                        break
                break
    for line in ifconfig_out.split("\n"):
        if "UP" in line and ("BROADCAST" in line or "RUNNING" in line or "MULTICAST" in line):
            interface_up = True
            break
    return {"wifi_ip": wifi_ip, "wifi_connected": interface_up and bool(wifi_ip)}


def _parse_disk(text):
    result = {}
    disk_parts = text.strip().split()
    if len(disk_parts) >= 4:
        try:
            result["disk_total_kb"] = int(disk_parts[1])
            result["disk_used_kb"] = int(disk_parts[2])
            result["disk_free_kb"] = int(disk_parts[3])
        except (ValueError, IndexError):
            pass
    return result


def _parse_model(text):
    prop_model = text.strip().rstrip('\x00')
    if prop_model and prop_model != "unknown":
        return {"model": prop_model}
    return {}


_INFO_PARSERS = {
    "cpu_info": _parse_cpu_info,
    "kernel": _parse_kernel,
    "uptime": _parse_uptime,
    "meminfo": _parse_meminfo,
    "network": _parse_network,
    "disk": _parse_disk,
    "model": _parse_model,
}


def get_device_info(serial=None):
    commands = [cmd for _, cmd in _INFO_SECTIONS]
    cmd = f"; echo '{_INFO_SECTION_DELIMITER}'; ".join(commands)
    r = _adb(["shell", cmd], serial=serial, timeout=10)
    if not r["ok"]:
        return {"ok": False, "error": r["stderr"]}
    raw_sections = r["stdout"].split(_INFO_SECTION_DELIMITER)
    info = {"ok": True}
    section_names = [name for name, _ in _INFO_SECTIONS]
    for i, section_text in enumerate(raw_sections):
        if i >= len(section_names):
            break
        name = section_names[i]
        parser = _INFO_PARSERS.get(name)
        if parser:
            info.update(parser(section_text))
    return info


def check_sair_status(serial=None):
    custom_installed = _shell_test(SAIR_REMOTE_PATH, serial)
    version = None
    if custom_installed:
        rv = _adb(["shell", f"stat -c '%Y' {SAIR_REMOTE_PATH} 2>/dev/null"], serial=serial)
        if rv["ok"] and rv["stdout"].strip():
            try:
                ts = int(rv["stdout"].strip().strip("'"))
                version = time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))
            except (ValueError, OSError):
                version = None
    pid = None
    custom_running = False
    native_running = False
    r2 = _adb(["shell", "pidof sair"], serial=serial)
    if r2["ok"] and r2["stdout"].strip():
        pid = r2["stdout"].strip().split()[0]
        r3 = _adb(["shell", f"ls -l /proc/{pid}/exe 2>/dev/null"], serial=serial)
        exe_path = ""
        if r3["ok"] and "->" in r3["stdout"]:
            exe_path = r3["stdout"].split("->")[-1].strip()
        if "/var/upgrade/sair" in exe_path:
            custom_running = True
        elif "/usr/bin/sair" in exe_path:
            native_running = True
        elif exe_path:
            custom_running = custom_installed
    return {
        "custom_installed": custom_installed,
        "custom_running": custom_running,
        "native_running": native_running,
        "installed": custom_installed,
        "running": custom_running,
        "pid": pid,
        "version": version,
    }


def deploy_sair(serial=None, binary_path=None):
    if binary_path is None:
        binary_path = _find_sair_binary()
        if not binary_path:
            return {"ok": False, "error": "sair binary not found"}
    if not os.path.isfile(binary_path):
        return {"ok": False, "error": f"Binary not found: {binary_path}"}
    logger.info("冷部署sair: serial=%s, binary=%s", serial, binary_path)
    r = _push_binary(binary_path, SAIR_REMOTE_PATH, serial)
    if not r["ok"]:
        return {"ok": False, "error": r['error']}
    _adb(["shell", "reboot"], serial=serial, timeout=5)
    logger.info("sair冷部署完成，设备重启中: serial=%s", serial)
    return {"ok": True, "binary": binary_path, "mode": "cold", "rebooting": True}


def hot_update_sair(serial=None, binary_path=None):
    if binary_path is None:
        binary_path = _find_sair_binary()
        if not binary_path:
            return {"ok": False, "error": "sair binary not found"}
    if not os.path.isfile(binary_path):
        return {"ok": False, "error": f"Binary not found: {binary_path}"}
    logger.info("热更新sair: serial=%s", serial)
    r = _push_binary(binary_path, SAIR_REMOTE_PATH + "_new", serial)
    if not r["ok"]:
        return {"ok": False, "error": r['error']}
    r = _adb(["shell", "kill -USR2 $(pidof sair)"], serial=serial, timeout=10)
    if not r["ok"]:
        return {"ok": False, "error": f"SIGUSR2 failed: {r['stderr']}"}
    logger.info("sair热更新已触发: serial=%s", serial)
    return {"ok": True, "binary": binary_path, "mode": "hot"}


def _find_sair_binary():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    return _find_file([
        os.path.join(base_dir, "..", "device", "assistant", "build", "sair"),
        os.path.join(base_dir, "device", "build", "sair"),
        os.path.join(base_dir, "..", "device", "assistant", "sair"),
        os.path.join(base_dir, "device", "assistant", "sair"),
    ])


def reboot_device(serial=None):
    logger.warning("通过ADB重启设备: serial=%s", serial)
    r = _adb(["shell", "reboot"], serial=serial, timeout=10)
    return {"ok": r["ok"], "error": r["stderr"] if not r["ok"] else ""}


def poweroff_device(serial=None):
    logger.warning("通过ADB关机: serial=%s", serial)
    r = _adb(["shell", "poweroff"], serial=serial, timeout=10)
    if not r["ok"]:
        r = _adb(["shell", "echo o > /proc/sysrq-trigger"], serial=serial, timeout=10)
    return {"ok": r["ok"], "error": r["stderr"] if not r["ok"] else ""}


def get_device_logs(serial=None, log_type="all", lines=100):
    result = {}
    if log_type in ("all", "sair"):
        r = _adb(["shell", f"tail -n {lines} {SAIR_LOG_PATH} 2>/dev/null || echo '[log file not found]'"], serial=serial, timeout=10)
        result["sair"] = r["stdout"].split("\n") if r["ok"] else []
    if log_type in ("all", "xwebd"):
        r = _adb(["shell", f"tail -n {lines} {XWEBD_LOG_PATH} 2>/dev/null || echo '[log file not found]'"], serial=serial, timeout=10)
        result["xwebd"] = r["stdout"].split("\n") if r["ok"] else []
    return {"ok": True, "logs": result}
