"""
assistant_api.py — Assistant API 封装（SairAPI 类 + Mock 模式）

本模块封装了与设备端 sair（Assistant）服务通信的 API 客户端，主要功能包括：
1. SairAPI 类：封装所有与 sair 服务的 HTTP 交互（状态查询、配置管理、唤醒、中止、升级等）
2. Mock 模式：当设备不可用时，使用 device_api.py 中的共享 Mock 数据返回模拟响应

关键说明：
- sair 是 Assistant 的二进制文件名（平台约束不可改名），运行在设备端端口 8081
- sair 提供语音助手相关 API：状态机控制（唤醒/中止）、WebSocket 配置、升级等
- Mock 数据与 device_api.py 共享，通过 _mock_lock 保证线程安全
"""

import json
import threading
from urllib.request import Request, urlopen

from device_api import MOCK_MODE, _mock_lock, _mock_sair_state, _mock_sair_config, _start_uptime_counter


class SairAPI:
    """sair（Assistant）服务的 API 客户端

    封装了与 sair 服务的所有 HTTP 交互，包括：
    - 助手状态查询（当前状态机状态、版本号等）
    - 配置管理（WebSocket 地址/令牌、实时模式、AEC、日志级别等）
    - 唤醒助手（触发语音识别）
    - 中止当前对话
    - 触发助手升级

    支持 Mock 模式：当 MOCK_MODE 为 True 时，所有请求返回模拟数据。
    初始化时如果处于 Mock 模式，会自动启动运行时间计数器。
    """

    def __init__(self, host="192.168.1.96", port=8081):
        """初始化 SairAPI 客户端

        Args:
            host: sair 服务地址，默认 "192.168.1.96"
            port: sair 服务端口，默认 8081
        """
        self.host = host
        self.port = port
        self.base_url = f"http://{host}:{port}"
        self.connected = False
        if MOCK_MODE:
            # Mock 模式下启动运行时间计数器，模拟设备运行
            _start_uptime_counter()

    def _request(self, method, path, data=None):
        """发送 HTTP 请求到 sair 服务

        Mock 模式下调用 _mock_handler 返回模拟数据，
        LIVE 模式下通过 urllib 发送真实 HTTP 请求。

        Args:
            method: HTTP 方法（GET/POST/PUT）
            path: API 路径（如 /api/status）
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
            return {"cpu": 12.5, "mem_free_kb": 4700}
        elif method == "GET" and path == "/api/status":
            return dict(_mock_sair_state)
        elif method == "GET" and path == "/api/config":
            return dict(_mock_sair_config)
        elif method == "PUT" and path == "/api/config":
            # 模拟更新配置：直接合并到 Mock 配置字典
            if data:
                _mock_sair_config.update(data)
            return {"ok": True}
        elif method == "POST" and path == "/api/wakeup":
            # 模拟唤醒：将状态机改为 Connecting
            _mock_sair_state["state"] = "Connecting"
            return {"ok": True}
        elif method == "POST" and path == "/api/abort":
            # 模拟中止：将状态机改为 Cleaning
            _mock_sair_state["state"] = "Cleaning"
            return {"ok": True}
        elif method == "POST" and path == "/api/upgrade":
            return {"ok": True, "status": "upgrading"}
        return {"error": f"Unknown endpoint: {method} {path}"}

    def get_status(self):
        """获取 sair 助手状态（状态机状态、版本号、实时模式等）

        Returns:
            dict: 助手状态信息，如 {"state": "Idle", "version": "...", ...}
        """
        return self._request("GET", "/api/status")

    def get_config(self):
        """获取 sair 助手配置

        Returns:
            dict: 配置信息，包含 ws_url、ws_token、realtime_mode、aec_enabled、log_level 等
        """
        return self._request("GET", "/api/config")

    def set_config(self, config):
        """更新 sair 助手配置

        Args:
            config: 要更新的配置字段字典
                    支持的字段：ws_url, ws_token, realtime_mode, aec_enabled, log_level

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("PUT", "/api/config", config)

    def wakeup(self):
        """唤醒助手，触发语音识别

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/wakeup", {})

    def abort(self):
        """中止当前对话

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/abort", {})

    def upgrade(self):
        """触发助手升级

        Returns:
            dict: {"ok": True, "status": "upgrading"} 成功，{"error": "..."} 失败
        """
        return self._request("POST", "/api/upgrade", {})

    def check_connection(self):
        """检查与 sair 服务的连接状态

        Mock 模式下直接返回 True。
        LIVE 模式下请求 /api/status，检查返回数据中是否包含 "state" 字段。

        Returns:
            bool: True 连接成功，False 连接失败
        """
        if MOCK_MODE:
            self.connected = True
            return True
        try:
            result = self._request("GET", "/api/status")
            self.connected = "state" in result
            return self.connected
        except Exception:
            self.connected = False
            return False
