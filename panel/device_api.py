"""
device_api.py — 设备 API 封装（XwebdAPI 类 + Mock 模式）

本模块封装了与设备端 xwebd HTTP 守护进程通信的 API 客户端，主要功能包括：
1. XwebdAPI 类：封装所有与 xwebd 服务的 HTTP 交互（系统信息、音量、亮度、文件管理、助手管理等）
2. Mock 模式：当设备不可用时，提供模拟数据用于开发和测试
3. 设备存活检测：check_device_alive() 函数用于判断设备是否在线
4. 共享 Mock 数据：为 XwebdAPI 和 SairAPI（assistant_api.py）提供统一的模拟状态

关键概念：
- xwebd：设备端 HTTP 守护进程，端口 8080，提供系统控制 API
- sair：Assistant 的二进制文件名（平台约束不可改名），端口 8081
- MOCK_MODE：全局标志，为 True 时所有 API 调用返回模拟数据
"""

import json
import time
import random
import threading
from urllib.parse import urlparse, parse_qs
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

MOCK_MODE = True

_mock_lock = threading.Lock()

_mock_sair_state = {
    "state": "Idle",
    "version": "assistant-v1.0",
    "realtime_mode": True,
    "aec_enabled": True,
}

_mock_sair_config = {
    "ws_url": "wss://api.tenclass.net/xiaozhi/v1/",
    "ws_token": "test_token_xxx",
    "realtime_mode": True,
    "aec_enabled": True,
    "log_level": "DEBUG",
}

_mock_xwebd_state = {
    "volume": 20,
    "brightness": 80,
    "muted": False,
    "mem_free_kb": 4700,
    "wifi_status": 5,
    "server_connected": True,
    "battery_cap": 85,
    "uptime_s": 0,
    "ip": "192.168.1.100",
    "xwebd_version": "1.0.0",
    "upload_max_mb": 20,
}

_mock_sair_logs = [
    "[10:30:00] [I][SM] Idle -> Connecting",
    "[10:30:01] [I][TLS] handshake 318ms",
    "[10:30:01] [I][WS] connected",
    "[10:30:02] [I][ASR] wakeup detected",
    "[10:30:03] [I][SM] Connecting -> Listening",
    "[10:30:05] [I][PROTO] tts_start",
    "[10:30:05] [I][SM] Listening -> Speaking",
    "[10:30:08] [I][PROTO] tts_end",
    "[10:30:08] [I][SM] Speaking -> Listening",
    "[10:30:10] [I][PROTO] goodbye",
    "[10:30:10] [I][SM] Listening -> Cleaning",
    "[10:30:10] [I][SM] Cleaning -> Idle",
]

_mock_files = [
    {"name": "xwebd", "size": 456789, "is_dir": False, "mtime": 1746596400},
    {"name": "sair", "size": 928468, "is_dir": False, "mtime": 1746596400},
    {"name": "sair_backup", "size": 856000, "is_dir": False, "mtime": 1746502800},
    {"name": "boot_watchdog.sh", "size": 256, "is_dir": False, "mtime": 1746417600},
    {"name": "test.sh", "size": 128, "is_dir": False, "mtime": 1746596400},
    {"name": "logs", "size": 0, "is_dir": True, "mtime": 1746596400},
    {"name": "xwebd.log", "size": 32768, "is_dir": False, "mtime": 1746675300},
    {"name": "xiaozhi.log", "size": 65536, "is_dir": False, "mtime": 1746675300},
    {"name": ".api_token", "size": 16, "is_dir": False, "mtime": 1746417600},
]

_uptime_thread = None
_uptime_running = False


def _start_uptime_counter():
    """启动 Mock 模式下的运行时间计数器

    在后台线程中每秒递增 _mock_xwebd_state["uptime_s"]，
    同时随机模拟内存变化，使 Mock 数据更接近真实设备行为。
    如果计数器已在运行则不重复启动。
    """
    global _uptime_thread, _uptime_running
    if _uptime_thread and _uptime_thread.is_alive():
        return
    _uptime_running = True

    def _tick():
        while _uptime_running:
            with _mock_lock:
                _mock_xwebd_state["uptime_s"] += 1
                _mock_xwebd_state["mem_free_kb"] = random.randint(4500, 5000)
            time.sleep(1)

    _uptime_thread = threading.Thread(target=_tick, daemon=True)
    _uptime_thread.start()


