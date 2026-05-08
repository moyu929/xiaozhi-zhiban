"""
server.py — Panel HTTP 服务器

本模块是小智值班控制面板的后端 HTTP 服务器，主要功能包括：
1. 提供静态文件服务（HTML/CSS/JS 前端页面）
2. 提供 RESTful API 接口，代理前端请求到设备端（xwebd / sair）
3. 支持文件上传（multipart/form-data），并代理到设备端
4. 支持 ADB 端口转发，通过 USB 连接设备
5. 自动检测设备在线状态，切换 MOCK/LIVE 模式

关键概念：
- xwebd：设备端 HTTP 守护进程，提供系统控制 API（音量、亮度、文件管理等），端口 8080
- sair：Assistant 二进制文件名（平台约束不可改名），提供语音助手 API（唤醒、中止、升级等），端口 8081
"""

import json
import os
import sys
import time
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, quote as url_quote
from urllib.request import Request, urlopen
import webbrowser
import threading
import subprocess
import shutil
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device_api import SairAPI, XwebdAPI, set_mock_mode, check_device_alive
import device_api
from adb_manager import (is_adb_available, detect_devices, is_xwebd_installed,
                          check_xwebd_status, deploy_xwebd, update_xwebd,
                          remove_xwebd, start_xwebd, stop_xwebd, restart_xwebd,
                          setup_forward, get_device_ip)

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

XWEBD_PORT = 8080
SAIR_PORT = 8081

_sair_api = None
_xwebd_api = None
_upload_progress = {}


def adb_forward_setup(serial=None):
    """设置 ADB 端口转发，将本地端口映射到设备端口

    通过 USB 连接设备时，需要使用 ADB forward 将本地 TCP 端口
    映射到设备上的 xwebd(8080) 和 sair(8081) 端口。

    Args:
        serial: ADB 设备序列号，为 None 时自动选择（仅一台设备时）

    Returns:
        dict: {"ok": True, "serial": "..."} 成功时返回设备序列号
              {"ok": False, "error": "..."} 失败时返回错误信息
    """
    if not shutil.which("adb"):
        return {"ok": False, "error": "ADB not found. Please install Android SDK Platform Tools."}
    try:
        result = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=5)
        lines = [l.strip() for l in result.stdout.strip().split("\n") if l.strip() and "List" not in l]
        devices = []
        for l in lines:
            parts = l.split("\t")
            if len(parts) >= 2 and parts[1] == "device":
                devices.append(parts[0])
            elif len(parts) >= 2 and parts[1] == "unauthorized":
                return {"ok": False, "error": "ADB device unauthorized. Please confirm USB debugging on the device."}
        if not devices:
            return {"ok": False, "error": "No ADB device connected. Please connect via USB."}
        target = serial if serial else (devices[0] if len(devices) == 1 else None)
        if not target:
            return {"ok": False, "error": "Multiple ADB devices found", "devices": devices}
        subprocess.run(["adb", "-s", target, "forward", f"tcp:{XWEBD_PORT}", f"tcp:{XWEBD_PORT}"],
                       capture_output=True, text=True, timeout=5, check=True)
        subprocess.run(["adb", "-s", target, "forward", f"tcp:{SAIR_PORT}", f"tcp:{SAIR_PORT}"],
                       capture_output=True, text=True, timeout=5, check=True)
        return {"ok": True, "serial": target}
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "ADB command timed out"}
    except Exception as e:
        return {"ok": False, "error": str(e)}


def get_sair_api():
    """获取当前 sair API 实例

    Returns:
        SairAPI: 当前的 sair API 实例，未连接时为 None
    """
    return _sair_api


def get_xwebd_api():
    """获取当前 xwebd API 实例

    Returns:
        XwebdAPI: 当前的 xwebd API 实例，未连接时为 None
    """
    return _xwebd_api


def set_apis(sair_api, xwebd_api):
    """设置全局 API 实例

    Args:
        sair_api: SairAPI 实例
        xwebd_api: XwebdAPI 实例
    """
    global _sair_api, _xwebd_api
    _sair_api = sair_api
    _xwebd_api = xwebd_api


