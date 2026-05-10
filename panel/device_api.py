"""
device_api.py — 设备 API 封装（XwebdAPI 类）

本模块封装了与设备端 xwebd HTTP 守护进程通信的 API 客户端，主要功能包括：
1. XwebdAPI 类：封装所有与 xwebd 服务的 HTTP 交互（系统信息、音量、亮度、文件管理、助手管理等）
2. 设备存活检测：check_device_alive() 函数用于判断设备是否在线

关键概念：
- xwebd：设备端 HTTP 守护进程，端口 8080，提供系统控制 API
- sair：Assistant 的二进制文件名（平台约束不可改名），端口 8081
"""

import json
import time
import logging
import threading
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

logger = logging.getLogger("panel.device_api")


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
                if check_key in data:
                    logger.debug("设备存活检查: %s:%d = True", host, port)
                    return True
    except Exception:
        pass
    logger.debug("设备存活检查: %s:%d = False", host, port)
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

        Args:
            method: HTTP 方法（GET/POST/PUT/DELETE）
            path: API 路径（如 /api/system）
            data: 请求体数据（dict），为 None 时不发送请求体

        Returns:
            dict: API 响应数据，请求失败时返回 {"error": "..."}
        """
        url = f"{self.base_url}{path}"
        body = json.dumps(data).encode("utf-8") if data is not None else None
        headers = {"Content-Type": "application/json"}
        req = Request(url, data=body, method=method, headers=headers)
        logger.debug("-> %s %s", method, path)
        try:
            with urlopen(req, timeout=5) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                logger.debug("<- %s %s: ok", method, path)
                return result
        except Exception as e:
            logger.warning("<- %s %s: %s", method, path, e)
            return {"error": str(e)}

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
        logger.info("设置音量: %d", volume)
        return self._request("POST", "/api/volume", {"volume": volume})

    def set_brightness(self, brightness):
        """设置设备亮度

        Args:
            brightness: 亮度值（0-900）

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        logger.info("设置亮度: %d", brightness)
        return self._request("POST", "/api/brightness", {"brightness": brightness})

    def set_mute(self, muted):
        logger.info("设置静音: %s", muted)
        return self._request("POST", "/api/mute", {"muted": 1 if muted else 0})

    def reboot(self):
        """重启设备

        Returns:
            dict: {"ok": True} 成功，{"error": "..."} 失败
        """
        logger.warning("重启设备")
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
        logger.info("部署助手: path=%s", path)
        return self._request("POST", "/api/assistant/deploy", {"path": path})

    def update_assistant(self, path="/var/upgrade/sair_new"):
        """更新设备上的助手

        Args:
            path: sair 新版本文件路径，默认 "/var/upgrade/sair_new"
                  （sair 是 Assistant 的二进制文件名，平台约束不可改名）

        Returns:
            dict: {"ok": True, "message": "..."} 成功，{"error": "..."} 失败
        """
        logger.info("更新助手: path=%s", path)
        return self._request("POST", "/api/assistant/update", {"path": path})

    def uninstall_assistant(self):
        """卸载设备上的助手

        Returns:
            dict: {"ok": True, "message": "..."} 成功，{"error": "..."} 失败
        """
        logger.info("卸载助手")
        return self._request("POST", "/api/assistant/uninstall", {})

    def assistant_status(self):
        """获取助手安装和运行状态

        Returns:
            dict: {"installed": True/False, "version": "...", "running": True/False}
        """
        return self._request("GET", "/api/assistant/status")

    def wakeup_assistant(self):
        logger.info("唤醒助手")
        return self._request("POST", "/api/assistant/wakeup")

    def abort_assistant(self):
        logger.info("中止对话")
        return self._request("POST", "/api/assistant/abort")

    def upgrade_assistant(self):
        logger.info("升级助手")
        return self._request("POST", "/api/assistant/upgrade")

    def get_assistant_status(self):
        return self._request("GET", "/api/assistant/status")

    def get_assistant_config(self):
        return self._request("GET", "/api/assistant/config")

    def set_assistant_config(self, config):
        logger.info("更新助手配置: %s", list(config.keys()))
        return self._request("PUT", "/api/assistant/config", config)

    def poweroff(self):
        return self._request("POST", "/api/poweroff")

    def get_services(self):
        return self._request("GET", "/api/services")

    def get_version(self):
        return self._request("GET", "/api/version")

    def check_connection(self):
        """检查与 xwebd 服务的连接状态

        请求 /api/system，检查返回数据中是否包含 "cpu" 字段。

        Returns:
            bool: True 连接成功，False 连接失败
        """
        try:
            result = self._request("GET", "/api/system")
            self.connected = "cpu" in result
            if self.connected:
                logger.info("xwebd连接成功: %s:%d", self.host, self.port)
            else:
                logger.warning("xwebd连接失败: %s:%d", self.host, self.port)
            return self.connected
        except Exception:
            self.connected = False
            logger.warning("xwebd连接失败: %s:%d", self.host, self.port)
            return False