def _stop_uptime_counter():
    """停止 Mock 模式下的运行时间计数器"""
    global _uptime_running
    _uptime_running = False


def _validate_int_range(value, name, min_val, max_val):
    """验证整数值是否在指定范围内

    Args:
        value: 待验证的值（可以是字符串或数字）
        name: 参数名称（用于错误信息）
        min_val: 允许的最小值
        max_val: 允许的最大值

    Returns:
        tuple: (验证后的整数值, 错误信息) 验证成功时错误信息为 None，
               验证失败时整数值为 None
    """
    try:
        v = int(value)
    except (ValueError, TypeError):
        return None, f"{name} must be a number"
    if v < min_val or v > max_val:
        return None, f"{name} must be between {min_val} and {max_val}"
    return v, None


def set_mock_mode(enabled):
    """设置 Mock 模式开关

    Args:
        enabled: True 启用 Mock 模式（使用模拟数据），False 使用真实设备连接
    """
    global MOCK_MODE
    MOCK_MODE = enabled


def check_device_alive(host, port, timeout=3):
    """检测设备是否在线

    根据端口号选择不同的健康检查接口：
    - 端口 8081（sair）：访问 /api/status，检查返回数据中是否包含 "state" 字段
    - 其他端口（xwebd）：访问 /api/system，检查返回数据中是否包含 "cpu" 字段

    Args:
        host: 设备 IP 地址
        port: 设备端口号
        timeout: 连接超时时间（秒），默认 3 秒

    Returns:
        bool: True 表示设备在线，False 表示设备离线或响应异常
    """
    try:
        if port == 8081:
            url = f"http://{host}:{port}/api/status"
            check_key = "state"
        else:
            url = f"http://{host}:{port}/api/system"
            check_key = "cpu"
        req = Request(url, method="GET")
        with urlopen(req, timeout=timeout) as resp:
            if resp.status == 200:
                data = json.loads(resp.read().decode("utf-8"))
                return check_key in data
    except Exception:
        pass
    return False