class ControlPanelHandler(BaseHTTPRequestHandler):
    """控制面板 HTTP 请求处理器

    继承自 BaseHTTPRequestHandler，处理所有来自前端页面的 HTTP 请求。
    主要职责：
    - 静态文件服务：返回 HTML/CSS/JS 文件
    - API 路由：将 /api/ 开头的请求路由到对应的处理逻辑
    - 请求代理：将设备控制请求转发到 xwebd 或 sair 服务
    - 文件上传：处理 multipart/form-data 上传并代理到设备端
    """

    def log_message(self, format, *args):
        """禁用默认的请求日志输出"""
        pass

    def _send_json(self, data, status=200):
        """发送 JSON 格式的 HTTP 响应

        Args:
            data: 要序列化为 JSON 的数据（dict/list）
            status: HTTP 状态码，默认 200
        """
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, filepath, content_type):
        """发送静态文件作为 HTTP 响应

        Args:
            filepath: 文件的绝对路径
            content_type: 文件的 MIME 类型
        """
        with open(filepath, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        """读取并解析 HTTP 请求体中的 JSON 数据

        Returns:
            dict: 解析后的 JSON 数据，无请求体时返回空字典
        """
        length = int(self.headers.get("Content-Length", 0))
        if length > 0:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        return {}

    def do_GET(self):
        """处理 GET 请求

        路由规则：
        - / 或 /index.html → 返回前端主页面
        - /style.css → 返回样式文件
        - /app.js → 返回前端脚本
        - /api/* → 转发到 API 处理器
        - 其他 → 返回 404
        """
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/" or path == "/index.html":
            self._send_file(os.path.join(STATIC_DIR, "index.html"), "text/html; charset=utf-8")
        elif path == "/style.css":
            self._send_file(os.path.join(STATIC_DIR, "style.css"), "text/css; charset=utf-8")
        elif path == "/app.js":
            self._send_file(os.path.join(STATIC_DIR, "app.js"), "application/javascript; charset=utf-8")
        elif path.startswith("/api/"):
            self._handle_api("GET", path, None, parse_qs(parsed.query))
        else:
            self.send_error(404)

    def do_POST(self):
        """处理 POST 请求

        路由规则：
        - /api/upload → 文件上传处理（特殊处理 multipart/form-data）
        - /api/* → 转发到 API 处理器
        - 其他 → 返回 404
        """
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/api/upload":
            self._handle_upload()
        elif path.startswith("/api/"):
            data = self._read_body()
            self._handle_api("POST", path, data, {})
        else:
            self.send_error(404)

    def do_PUT(self):
        """处理 PUT 请求

        路由规则：
        - /api/* → 转发到 API 处理器
        - 其他 → 返回 404
        """
        parsed = urlparse(self.path)
        path = parsed.path
        if path.startswith("/api/"):
            data = self._read_body()
            self._handle_api("PUT", path, data, {})
        else:
            self.send_error(404)

    def _handle_upload(self):
        """处理文件上传请求

        接收前端通过 multipart/form-data 上传的文件，解析出文件数据后，
        通过 HTTP 请求代理转发到设备端 xwebd 的 /api/upload 接口。
        上传进度存储在 _upload_progress 字典中，前端可轮询查询。

        流程：
        1. 接收上传数据（分块读取，记录接收进度 0-50%）
        2. 解析 multipart/form-data，提取文件内容和文件名
        3. MOCK 模式下直接返回成功
        4. LIVE 模式下将文件保存到临时目录，然后转发到设备端（进度 55-100%）
        5. 清理临时文件
        """
        xwebd = get_xwebd_api()
        if not xwebd:
            self._send_json({"error": "xwebd not connected"}, 503)
            return
        content_type = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in content_type:
            self._send_json({"error": "Expected multipart/form-data"}, 400)
            return
        boundary = content_type.split("boundary=")[-1].strip()
        content_length = int(self.headers.get("Content-Length", 0))
        upload_max = 20 * 1024 * 1024
        if content_length > upload_max:
            self._send_json({"error": f"File too large, max {upload_max // (1024*1024)}MB"}, 400)
            return
        upload_id = str(uuid.uuid4())[:8]
        _upload_progress[upload_id] = {"progress": 0, "status": "receiving", "method": "proxy"}
        chunk_size = 65536
        received = 0
        chunks = []
        while received < content_length:
            to_read = min(chunk_size, content_length - received)
            chunk = self.rfile.read(to_read)
            if not chunk:
                break
            chunks.append(chunk)
            received += len(chunk)
            pct = int(received / content_length * 50) if content_length > 0 else 50
            _upload_progress[upload_id] = {"progress": pct, "status": "receiving", "method": "proxy"}
        raw = b"".join(chunks)
        boundary_bytes = b"--" + boundary.encode()
        parts = raw.split(boundary_bytes)
        file_data = None
        filename = ""
        for part in parts:
            if b"filename=" not in part:
                continue
            header_end = part.find(b"\r\n\r\n")
            if header_end < 0:
                continue
            header = part[:header_end].decode("utf-8", errors="replace")
            for seg in header.split(";"):
                seg = seg.strip()
                if seg.startswith("filename="):
                    filename = seg.split("=", 1)[1].strip('" ')
            file_data = part[header_end + 4:]
            if file_data.endswith(b"\r\n"):
                file_data = file_data[:-2]
            break
        if not file_data or not filename:
            _upload_progress[upload_id] = {"progress": 0, "status": "error", "error": "No file found"}
            self._send_json({"error": "No file found in upload"}, 400)
            return
        if device_api.MOCK_MODE:
            _upload_progress[upload_id] = {"progress": 100, "status": "uploaded", "method": "mock"}
            self._send_json({"ok": True, "upload_id": upload_id, "file_size": len(file_data), "method": "mock"})
            return
        try:
            import tempfile
            tmp_dir = tempfile.mkdtemp(prefix="xiaozhi_upload_")
            tmp_path = os.path.join(tmp_dir, os.path.basename(filename))
            with open(tmp_path, "wb") as f:
                f.write(file_data)
            _upload_progress[upload_id] = {"progress": 55, "status": "forwarding", "method": "http"}
            url = f"http://{xwebd.host}:{xwebd.port}/api/upload"
            req_data = open(tmp_path, "rb").read()
            req = __import__("urllib.request", fromlist=["Request"]).Request(
                url, data=req_data, method="POST",
                headers={"Content-Type": "application/octet-stream",
                         "X-Filename": os.path.basename(filename)})
            _upload_progress[upload_id] = {"progress": 75, "status": "forwarding", "method": "http"}
            with __import__("urllib.request", fromlist=["urlopen"]).urlopen(req, timeout=120) as resp:
                result = json.loads(resp.read().decode("utf-8"))
            _upload_progress[upload_id] = {"progress": 100, "status": "uploaded", "method": "http"}
            self._send_json({"ok": True, "upload_id": upload_id, "file_size": len(file_data), "method": "http"})
            try:
                os.unlink(tmp_path)
                os.rmdir(tmp_dir)
            except Exception:
                pass
        except Exception as e:
            _upload_progress[upload_id] = {"progress": 0, "status": "error", "error": str(e)}
            try:
                if tmp_path and os.path.exists(tmp_path):
                    os.unlink(tmp_path)
                if tmp_dir and os.path.exists(tmp_dir):
                    os.rmdir(tmp_dir)
            except Exception:
                pass
            self._send_json({"error": f"Upload failed: {e}"}, 500)

    def _handle_api(self, method, path, body, query):
        """统一 API 请求路由处理器

        根据请求方法和路径，将请求路由到对应的处理逻辑。
        大部分 API 是对 xwebd 或 sair 服务的代理转发。

        Args:
            method: HTTP 方法（GET/POST/PUT/DELETE）
            path: 请求路径（如 /api/status）
            body: 请求体数据（POST/PUT 时为 dict，GET 时为 None）
            query: URL 查询参数（GET 时为 dict，POST/PUT 时为空 dict）
        """
        sair = get_sair_api()
        xwebd = get_xwebd_api()

        # 以下路径需要 xwebd 连接才能工作
        if path.startswith("/api/assistant/") or path.startswith("/api/files/") or path == "/api/upload-progress":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return

        # --- 连接设备 ---
        if path == "/api/connect":
            host = body.get("host", "192.168.1.96") if body else "192.168.1.96"
            is_usb = (host == "localhost" or host == "127.0.0.1")
            if is_usb:
                # USB 连接需要先设置 ADB 端口转发
                adb_result = adb_forward_setup()
                if not adb_result.get("ok"):
                    self._send_json(adb_result, 502)
                    return
            new_sair = SairAPI(host, SAIR_PORT)
            new_xwebd = XwebdAPI(host, XWEBD_PORT)
            sair_ok = new_sair.check_connection()
            xwebd_ok = new_xwebd.check_connection()
            set_apis(new_sair, new_xwebd)
            self._send_json({"ok": True, "host": host, "sair_port": SAIR_PORT, "xwebd_port": XWEBD_PORT,
                             "sair_connected": sair_ok, "xwebd_connected": xwebd_ok})
            return

        # --- 获取当前连接信息 ---
        if path == "/api/info":
            self._send_json({
                "mode": "MOCK" if device_api.MOCK_MODE else "LIVE",
                "sair_host": sair.host if sair else None,
                "sair_port": sair.port if sair else SAIR_PORT,
                "xwebd_host": xwebd.host if xwebd else None,
                "xwebd_port": xwebd.port if xwebd else XWEBD_PORT,
                "sair_connected": sair.connected if sair else False,
                "xwebd_connected": xwebd.connected if xwebd else False,
            })
            return

        # --- ADB 端口转发 ---
        if path == "/api/adb-forward":
            serial = body.get("serial") if body else None
            result = adb_forward_setup(serial)
            self._send_json(result)
            return

        # --- 获取设备状态 ---
        if method == "GET" and path == "/api/status":
            if not sair:
                self._send_json({"error": "Not connected"}, 503)
                return
            # 合并 sair 状态和 xwebd 系统信息
            sair_status = sair.get_status()
            if xwebd:
                xwebd_status = xwebd._request("GET", "/api/system")
                if "error" not in xwebd_status:
                    sair_status.update(xwebd_status)
            self._send_json(sair_status)
            return

        # --- 服务状态 ---
        if method == "GET" and path == "/api/services":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/services"))
            return

        # --- 获取日志 ---
        if method == "GET" and path.startswith("/api/logs"):
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            lines = int(query.get("lines", [100])[0])
            lines = max(1, min(lines, 500))
            level = query.get("level", [""])[0]
            source = query.get("source", ["0"])[0]
            url = f"/api/logs?lines={lines}"
            if level:
                url += f"&level={level}"
            if source != "0":
                url += f"&source={source}"
            self._send_json(xwebd._request("GET", url))
            return

        # --- 清除日志 ---
        if method == "POST" and path == "/api/assistant/logs/clear":
            if not xwebd:
                self._send_json({"ok": False, "error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("POST", "/api/assistant/logs/clear"))
            return

        # --- xwebd 自检 ---
        if method == "GET" and path == "/api/xwebd/diag":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/diag"))
            return

        # --- xwebd 版本 ---
        if method == "GET" and path == "/api/xwebd/version":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/version"))
            return

        # --- Assistant 自检 ---
        if method == "GET" and path == "/api/assistant/diag":
            if not sair:
                self._send_json({"error": "sair not connected"}, 503)
                return
            self._send_json(sair._request("GET", "/api/diag"))
            return

        # --- 获取配置 ---
        if method == "GET" and path == "/api/config":
            if not sair:
                self._send_json({"error": "Not connected"}, 503)
                return
            # 合并 sair 和 xwebd 的配置
            sair_cfg = sair.get_config()
            if xwebd:
                xwebd_cfg = xwebd.get_config()
                if "error" not in xwebd_cfg:
                    sair_cfg.update(xwebd_cfg)
            self._send_json(sair_cfg)
            return

        # --- 更新配置 ---
        if method == "PUT" and path == "/api/config":
            if not sair:
                self._send_json({"error": "Not connected"}, 503)
                return
            # 将配置字段分发到对应的服务：sair 字段发给 sair，xwebd 字段发给 xwebd
            sair_fields = {}
            xwebd_fields = {}
            if body:
                for k in ("ws_url", "ws_token", "realtime_mode", "aec_enabled", "log_level"):
                    if k in body:
                        sair_fields[k] = body[k]
                if "upload_max_mb" in body:
                    v, err = _validate_int_range(body["upload_max_mb"], "upload_max_mb", 1, 100)
                    if err:
                        self._send_json({"error": err}, 400)
                        return
                    xwebd_fields["upload_max_mb"] = v
            result = {"ok": True}
            if sair_fields:
                r = sair.set_config(sair_fields)
                if "error" in r:
                    result = r
            if xwebd_fields and xwebd:
                r = xwebd.set_config(xwebd_fields)
                if "error" in r:
                    result = r
            self._send_json(result)
            return

        # --- Assistant 配置 ---
        if method == "PUT" and path == "/api/assistant/config":
            if not sair:
                self._send_json({"error": "sair not connected"}, 503)
                return
            self._send_json(sair._request("PUT", "/api/config", body))
            return

        # --- xwebd 配置 ---
        if method == "PUT" and path == "/api/xwebd/config":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("PUT", "/api/config", body))
            return

        # --- 获取音量 ---
        if method == "GET" and path == "/api/volume":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/volume"))
            return

        # --- 设置音量 ---
        if method == "POST" and path == "/api/volume":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            vol = body.get("volume") if body else None
            if vol is None:
                self._send_json({"error": "volume is required"}, 400)
                return
            v, err = _validate_int_range(vol, "volume", 0, 80)
            if err:
                self._send_json({"error": err}, 400)
                return
            self._send_json(xwebd.set_volume(v))
            return

        # --- 获取亮度 ---
        if method == "GET" and path == "/api/brightness":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/brightness"))
            return

        # --- 设置亮度 ---
        if method == "POST" and path == "/api/brightness":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            brt = body.get("brightness") if body else None
            if brt is None:
                self._send_json({"error": "brightness is required"}, 400)
                return
            v, err = _validate_int_range(brt, "brightness", 0, 900)
            if err:
                self._send_json({"error": err}, 400)
                return
            self._send_json(xwebd.set_brightness(v))
            return

        # --- 获取静音状态 ---
        if method == "GET" and path == "/api/mute":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("GET", "/api/mute"))
            return

        # --- 设置静音 ---
        if method == "POST" and path == "/api/mute":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.set_mute(body.get("muted", False) if body else False))
            return

        # --- 重启设备 ---
        if method == "POST" and path == "/api/reboot":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.reboot())
            return

        # --- 关机 ---
        if method == "POST" and path == "/api/poweroff":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd._request("POST", "/api/poweroff"))
            return

        # --- 唤醒助手（sair 服务） ---
        if method == "POST" and path == "/api/wakeup":
            if not sair:
                self._send_json({"error": "sair not connected"}, 503)
                return
            self._send_json(sair.wakeup())
            return

        # --- 中止当前对话（sair 服务） ---
        if method == "POST" and path == "/api/abort":
            if not sair:
                self._send_json({"error": "sair not connected"}, 503)
                return
            self._send_json(sair.abort())
            return

        # --- 升级助手（sair 服务） ---
        if method == "POST" and path == "/api/upgrade":
            if not sair:
                self._send_json({"error": "sair not connected"}, 503)
                return
            self._send_json(sair.upgrade())
            return

        # --- 列出设备文件 ---
        if method == "GET" and path == "/api/files":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.list_files())
            return

        # --- 下载设备文件 ---
        if method == "GET" and path.startswith("/api/files/download"):
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            file_path = query.get("path", [""])[0] if query else ""
            if not file_path:
                self._send_json({"error": "path parameter required"}, 400)
                return
            try:
                url = f"http://{xwebd.host}:{xwebd.port}/api/files/download?path={url_quote(file_path, safe='')}"
                req = Request(url)
                with urlopen(req, timeout=30) as resp:
                    content_type = resp.headers.get("Content-Type", "application/octet-stream")
                    self.send_response(200)
                    self.send_header("Content-Type", content_type)
                    self.send_header("Content-Disposition", f'attachment; filename="{os.path.basename(file_path)}"')
                    data = resp.read()
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        # --- 删除设备文件 ---
        if method == "DELETE" and path.startswith("/api/files/download"):
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            file_path = query.get("path", [""])[0] if query else ""
            if not file_path:
                self._send_json({"error": "path parameter required"}, 400)
                return
            self._send_json(xwebd._request("DELETE", f"/api/files/download?path={url_quote(file_path, safe='')}"))
            return

        # --- 清理设备临时文件 ---
        if method == "POST" and path == "/api/files/cleanup":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.cleanup())
            return

        # --- 上传文件到设备 ---
        if method == "POST" and path.startswith("/api/files/upload"):
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            target_path = query.get("path", ["/var/upgrade"])[0] if query else "/var/upgrade"
            content_type = self.headers.get('Content-Type', '')
            if 'multipart/form-data' in content_type:
                import cgi
                form = cgi.FieldStorage(fp=self.rfile, headers=self.headers, environ={'REQUEST_METHOD': 'POST', 'CONTENT_TYPE': content_type})
                file_item = form['file']
                if file_item.filename:
                    upload_data = file_item.file.read()
                    url = f"http://{xwebd.host}:{xwebd.port}/api/files/upload?path={url_quote(target_path, safe='')}"
                    req = Request(url, data=upload_data, headers={'Content-Type': 'application/octet-stream', 'X-Filename': file_item.filename})
                    try:
                        with urlopen(req, timeout=60) as resp:
                            resp_data = json.loads(resp.read())
                            self._send_json(resp_data)
                    except Exception as e:
                        self._send_json({"ok": False, "error": str(e)}, 500)
                else:
                    self._send_json({"error": "no file in form"}, 400)
            else:
                self._send_json({"error": "multipart/form-data required"}, 400)
            return

        # --- 部署助手（sair 二进制文件，平台约束不可改名） ---
        if method == "POST" and path == "/api/assistant/deploy":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            # sair 是 Assistant 的二进制文件名（平台约束不可改名）
            file_path = body.get("path", "/var/upgrade/sair_new") if body else "/var/upgrade/sair_new"
            self._send_json(xwebd.deploy_assistant(file_path))
            return

        # --- 更新助手（sair 二进制文件，平台约束不可改名） ---
        if method == "POST" and path == "/api/assistant/update":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            # sair 是 Assistant 的二进制文件名（平台约束不可改名）
            file_path = body.get("path", "/var/upgrade/sair_new") if body else "/var/upgrade/sair_new"
            self._send_json(xwebd.update_assistant(file_path))
            return

        # --- 卸载助手 ---
        if method == "POST" and path == "/api/assistant/uninstall":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.uninstall_assistant())
            return

        # --- 获取助手状态 ---
        if method == "GET" and path == "/api/assistant/status":
            if not xwebd:
                self._send_json({"error": "xwebd not connected"}, 503)
                return
            self._send_json(xwebd.assistant_status())
            return

        # --- 查询上传进度 ---
        if method == "GET" and path.startswith("/api/upload-progress"):
            qs = path.split("?", 1)[-1] if "?" in path else ""
            uid = qs.split("=")[-1] if "upload_id=" in qs else ""
            info = _upload_progress.get(uid, {"progress": 0, "status": "unknown"})
            self._send_json(info)
            return

        # --- 检测 ADB 设备列表 ---
        if method == "GET" and path == "/api/adb/devices":
            if not is_adb_available():
                self._send_json({"error": "ADB not installed", "adb_available": False}, 503)
                return
            devices = detect_devices()
            result = []
            for d in devices:
                info = {"serial": d["serial"], "model": d.get("model", "unknown")}
                info["xwebd_installed"] = is_xwebd_installed(d["serial"])
                info["xwebd_status"] = check_xwebd_status(d["serial"])
                info["ip"] = get_device_ip(d["serial"])
                result.append(info)
            self._send_json({"adb_available": True, "devices": result})
            return

        # --- 检查 ADB 和 xwebd 状态 ---
        if method == "GET" and path == "/api/adb/check":
            serial = query.get("serial", [None])[0] if query else None
            if not is_adb_available():
                self._send_json({"adb_available": False})
                return
            self._send_json({
                "adb_available": True,
                "xwebd_installed": is_xwebd_installed(serial),
                "xwebd_status": check_xwebd_status(serial),
                "ip": get_device_ip(serial),
            })
            return

        # --- 通过 ADB 部署 xwebd 到设备 ---
        if method == "POST" and path == "/api/deploy/xwebd":
            serial = body.get("serial") if body else None
            binary_path = body.get("binary_path") if body else None
            r = deploy_xwebd(serial, binary_path)
            if r["ok"]:
                # 部署成功后自动设置端口转发并获取设备 IP
                fwd = setup_forward(serial)
                ip = get_device_ip(serial)
                self._send_json({"ok": True, "ip": ip, "forward": fwd})
            else:
                self._send_json({"error": r.get("error", "deploy failed")}, 500)
            return

        # --- 通过 ADB 更新设备上的 xwebd ---
        if method == "POST" and path == "/api/xwebd/update":
            serial = body.get("serial") if body else None
            binary_path = body.get("binary_path") if body else None
            r = update_xwebd(serial, binary_path)
            self._send_json(r if r["ok"] else {"error": r.get("error", "update failed")}, 200 if r["ok"] else 500)
            return

        # --- 通过 ADB 移除设备上的 xwebd ---
        if method == "POST" and path == "/api/xwebd/remove":
            serial = body.get("serial") if body else None
            r = remove_xwebd(serial)
            self._send_json(r if r["ok"] else {"error": r.get("error", "remove failed")}, 200 if r["ok"] else 500)
            return

        # --- 通过 ADB 重启设备上的 xwebd ---
        if method == "POST" and path == "/api/xwebd/restart":
            serial = body.get("serial") if body else None
            r = restart_xwebd(serial)
            self._send_json(r if r["ok"] else {"error": r.get("error", "restart failed")}, 200 if r["ok"] else 500)
            return

        # --- 设置 ADB 端口转发 ---
        if method == "POST" and path == "/api/adb/forward":
            serial = body.get("serial") if body else None
            r = setup_forward(serial)
            self._send_json(r)
            return

        self._send_json({"error": f"Unknown: {method} {path}"}, 404)


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


