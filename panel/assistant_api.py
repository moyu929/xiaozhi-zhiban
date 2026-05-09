"""
assistant_api.py — Assistant API 封装（SairAPI 类）

本模块封装了与设备端 sair（Assistant）服务通信的 API 客户端，主要功能包括：
1. SairAPI 类：封装所有与 sair 服务的 HTTP 交互（状态查询、配置管理、唤醒、中止、升级等）

关键说明：
- sair 是 Assistant 的二进制文件名（平台约束不可改名），运行在设备端端口 8081
- sair 提供语音助手相关 API：状态机控制（唤醒/中止）、WebSocket 配置、升级等
"""

import json
from urllib.request import Request, urlopen


class SairAPI:
    """sair（Assistant）服务的 API 客户端

    封装了与 sair 服务的所有 HTTP 交互，包括：
    - 助手状态查询（当前状态机状态、版本号等）
    - 配置管理（WebSocket 地址/令牌、实时模式、AEC、日志级别等）
    - 唤醒助手（触发语音识别）
    - 中止当前对话
    - 触发助手升级
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

    def _request(self, method, path, data=None):
        url = f"{self.base_url}{path}"
        body = json.dumps(data).encode("utf-8") if data is not None else None
        headers = {"Content-Type": "application/json"}
        req = Request(url, data=body, method=method, headers=headers)
        try:
            with urlopen(req, timeout=5) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as e:
            err_str = str(e)
            if any(k in err_str for k in ("ConnectionReset", "Broken pipe", "104",
                                           "远程主机强迫关闭", "10054", "10053",
                                           "Remote end closed")):
                if method == "POST" and path in ("/api/abort", "/api/wakeup", "/api/upgrade"):
                    return {"ok": True}
                if method == "PUT" and path == "/api/config":
                    return {"ok": True, "note": "config applied, sair restarting"}
            return {"error": err_str}

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

        请求 /api/status，检查返回数据中是否包含 "state" 字段。

        Returns:
            bool: True 连接成功，False 连接失败
        """
        try:
            result = self._request("GET", "/api/status")
            self.connected = "state" in result
            return self.connected
        except Exception:
            self.connected = False
            return False