class XwebdAPI:
    """xwebd 设备端 HTTP 守护进程的 API 客户端

    封装了与 xwebd 服务的所有 HTTP 交互，包括：
    - 系统信息查询（CPU、内存、运行时间等）
    - 设备控制（音量、亮度、静音、重启）
    - 配置管理（读取和修改 xwebd 配置）
    - 文件管理（列表、清理）
    - 助手管理（部署、更新、卸载、状态查询）
    - 日志查询

    支持 Mock 模式：当 MOCK_MODE 为 True 时，所有请求返回模拟数据。
    """

    def __init__(self, host="192.168.1.96", port=8080):
        """初始化 XwebdAPI 客户端

        Args:
            host: xwebd 服务地址，默认 "192.168.1.96"
            port: xwebd 服务端口，默认 8080
        """
        self.host = host
        self.port = port
        self.base_url = f"http://{host}:{port}"
        self.connected = False

    def _request(self, method, path, data=None):
        """发送 HTTP 请求到 xwebd 服务

        Mock 模式下调用 _mock_handler 返回模拟数据，
        LIVE 模式下通过 urllib 发送真实 HTTP 请求。

        Args:
            method: HTTP 方法（GET/POST/PUT/DELETE）
            path: API 路径（如 /api/system）
            data: 请求体数据（dict），为 None 时不发送请求体

        Returns:
            dict: API 响应数据，请求失败时返回 {"error": "..."}
        """
        if MOCK_MODE:
            return self._mock_handler(method, path, data)
        url = f"{self.base_url}{path}"
        body = json.dumps(data).encode("utf-8") if data is not None else None
        headers = {"Content-Type": "application/json"}
        req = Request(url, data=body, method=method, headers=headers)
        try:
            with urlopen(req, timeout=5) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as e:
            return {"error": str(e)}

    def _mock_handler(self, method, path, data):
        """Mock 请求处理器（线程安全入口）

        获取锁后调用 _mock_handler_locked，确保 Mock 状态的线程安全。

        Args:
            method: HTTP 方法
            path: API 路径
            data: 请求体数据

        Returns:
            dict: 模拟的 API 响应数据
        """
        with _mock_lock:
            return self._mock_handler_locked(method, path, data)

    def _mock_handler_locked(self, method, path, data):
        """Mock 请求处理器（实际逻辑，需在锁内调用）

        根据 HTTP 方法和路径返回对应的模拟数据，
        并对写入操作更新 Mock 状态。

        Args:
            method: HTTP 方法
            path: API 路径
            data: 请求体数据

        Returns:
            dict: 模拟的 API 响应数据
        """
        if method == "GET" and path == "/api/system":
            return {"cpu": 12.5, "mem_free_kb": _mock_xwebd_state["mem_free_kb"],
                    "muted": _mock_xwebd_state["muted"], "uptime_s": _mock_xwebd_state["uptime_s"]}
        elif method == "GET" and path == "/api/status":
            return dict(_mock_xwebd_state)
        elif method == "GET" and path == "/api/volume":
            return {"volume": _mock_xwebd_state["volume"]}
        elif method == "GET" and path == "/api/brightness":
            return {"brightness": _mock_xwebd_state["brightness"]}
        elif method == "GET" and path == "/api/mute":
            return {"muted": _mock_xwebd_state["muted"]}
        elif method == "GET" and path.startswith("/api/logs"):
            parsed = urlparse(path)
            qs = parse_qs(parsed.query)
            lines = int(qs.get("lines", [100])[0])
            lines = max(1, min(lines, 100))
            return {"logs": _mock_sair_logs[:lines]}
        elif method == "GET" and path == "/api/config":
            return {"upload_max_mb": _mock_xwebd_state["upload_max_mb"]}
        elif method == "PUT" and path == "/api/config":
            if data and "upload_max_mb" in data:
                v, err = _validate_int_range(data["upload_max_mb"], "upload_max_mb", 1, 100)
                if err:
                    return {"error": err}
                _mock_xwebd_state["upload_max_mb"] = v
            return {"ok": True}
        elif method == "POST" and path == "/api/volume":
            if data and "volume" in data:
                v, err = _validate_int_range(data["volume"], "volume", 0, 80)
                if err:
                    return {"error": err}
                _mock_xwebd_state["volume"] = v
            return {"ok": True}
        elif method == "POST" and path == "/api/brightness":
            if data and "brightness" in data:
                v, err = _validate_int_range(data["brightness"], "brightness", 0, 900)
                if err:
                    return {"error": err}
                _mock_xwebd_state["brightness"] = v
            return {"ok": True}
        elif method == "POST" and path == "/api/mute":
            if data and "muted" in data:
                _mock_xwebd_state["muted"] = bool(data["muted"])
            return {"ok": True}
        elif method == "POST" and path == "/api/reboot":
            # 模拟重启：重置运行时间
            _mock_xwebd_state["uptime_s"] = 0
            return {"ok": True}
        elif method == "GET" and path.startswith("/api/files"):
            return {"path": "/var/upgrade", "files": list(_mock_files)}
        elif method == "POST" and path == "/api/files/cleanup":
            # 模拟清理：清空日志文件、删除临时文件和 .api_token
            freed_bytes = 0
            removed_files = 0
            for f in _mock_files[:]:
                if f["name"] in ("xwebd.log",):
                    freed_bytes += f["size"]
                    removed_files += 1
                    f["size"] = 0
                elif f["name"] in (".api_token",) or f["name"].endswith(".tmp"):
                    freed_bytes += f["size"]
                    removed_files += 1
                    _mock_files.remove(f)
            return {"freed_bytes": freed_bytes, "removed_files": removed_files}
        elif method == "POST" and path == "/api/assistant/deploy":
            return {"ok": True, "message": "assistant deployed"}
        elif method == "POST" and path == "/api/assistant/update":
            return {"ok": True, "message": "assistant updated"}
        elif method == "POST" and path == "/api/assistant/uninstall":
            # 模拟卸载：将助手状态改为 Starting
            _mock_sair_state["state"] = "Starting"
            return {"ok": True, "message": "assistant uninstalled"}
        elif method == "GET" and path == "/api/assistant/status":
            return {"installed": True, "version": _mock_sair_state["version"], "running": True}
        elif method == "POST" and path == "/api/poweroff":
            return {"ok": True}
        elif method == "GET" and path == "/api/services":
            return {"telnet": {"running": False}, "boot_watchdog": {"deployed": True}, "xwebd_autostart": {"enabled": True}}
        elif method == "GET" and path == "/api/version":
            return {"version": "1.0.0"}
        return {"error": f"Unknown endpoint: {method} {path}"}

    def get_status(self):
        """获取 xwebd 系统状态（CPU、内存、静音、运行时间）

        Returns:
            dict: 系统状态信息
        """
        return self._request("GET", "/api/system")

    def get_logs(self, lines=100):
        """获取设备日志

        Args:
            lines: 返回的日志行数，默认 100

        Returns:
            dict: {"logs": [...]} 日志内容
        """
        return self._request("GET", f"/api/logs?lines={lines}")

    def get_config(self):
        """获取 xwebd 配置

        Returns:
            dict: 配置信息（如 upload_max_mb 等）
        """
        return self._request("GET", "/api/config")

    def set_config(self, config):
        """更新 xwebd 配置

        Args:
            config: 要更新的配置字段字典（如 {"upload_max_mb": 30}）

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("PUT", "/api/config", config)

    def set_volume(self, volume):
        """设置设备音量

        Args:
            volume: 音量值（0-80）

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/volume", {"volume": volume})

    def set_brightness(self, brightness):
        """设置设备亮度

        Args:
            brightness: 亮度值（0-900）

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/brightness", {"brightness": brightness})

    def set_mute(self, muted):
        """设置设备静音状态

        Args:
            muted: True 静音，False 取消静音

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/mute", {"muted": muted})

    def reboot(self):
        """重启设备

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/reboot", {})

    def list_files(self, path="/var/upgrade"):
        """列出设备上的文件

        Args:
            path: 要列出的目录路径，默认 "/var/upgrade"

        Returns:
            dict: {"path": "...", "files": [...]} 文件列表
        """
        return self._request("GET", f"/api/files?path={path}")

    def cleanup(self):
        """清理设备上的临时文件（日志、临时文件、.api_token 等）

        Returns:
            dict: {"freed_bytes": ..., "removed_files": ...} 清理结果
        """
        return self._request("POST", "/api/files/cleanup", {})

    def deploy_assistant(self, path="/var/upgrade/sair_new"):
        """部署助手到设备

        Args:
            path: sair 新版本文件路径，默认 "/var/upgrade/sair_new"
                  （sair 是 Assistant 的二进制文件名，平台约束不可改名）

        Returns:
            dict: {"ok": True, "message": "..."} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/assistant/deploy", {"path": path})

    def update_assistant(self, path="/var/upgrade/sair_new"):
        """更新设备上的助手

        Args:
            path: sair 新版本文件路径，默认 "/var/upgrade/sair_new"
                  （sair 是 Assistant 的二进制文件名，平台约束不可改名）

        Returns:
            dict: {"ok": True, "message": "..."} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/assistant/update", {"path": path})

    def uninstall_assistant(self):
        """卸载设备上的助手

        Returns:
            dict: {"ok": True, "message": "..."} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/assistant/uninstall", {})

    def assistant_status(self):
        """获取助手安装和运行状态

        Returns:
            dict: {"installed": True/False, "version": "...", "running": True/False}
        """
        return self._request("GET", "/api/assistant/status")

    def poweroff(self):
        return self._request("POST", "/api/poweroff")

    def get_services(self):
        return self._request("GET", "/api/services")

    def get_version(self):
        return self._request("GET", "/api/version")

    def check_connection(self):
        """检查与 xwebd 服务的连接状态

        Mock 模式下直接返回 True。
        LIVE 模式下请求 /api/system，检查返回数据中是否包含 "cpu" 字段。

        Returns:
            bool: True 连接成功，False 连接失败
        """
        if MOCK_MODE:
            self.connected = True
            return True
        try:
            result = self._request("GET", "/api/system")
            self.connected = "cpu" in result
            return self.connected
        except Exception:
            self.connected = False
            return False


from assistant_api import SairAPI