def create_server(host="0.0.0.0", port=3000, device_host="192.168.1.96", force_live=False):
    """创建 HTTP 服务器实例

    根据设备在线状态自动决定是否启用 MOCK 模式，
    然后初始化 API 实例并创建多线程 HTTP 服务器。

    Args:
        host: 服务器监听地址，默认 "0.0.0.0"（所有网卡）
        port: 服务器监听端口，默认 3000
        device_host: 设备 IP 地址，默认 "192.168.1.96"
        force_live: 是否强制 LIVE 模式（连接真实设备），默认 False

    Returns:
        ThreadingHTTPServer: 已配置好的 HTTP 服务器实例
    """
    if force_live:
        set_mock_mode(False)
    elif check_device_alive(device_host, XWEBD_PORT):
        # 设备在线，使用 LIVE 模式
        set_mock_mode(False)
    else:
        # 设备离线，使用 MOCK 模式
        set_mock_mode(True)

    sair_api = SairAPI(device_host, SAIR_PORT)
    xwebd_api = XwebdAPI(device_host, XWEBD_PORT)
    sair_api.check_connection()
    xwebd_api.check_connection()
    set_apis(sair_api, xwebd_api)

    server = ThreadingHTTPServer((host, port), ControlPanelHandler)
    return server


def start_server(host="0.0.0.0", port=3000, device_host="192.168.1.96", open_browser=True, force_live=False):
    """启动控制面板 HTTP 服务器

    创建服务器实例，打印启动信息，可选自动打开浏览器，
    然后进入服务循环。

    Args:
        host: 服务器监听地址，默认 "0.0.0.0"
        port: 服务器监听端口，默认 3000
        device_host: 设备 IP 地址，默认 "192.168.1.96"
        open_browser: 是否自动打开浏览器，默认 True
        force_live: 是否强制 LIVE 模式，默认 False
    """
    server = create_server(host, port, device_host, force_live=force_live)
    url = f"http://localhost:{port}"

    mode_label = "MOCK" if device_api.MOCK_MODE else "LIVE"
    print(f"=== xiaozhi-zhiban 控制面板 v3.0 ===")
    print(f"  模式: {mode_label}")
    print(f"  面板: {url}")
    print(f"  xwebd: {device_host}:{XWEBD_PORT}")
    print(f"  sair:  {device_host}:{SAIR_PORT}")
    print(f"  按 Ctrl+C 停止")
    print()

    if open_browser:
        # 延迟 0.5 秒后打开浏览器，确保服务器已就绪
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        server.shutdown()
