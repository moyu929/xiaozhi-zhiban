"""
server.py — Panel HTTP 服务器

本模块是小智智伴控制面板的后端 HTTP 服务器，主要功能包括：
1. 提供静态文件服务（HTML/CSS/JS 前端页面）
2. 提供 RESTful API 接口，代理前端请求到设备端 xwebd
3. 支持文件上传（multipart/form-data），并代理到设备端
4. 支持 ADB 端口转发，通过 USB 连接设备
5. 连接真实设备，设备离线时返回连接错误

关键概念：
- xwebd：设备端 HTTP 守护进程，提供系统控制 API（音量、亮度、文件管理等）及助手代理 API，端口 8080
- sair：Assistant 二进制文件名（平台约束不可改名），所有请求通过 xwebd 转发
"""

import json
import logging
import os
import socket
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
import io

logger = logging.getLogger("panel.server")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device_api import XwebdAPI
from config import DEFAULT_DEVICE_HOST, DEFAULT_PANEL_PORT, DEFAULT_XWEBD_PORT
from adb_manager import (is_adb_available, detect_devices, is_xwebd_installed,
                          check_xwebd_status, deploy_xwebd, update_xwebd,
                          remove_xwebd, start_xwebd, stop_xwebd, restart_xwebd,
                          setup_forward, get_device_ip, get_device_info,
                          check_sair_status, deploy_sair, hot_update_sair, reboot_device,
                          poweroff_device, get_device_logs, _find_adb,
                          init_device, is_device_initialized, factory_reset_device,
                          check_xwebd_env)

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

XWEBD_PORT = 8080

_xwebd_api = None
_xwebd_lock = threading.Lock()
_upload_progress = {}
_upload_progress_lock = threading.Lock()


def get_panel_logs(lines_count=100):
    log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "panel.log")
    if not os.path.exists(log_path):
        return []
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        all_lines = f.readlines()
    all_lines = [l.rstrip("\n\r") for l in all_lines]
    return all_lines[-lines_count:] if len(all_lines) > lines_count else all_lines


def _parse_multipart(headers, rfile):
    """解析 multipart/form-data 请求，提取文件名和文件数据

    替代已弃用的 cgi.FieldStorage（Python 3.13 移除）。

    Args:
        headers: HTTP 请求头字典
        rfile: 请求体输入流

    Returns:
        dict: {"filename": str, "data": bytes, "fields": dict} 或 None
    """
    content_type = headers.get("Content-Type", "")
    if "multipart/form-data" not in content_type:
        return None
    boundary = content_type.split("boundary=")[-1].strip()
    if not boundary:
        return None
    content_length = int(headers.get("Content-Length", 0))
    if content_length <= 0:
        return None
    raw = rfile.read(content_length)
    boundary_bytes = b"--" + boundary.encode()
    parts = raw.split(boundary_bytes)
    result = {"filename": None, "data": None, "fields": {}}
    for part in parts:
        if b"Content-Disposition" not in part:
            continue
        header_end = part.find(b"\r\n\r\n")
        if header_end < 0:
            continue
        header = part[:header_end].decode("utf-8", errors="replace")
        body = part[header_end + 4:]
        if body.endswith(b"\r\n"):
            body = body[:-2]
        name = None
        filename = None
        for seg in header.split(";"):
            seg = seg.strip()
            if seg.startswith("name="):
                name = seg.split("=", 1)[1].strip('" ')
            elif seg.startswith("filename="):
                filename = seg.split("=", 1)[1].strip('" ')
        if filename:
            result["filename"] = filename
            result["data"] = body
        elif name:
            result["fields"][name] = body.decode("utf-8", errors="replace")
    return result


def adb_forward_setup(serial=None):
    """设置 ADB 端口转发，将本地端口映射到设备端口

    通过 USB 连接设备时，需要使用 ADB forward 将本地 TCP 端口
    映射到设备上的 xwebd(8080) 端口。

    Args:
        serial: ADB 设备序列号，为 None 时自动选择（仅一台设备时）

    Returns:
        dict: {"ok": True, "serial": "..."} 成功时返回设备序列号
              {"ok": False, "error": "..."} 失败时返回错误信息
    """
    if not is_adb_available():
        return {"ok": False, "error": "ADB not found. Please install Android SDK Platform Tools."}
    try:
        adb_cmd = _find_adb()
        result = subprocess.run([adb_cmd, "devices"], capture_output=True, text=True, timeout=5)
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
        subprocess.run([adb_cmd, "-s", target, "forward", f"tcp:{XWEBD_PORT}", f"tcp:{XWEBD_PORT}"],
                       capture_output=True, text=True, timeout=5, check=True)
        logger.info("ADB端口转发设置成功: serial=%s", target)
        return {"ok": True, "serial": target}
    except subprocess.TimeoutExpired:
        logger.warning("ADB端口转发失败: ADB command timed out")
        return {"ok": False, "error": "ADB command timed out"}
    except Exception as e:
        logger.warning("ADB端口转发失败: %s", str(e))
        return {"ok": False, "error": str(e)}


def get_xwebd_api():
    with _xwebd_lock:
        return _xwebd_api


def set_apis(xwebd_api):
    global _xwebd_api
    with _xwebd_lock:
        _xwebd_api = xwebd_api
    logger.info("API实例更新: xwebd=%s", xwebd_api is not None)


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


_API_ROUTES = {}
_STREAM_SENTINEL = object()


def _api_route(method, path):
    def decorator(func):
        _API_ROUTES[(method, path)] = func
        return func
    return decorator


def _requires_xwebd(func):
    def wrapper(handler, xwebd, body, query):
        if not xwebd:
            return {"error": "xwebd not connected"}, 503
        return func(handler, xwebd, body, query)
    return wrapper


@_api_route("POST", "/api/connect")
def _api_connect(handler, xwebd, body, query):
    host = body.get("host", DEFAULT_DEVICE_HOST) if body else DEFAULT_DEVICE_HOST
    is_usb = (host == "localhost" or host == "127.0.0.1")
    logger.info("连接设备: host=%s, usb=%s", host, is_usb)
    if is_usb:
        adb_result = adb_forward_setup()
        if not adb_result.get("ok"):
            return adb_result, 502
    new_xwebd = XwebdAPI(host, XWEBD_PORT)
    xwebd_ok = new_xwebd.check_connection()
    if xwebd_ok:
        sair_status = new_xwebd._request("GET", "/api/assistant/status")
        sair_ok = sair_status and "error" not in sair_status and sair_status.get("running", False)
        set_apis(new_xwebd)
        logger.info("连接结果: xwebd=%s, assistant=%s", xwebd_ok, sair_ok)
        return {"ok": True, "host": host, "xwebd_port": XWEBD_PORT,
                "sair_connected": sair_ok, "xwebd_connected": xwebd_ok}

    set_apis(None)
    if is_usb:
        adb_reachable = False
        adb_serial = None
        try:
            devices = detect_devices()
            if devices:
                adb_reachable = True
                adb_serial = devices[0]["serial"]
                logger.info("xwebd不可达，但ADB设备可用: %s", adb_serial)
        except Exception as e:
            logger.debug("ADB检测异常: %s", e)

        if adb_reachable:
            return {"ok": True, "host": host, "xwebd_connected": False,
                    "sair_connected": False, "adb_reachable": True,
                    "adb_serial": adb_serial}, 200
    logger.warning("连接失败: host=%s", host)
    return {"ok": False, "error": "无法连接设备，请检查设备是否开机及IP地址是否正确",
            "host": host, "sair_connected": False, "xwebd_connected": False,
            "adb_reachable": False}, 502


@_api_route("GET", "/api/info")
def _api_info(handler, xwebd, body, query):
    return {
        "mode": "LIVE",
        "xwebd_host": xwebd.host if xwebd else None,
        "xwebd_port": xwebd.port if xwebd else XWEBD_PORT,
        "xwebd_connected": xwebd.connected if xwebd else False,
    }


@_api_route("POST", "/api/adb-forward")
def _api_adb_forward(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    return adb_forward_setup(serial)


@_api_route("GET", "/api/status")
def _api_status(handler, xwebd, body, query):
    result = {}
    if xwebd:
        xwebd_status = xwebd._request("GET", "/api/system", timeout=10)
        if xwebd_status and "error" not in xwebd_status:
            result.update(xwebd_status)
        sair_status = xwebd._request("GET", "/api/assistant/status", timeout=5)
        if sair_status and "error" not in sair_status:
            if "state" in sair_status:
                result["state"] = sair_status["state"]
            if "version" in sair_status:
                result["assistant_version"] = sair_status["version"]
            result["assistant_installed"] = sair_status.get("installed", False)
            result["assistant_running"] = sair_status.get("running", False)
            if "activation_code" in sair_status:
                result["activation_code"] = sair_status["activation_code"]
            if "ws_url" in sair_status:
                result["ws_url"] = sair_status["ws_url"]
    if not result:
        return {"error": "Not connected"}, 503
    return result


@_api_route("GET", "/api/services")
@_requires_xwebd
def _api_services(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/services")

@_api_route("POST", "/api/services/toggle")
@_requires_xwebd
def _api_services_toggle(handler, xwebd, body, query):
    return xwebd._request("POST", "/api/services/toggle", body)


@_api_route("GET", "/api/panel/logs/stream")
def _api_panel_logs_stream(handler, xwebd, body, query):
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("Connection", "keep-alive")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    try:
        logs = get_panel_logs(80)
        last_count = len(logs)
        for line in logs:
            data = json.dumps({"text": line}, ensure_ascii=False)
            handler.wfile.write(("data: " + data + "\n\n").encode("utf-8"))
        handler.wfile.flush()
        while True:
            try:
                logs = get_panel_logs(80)
                current_count = len(logs)
                if current_count != last_count:
                    new_lines = logs[last_count:]
                    for line in new_lines:
                        data = json.dumps({"text": line}, ensure_ascii=False)
                        handler.wfile.write(("data: " + data + "\n\n").encode("utf-8"))
                    handler.wfile.flush()
                    last_count = current_count
            except Exception:
                pass
            time.sleep(1)
    except (BrokenPipeError, ConnectionResetError):
        pass
    return _STREAM_SENTINEL


@_api_route("GET", "/api/panel/logs")
def _api_panel_logs(handler, xwebd, body, query):
    lines_count = int(query.get("lines", [100])[0])
    lines_count = max(1, min(lines_count, 500))
    level = query.get("level", [""])[0]
    try:
        all_lines = get_panel_logs(500)
        if level:
            level_map = {"E": ["ERROR", "CRITICAL"], "W": ["WARNING"], "I": ["INFO"], "D": ["DEBUG"]}
            targets = level_map.get(level, [level])
            all_lines = [l for l in all_lines if any(t in l for t in targets)]
        result_lines = all_lines[-lines_count:] if len(all_lines) > lines_count else all_lines
        return {"lines": result_lines, "source": "panel"}
    except Exception as e:
        logger.error("读取Panel日志失败: %s", e)
        return {"error": str(e)}, 500


@_api_route("POST", "/api/panel/logs/clean")
def _api_panel_logs_clean(handler, xwebd, body, query):
    try:
        log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "panel.log")
        if os.path.exists(log_path):
            with open(log_path, "w", encoding="utf-8") as f:
                pass
        logger.info("Panel日志已清理")
        return {"ok": True}
    except Exception as e:
        logger.error("清理Panel日志失败: %s", e)
        return {"error": str(e)}, 500


@_api_route("POST", "/api/logs/clean")
@_requires_xwebd
def _api_logs_clean(handler, xwebd, body, query):
    source = query.get("source", ["0"])[0] if query else "0"
    url = "/api/logs/clean"
    if source and source != "0":
        url += "?source=" + source
    logger.info("清理设备端日志: source=%s", source)
    return xwebd._request("POST", url)


@_api_route("GET", "/api/logs")
@_requires_xwebd
def _api_logs(handler, xwebd, body, query):
    lines = int(query.get("lines", [100])[0])
    lines = max(1, min(lines, 500))
    level = query.get("level", [""])[0]
    source = query.get("source", ["0"])[0]
    url = f"/api/logs?lines={lines}"
    if level:
        url += f"&level={level}"
    if source and source != "0":
        url += f"&source={source}"
    return xwebd._request("GET", url)


@_api_route("GET", "/api/device/logs/stream")
def _api_device_logs_stream(handler, xwebd, body, query):
    if not xwebd:
        return {"error": "xwebd not connected"}, 503
    source = query.get("source", ["0"])[0] if query else "0"
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("Connection", "keep-alive")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    last_line_count = 0
    try:
        while True:
            url = "/api/logs?lines=100"
            if source and source != "0":
                url += f"&source={source}"
            result = xwebd._request("GET", url)
            if result and "error" not in result:
                logs = result.get("logs", [])
                current_count = len(logs)
                if current_count > last_line_count and last_line_count > 0:
                    new_logs = logs[last_line_count:]
                    for log_entry in new_logs:
                        data = json.dumps(log_entry, ensure_ascii=False)
                        handler.wfile.write(f"data: {data}\n\n".encode("utf-8"))
                    handler.wfile.flush()
                elif last_line_count == 0 and current_count > 0:
                    handler.wfile.write(f"data: {json.dumps({'type': 'init', 'count': current_count}, ensure_ascii=False)}\n\n".encode("utf-8"))
                    handler.wfile.flush()
                last_line_count = current_count
            else:
                handler.wfile.write(b": ping\n\n")
                handler.wfile.flush()
            time.sleep(5)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    except Exception as e:
        logger.debug("设备日志SSE断开: %s", e)
    return _STREAM_SENTINEL


@_api_route("GET", "/api/processes")
@_requires_xwebd
def _api_processes(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/processes")


@_api_route("POST", "/api/processes/control")
@_requires_xwebd
def _api_processes_control(handler, xwebd, body, query):
    return xwebd._request("POST", "/api/processes/control", body)


@_api_route("GET", "/api/usb/mode")
@_requires_xwebd
def _api_usb_mode(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/usb/mode")


@_api_route("POST", "/api/usb/mode")
@_requires_xwebd
def _api_usb_mode_set(handler, xwebd, body, query):
    return xwebd._request("POST", "/api/usb/mode", body)


@_api_route("GET", "/api/xwebd/diag")
@_requires_xwebd
def _api_xwebd_diag(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/diag")


@_api_route("GET", "/api/xwebd/version")
@_requires_xwebd
def _api_xwebd_version(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/version")


@_api_route("GET", "/api/assistant/diag")
@_requires_xwebd
def _api_assistant_diag(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/assistant/diag")


@_api_route("GET", "/api/assistant/env")
@_requires_xwebd
def _api_assistant_env(handler, xwebd, body, query):
    return xwebd._request("GET", "/api/assistant/env")


@_api_route("GET", "/api/assistant/config")
@_requires_xwebd
def _api_assistant_config_get(handler, xwebd, body, query):
    return xwebd.get_assistant_config()


@_api_route("GET", "/api/xwebd/config")
@_requires_xwebd
def _api_xwebd_config_get(handler, xwebd, body, query):
    return xwebd.get_config()


@_api_route("GET", "/api/config")
def _api_config_get(handler, xwebd, body, query):
    if not xwebd:
        return {"error": "Not connected"}, 503
    result = xwebd.get_assistant_config()
    xwebd_cfg = xwebd.get_config()
    if "error" not in xwebd_cfg:
        result.update(xwebd_cfg)
    return result


@_api_route("PUT", "/api/config")
def _api_config_put(handler, xwebd, body, query):
    if not xwebd:
        return {"error": "Not connected"}, 503
    assistant_fields = {}
    xwebd_fields = {}
    if body:
        for k in ("ws_url", "ws_token"):
            if k in body:
                assistant_fields[k] = body[k]
        if "log_level" in body:
            xwebd_fields["log_level"] = body["log_level"]
        if "upload_max_mb" in body:
            v, err = _validate_int_range(body["upload_max_mb"], "upload_max_mb", 1, 100)
            if err:
                return {"error": err}, 400
            xwebd_fields["upload_max_mb"] = v
    result = {"ok": True}
    logger.info("更新配置: assistant_fields=%s, xwebd_fields=%s", list(assistant_fields.keys()), list(xwebd_fields.keys()))
    if assistant_fields:
        r = xwebd.set_assistant_config(assistant_fields)
        if "error" in r:
            result = r
    if xwebd_fields:
        r = xwebd.set_config(xwebd_fields)
        if "error" in r:
            result = r
    return result


@_api_route("PUT", "/api/assistant/config")
@_requires_xwebd
def _api_assistant_config_put(handler, xwebd, body, query):
    return xwebd.set_assistant_config(body or {})


@_api_route("PUT", "/api/xwebd/config")
@_requires_xwebd
def _api_xwebd_config_put(handler, xwebd, body, query):
    return xwebd._request("PUT", "/api/config", body)


@_api_route("POST", "/api/reboot")
@_requires_xwebd
def _api_reboot(handler, xwebd, body, query):
    logger.warning("重启设备")
    return xwebd.reboot()


@_api_route("POST", "/api/poweroff")
@_requires_xwebd
def _api_poweroff(handler, xwebd, body, query):
    logger.warning("关机")
    return xwebd._request("POST", "/api/poweroff")


@_api_route("POST", "/api/assistant/upgrade")
@_requires_xwebd
def _api_assistant_upgrade(handler, xwebd, body, query):
    from adb_manager import _find_sair_binary
    binary_path = _find_sair_binary()
    if not binary_path:
        return {"error": "sair 二进制文件不存在，请先编译"}, 404
    logger.info("热更新助手: binary=%s", binary_path)
    with open(binary_path, "rb") as f:
        binary_data = f.read()
    upload_r = xwebd._request("POST", "/api/upload",
                               body=binary_data,
                               headers={"Content-Type": "application/octet-stream",
                                        "X-Filename": "sair_new"},
                               timeout=30)
    if not upload_r or upload_r.get("error"):
        return {"error": "上传sair文件失败: " + (upload_r.get("error", "") if upload_r else "连接失败")}, 500
    time.sleep(1)
    r = xwebd.upgrade_assistant()
    if r and r.get("ok"):
        return {"ok": True, "method": r.get("method", "hot_update")}
    if not r:
        return {"ok": True, "method": "hot_update"}
    return r


@_api_route("POST", "/api/assistant/wakeup")
@_requires_xwebd
def _api_assistant_wakeup(handler, xwebd, body, query):
    logger.info("唤醒助手")
    return xwebd.wakeup_assistant()


@_api_route("POST", "/api/assistant/abort")
@_requires_xwebd
def _api_assistant_abort(handler, xwebd, body, query):
    logger.info("中止对话")
    return xwebd.abort_assistant()


@_api_route("POST", "/api/assistant/activate")
@_requires_xwebd
def _api_assistant_activate(handler, xwebd, body, query):
    logger.info("激活助手")
    r = xwebd.activate_assistant()
    if r and (r.get("ok") or r.get("code") == 0):
        return {"ok": True, "message": r.get("msg", "ok")}
    if not r:
        return {"error": "连接失败"}, 502
    return {"error": r.get("msg", r.get("error", "激活失败"))}, 500


@_api_route("POST", "/api/upgrade")
@_requires_xwebd
def _api_upgrade(handler, xwebd, body, query):
    logger.info("升级助手")
    return xwebd.upgrade_assistant()


@_api_route("POST", "/api/wakeup")
@_requires_xwebd
def _api_wakeup(handler, xwebd, body, query):
    logger.info("唤醒助手")
    return xwebd.wakeup_assistant()


@_api_route("POST", "/api/abort")
@_requires_xwebd
def _api_abort(handler, xwebd, body, query):
    logger.info("中止对话")
    return xwebd.abort_assistant()


@_api_route("GET", "/api/files")
@_requires_xwebd
def _api_files_list(handler, xwebd, body, query):
    file_path = query.get("path", ["/var/upgrade"])[0] if query else "/var/upgrade"
    return xwebd.list_files(file_path)


@_api_route("GET", "/api/files/download")
@_requires_xwebd
def _api_files_download(handler, xwebd, body, query):
    file_path = query.get("path", [""])[0] if query else ""
    if not file_path:
        return {"error": "path parameter required"}, 400
    logger.info("下载设备文件: %s", file_path)
    try:
        url = f"http://{xwebd.host}:{xwebd.port}/api/files/download?path={url_quote(file_path, safe='')}"
        req = Request(url)
        with urlopen(req, timeout=30) as resp:
            content_type = resp.headers.get("Content-Type", "application/octet-stream")
            handler.send_response(200)
            handler.send_header("Content-Type", content_type)
            handler.send_header("Content-Disposition", f'attachment; filename="{os.path.basename(file_path)}"')
            data = resp.read()
            handler.send_header("Content-Length", str(len(data)))
            handler.end_headers()
            handler.wfile.write(data)
    except Exception as e:
        return {"error": str(e)}, 500
    return _STREAM_SENTINEL


@_api_route("DELETE", "/api/files/download")
@_requires_xwebd
def _api_files_delete(handler, xwebd, body, query):
    file_path = query.get("path", [""])[0] if query else ""
    if not file_path:
        return {"error": "path parameter required"}, 400
    logger.info("删除设备文件: %s", file_path)
    return xwebd._request("DELETE", f"/api/files/download?path={url_quote(file_path, safe='')}")


@_api_route("POST", "/api/files/cleanup")
@_requires_xwebd
def _api_files_cleanup(handler, xwebd, body, query):
    logger.info("清理设备临时文件")
    return xwebd.cleanup()


@_api_route("POST", "/api/files/upload")
@_requires_xwebd
def _api_files_upload(handler, xwebd, body, query):
    target_path = query.get("path", ["/var/upgrade"])[0] if query else "/var/upgrade"
    content_type = handler.headers.get('Content-Type', '')
    if 'multipart/form-data' in content_type:
        parsed = _parse_multipart(handler.headers, handler.rfile)
        if not parsed or not parsed["filename"] or not parsed["data"]:
            return {"error": "no file in form"}, 400
        upload_data = parsed["data"]
        filename = parsed["filename"]
        logger.info("文件上传到设备: %s -> %s", filename, target_path)
        url = f"http://{xwebd.host}:{xwebd.port}/api/files/upload?path={url_quote(target_path, safe='')}"
        req = Request(url, data=upload_data, headers={'Content-Type': 'application/octet-stream', 'X-Filename': filename})
        try:
            with urlopen(req, timeout=60) as resp:
                resp_data = json.loads(resp.read())
                return resp_data
        except Exception as e:
            return {"ok": False, "error": str(e)}, 500
    else:
        return {"error": "multipart/form-data required"}, 400


@_api_route("POST", "/api/assistant/smart-deploy")
@_requires_xwebd
def _api_assistant_smart_deploy(handler, xwebd, body, query):
    from adb_manager import _find_sair_binary
    binary_path = _find_sair_binary()
    if not binary_path:
        return {"error": "sair 二进制文件不存在，请先编译"}, 404
    logger.info("智能部署助手: binary=%s", binary_path)
    with open(binary_path, "rb") as f:
        binary_data = f.read()
    upload_r = xwebd._request("POST", "/api/upload",
                               body=binary_data,
                               headers={"Content-Type": "application/octet-stream",
                                        "X-Filename": "sair_new"},
                               timeout=30)
    if not upload_r or upload_r.get("error"):
        return {"error": "上传sair文件失败: " + (upload_r.get("error", "") if upload_r else "连接失败")}, 500
    time.sleep(2)
    deploy_r = xwebd.deploy_assistant("/var/upgrade/sair_new")
    logger.info("部署助手结果: %s", deploy_r)
    if deploy_r and deploy_r.get("ok"):
        method = deploy_r.get("method", "deploy")
        rebooting = method != "hot_update"
        return {"ok": True, "method": method, "rebooting": rebooting}
    if not deploy_r:
        return {"ok": True, "method": "deploy", "rebooting": True}
    err_str = str(deploy_r.get("error", "")).lower()
    if any(kw in err_str for kw in ["connection", "timed out", "reset", "refused", "broken", "errno"]):
        return {"ok": True, "method": "deploy", "rebooting": True}
    return deploy_r


@_api_route("POST", "/api/assistant/deploy")
@_requires_xwebd
def _api_assistant_deploy(handler, xwebd, body, query):
    file_path = body.get("path", "/var/upgrade/sair_new") if body else "/var/upgrade/sair_new"
    logger.info("部署助手: path=%s", file_path)
    return xwebd.deploy_assistant(file_path)


@_api_route("POST", "/api/assistant/update")
@_requires_xwebd
def _api_assistant_update(handler, xwebd, body, query):
    from adb_manager import _find_sair_binary
    binary_path = _find_sair_binary()
    if not binary_path:
        return {"error": "sair 二进制文件不存在，请先编译"}, 404
    logger.info("冷更新助手: binary=%s", binary_path)
    with open(binary_path, "rb") as f:
        binary_data = f.read()
    upload_r = xwebd._request("POST", "/api/upload",
                               body=binary_data,
                               headers={"Content-Type": "application/octet-stream",
                                        "X-Filename": "sair_new"},
                               timeout=30)
    if not upload_r or upload_r.get("error"):
        return {"error": "上传sair文件失败: " + (upload_r.get("error", "") if upload_r else "连接失败")}, 500
    time.sleep(1)
    r = xwebd.update_assistant("/var/upgrade/sair_new")
    if r and r.get("ok"):
        return {"ok": True, "rebooting": True}
    if not r:
        return {"ok": True, "rebooting": True}
    err_str = str(r.get("error", "")).lower()
    if any(kw in err_str for kw in ["connection", "timed out", "reset", "refused", "broken", "errno", "unfinished"]):
        return {"ok": True, "rebooting": True}
    return r


@_api_route("POST", "/api/assistant/uninstall")
@_requires_xwebd
def _api_assistant_uninstall(handler, xwebd, body, query):
    logger.info("卸载助手")
    r = xwebd.uninstall_assistant()
    if r and r.get("ok"):
        return {"ok": True}
    if not r:
        err_str = ""
    else:
        err_str = str(r.get("error", "")).lower()
    if any(kw in err_str for kw in ["connection", "timed out", "reset", "refused", "broken", "errno", "unfinished"]):
        return {"ok": True, "rebooting": True}
    if not r:
        return {"ok": True}
    return r


@_api_route("GET", "/api/assistant/status")
@_requires_xwebd
def _api_assistant_status(handler, xwebd, body, query):
    status = xwebd.get_assistant_status()
    if status and not status.get("error"):
        config = xwebd.get_assistant_config()
        if config and not config.get("error"):
            for key in ["listen_timeout", "session_timeout", "wakeup_cooldown", "ws_ping_interval"]:
                if key in config:
                    status[key] = config[key]
    return status


@_api_route("GET", "/api/upload-progress")
@_requires_xwebd
def _api_upload_progress(handler, xwebd, body, query):
    uid = query.get("upload_id", [""])[0] if query else ""
    with _upload_progress_lock:
        info = _upload_progress.get(uid, {"progress": 0, "status": "unknown"}).copy()
    return info


@_api_route("GET", "/api/adb/devices")
def _api_adb_devices(handler, xwebd, body, query):
    if not is_adb_available():
        return {"error": "ADB not installed", "adb_available": False}, 503
    devices = detect_devices()
    result = []
    for d in devices:
        info = {"serial": d["serial"], "model": d.get("model", "unknown"),
                "state": d.get("state", "unknown")}
        if "device" in d:
            info["device"] = d["device"]
        info["xwebd_installed"] = is_xwebd_installed(d["serial"])
        info["xwebd_status"] = check_xwebd_status(d["serial"])
        info["ip"] = get_device_ip(d["serial"])
        info["initialized"] = is_device_initialized(d["serial"])
        result.append(info)
    return {"adb_available": True, "devices": result}


@_api_route("GET", "/api/adb/check")
def _api_adb_check(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    if not is_adb_available():
        return {"adb_available": False}
    return {
        "adb_available": True,
        "xwebd_installed": is_xwebd_installed(serial),
        "xwebd_status": check_xwebd_status(serial),
        "ip": get_device_ip(serial),
    }


@_api_route("GET", "/api/adb/init-status")
def _api_adb_init_status(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    if not is_adb_available():
        return {"adb_available": False}
    result = is_device_initialized(serial)
    result["adb_available"] = True
    return result


@_api_route("POST", "/api/adb/init-device")
def _api_adb_init_device(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    logger.info("初始化设备: serial=%s", serial)
    r = init_device(serial)
    if r["ok"]:
        return {"ok": True, "details": r}
    return {"error": r.get("error", "init failed")}, 500


@_api_route("POST", "/api/deploy/xwebd")
def _api_deploy_xwebd(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    binary_path = body.get("binary_path") if body else None
    logger.info("通过ADB部署xwebd: serial=%s", serial)
    r = deploy_xwebd(serial, binary_path)
    if r["ok"]:
        fwd = setup_forward(serial)
        ip = get_device_ip(serial)
        return {"ok": True, "ip": ip, "forward": fwd}
    return {"error": r.get("error", "deploy failed")}, 500


@_api_route("POST", "/api/xwebd/update")
def _api_xwebd_update(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    binary_path = body.get("binary_path") if body else None
    logger.info("通过ADB更新xwebd: serial=%s", serial)
    r = update_xwebd(serial, binary_path)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "update failed")}, 500


@_api_route("POST", "/api/xwebd/upload-update")
def _api_xwebd_upload_update(handler, xwebd, body, query):
    logger.info("通过ADB上传更新xwebd")
    if not is_adb_available():
        return {"error": "ADB 不可用"}, 503
    content_type = handler.headers.get('Content-Type', '')
    if 'multipart/form-data' in content_type:
        parsed = _parse_multipart(handler.headers, handler.rfile)
        if not parsed or not parsed["filename"] or not parsed["data"]:
            return {"error": "no file in form"}, 400
        import tempfile
        tmp_dir = tempfile.mkdtemp(prefix="xiaozhi_xwebd_")
        tmp_path = os.path.join(tmp_dir, "xwebd")
        with open(tmp_path, "wb") as f:
            f.write(parsed["data"])
        serial = parsed["fields"].get("serial")
        r = update_xwebd(serial, tmp_path)
        try:
            shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception:
            pass
    else:
        serial = body.get("serial") if body else None
        binary_path = body.get("binary_path") if body else None
        r = update_xwebd(serial, binary_path)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "update failed")}, 500


@_api_route("POST", "/api/xwebd/wireless-update")
@_requires_xwebd
def _api_xwebd_wireless_update(handler, xwebd, body, query):
    logger.info("无线模式更新xwebd")
    local_binary = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "device", "xwebd", "build", "xwebd")
    if not os.path.exists(local_binary):
        return {"error": "xwebd二进制文件不存在，请先编译"}, 404
    with open(local_binary, "rb") as f:
        binary_data = f.read()
    r = xwebd._request("POST", "/api/upload", body=binary_data,
                        headers={"Content-Type": "application/octet-stream",
                                 "X-Filename": "xwebd_new"})
    if not r or r.get("error"):
        return {"error": "上传xwebd_new失败: " + (r.get("error", "") if r else "连接失败")}, 500
    time.sleep(1)
    r2 = xwebd._request("POST", "/api/self-update")
    if not r2:
        return {"ok": True, "rebooting": True}
    if r2.get("error"):
        return {"error": "自更新失败: " + r2.get("error", "")}, 500
    return {"ok": True, "rebooting": True}


@_api_route("POST", "/api/xwebd/remove")
def _api_xwebd_remove(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    logger.info("通过ADB移除xwebd: serial=%s", serial)
    r = remove_xwebd(serial)
    if r["ok"]:
        return r
    return {"error": r.get("error", "remove failed")}, 500


@_api_route("POST", "/api/xwebd/restart")
def _api_xwebd_restart(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    logger.info("通过ADB重启xwebd: serial=%s", serial)
    r = restart_xwebd(serial)
    if r["ok"]:
        return r
    return {"error": r.get("error", "restart failed")}, 500


@_api_route("POST", "/api/adb/forward")
def _api_adb_forward_post(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    logger.info("设置ADB端口转发: serial=%s", serial)
    return setup_forward(serial)


@_api_route("GET", "/api/adb/device-info")
def _api_adb_device_info(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    return get_device_info(serial)


@_api_route("GET", "/api/adb/sair-status")
def _api_adb_sair_status(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    status = check_sair_status(serial)
    xwebd_status = check_xwebd_status(serial)
    return {"ok": True, "sair": status, "xwebd": xwebd_status}


@_api_route("POST", "/api/adb/deploy-sair")
def _api_adb_deploy_sair(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    binary_path = body.get("binary_path") if body else None
    mode = body.get("mode", "cold") if body else "cold"
    logger.info("通过ADB部署sair: serial=%s, mode=%s", serial, mode)
    if mode == "hot":
        r = hot_update_sair(serial, binary_path)
    else:
        r = deploy_sair(serial, binary_path)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "deploy failed")}, 500


@_api_route("POST", "/api/adb/uninstall-sair")
def _api_adb_uninstall_sair(handler, xwebd, body, query):
    from adb_manager import uninstall_sair
    serial = body.get("serial") if body else None
    r = uninstall_sair(serial)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "uninstall failed")}, 500


@_api_route("POST", "/api/adb/reboot")
def _api_adb_reboot(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    r = reboot_device(serial)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "reboot failed")}, 500


@_api_route("POST", "/api/adb/poweroff")
def _api_adb_poweroff(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    r = poweroff_device(serial)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "poweroff failed")}, 500


@_api_route("POST", "/api/adb/factory-reset")
def _api_adb_factory_reset(handler, xwebd, body, query):
    serial = body.get("serial") if body else None
    r = factory_reset_device(serial)
    if r.get("ok"):
        return r
    return {"error": r.get("error", "factory reset failed")}, 500


@_api_route("GET", "/api/adb/logs")
def _api_adb_logs(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    log_type = query.get("type", ["all"])[0] if query else "all"
    lines = int(query.get("lines", [100])[0]) if query else 100
    return get_device_logs(serial, log_type, lines)


@_api_route("GET", "/api/adb/xwebd-env")
def _api_adb_xwebd_env(handler, xwebd, body, query):
    serial = query.get("serial", [None])[0] if query else None
    if not is_adb_available():
        return {"error": "ADB not installed", "adb_available": False}, 503
    return check_xwebd_env(serial)


class ControlPanelHandler(BaseHTTPRequestHandler):
    """控制面板 HTTP 请求处理器

    继承自 BaseHTTPRequestHandler，处理所有来自前端页面的 HTTP 请求。
    主要职责：
    - 静态文件服务：返回 HTML/CSS/JS 文件
    - API 路由：将 /api/ 开头的请求路由到对应的处理逻辑
    - 请求代理：将设备控制请求转发到 xwebd 服务
    - 文件上传：处理 multipart/form-data 上传并代理到设备端
    """

    def log_message(self, format, *args):
        msg = format % args
        skip_paths = ['/api/panel/logs/stream', '/api/device/logs/stream',
                      '/api/logs?', '/api/panel/logs?',
                      '/api/status', '/api/assistant/status', '/api/services']
        for p in skip_paths:
            if p in msg:
                return
        logger.debug("%s - %s", self.client_address[0], msg)

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
        query = parse_qs(parsed.query)
        if path == "/api/upload":
            self._handle_upload()
        elif path.startswith("/api/"):
            content_type = self.headers.get("Content-Type", "")
            if "multipart/form-data" in content_type:
                self._handle_api("POST", path, None, query)
            else:
                data = self._read_body()
                self._handle_api("POST", path, data, query)
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
        query = parse_qs(parsed.query)
        if path.startswith("/api/"):
            data = self._read_body()
            self._handle_api("PUT", path, data, query)
        else:
            self.send_error(404)

    def do_DELETE(self):
        """处理 DELETE 请求

        路由规则：
        - /api/* → 转发到 API 处理器
        - 其他 → 返回 404
        """
        parsed = urlparse(self.path)
        path = parsed.path
        if path.startswith("/api/"):
            self._handle_api("DELETE", path, None, parse_qs(parsed.query))
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
        3. 将文件保存到临时目录，然后转发到设备端（进度 55-100%）
        4. 清理临时文件
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
        with _upload_progress_lock:
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
            with _upload_progress_lock:
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
        logger.info("上传开始: %s (%d bytes), upload_id=%s", filename, content_length, upload_id)
        if not file_data or not filename:
            with _upload_progress_lock:
                _upload_progress[upload_id] = {"progress": 0, "status": "error", "error": "No file found"}
            self._send_json({"error": "No file found in upload"}, 400)
            return
        try:
            import tempfile
            tmp_dir = tempfile.mkdtemp(prefix="xiaozhi_upload_")
            tmp_path = os.path.join(tmp_dir, os.path.basename(filename))
            with open(tmp_path, "wb") as f:
                f.write(file_data)
            logger.debug("上传接收完成: %d bytes", len(file_data))
            with _upload_progress_lock:
                _upload_progress[upload_id] = {"progress": 55, "status": "forwarding", "method": "http"}
            url = f"http://{xwebd.host}:{xwebd.port}/api/upload"
            logger.info("上传转发到设备: %s -> %s", filename, url)
            with open(tmp_path, "rb") as f:
                req_data = f.read()
            req = Request(
                url, data=req_data, method="POST",
                headers={"Content-Type": "application/octet-stream",
                         "X-Filename": os.path.basename(filename)})
            with _upload_progress_lock:
                _upload_progress[upload_id] = {"progress": 75, "status": "forwarding", "method": "http"}
            with urlopen(req, timeout=120) as resp:
                result = json.loads(resp.read().decode("utf-8"))
            with _upload_progress_lock:
                _upload_progress[upload_id] = {"progress": 100, "status": "uploaded", "method": "http"}
            logger.info("上传成功: %s (%d bytes)", filename, len(file_data))
            self._send_json({"ok": True, "upload_id": upload_id, "file_size": len(file_data), "method": "http"})
            try:
                os.unlink(tmp_path)
                os.rmdir(tmp_dir)
            except Exception:
                pass
        except Exception as e:
            logger.error("上传失败: %s", e)
            with _upload_progress_lock:
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
        xwebd = get_xwebd_api()
        handler = _API_ROUTES.get((method, path))
        if handler:
            try:
                result = handler(self, xwebd, body, query)
            except Exception as e:
                logger.error("API error: %s %s: %s", method, path, e)
                self._send_json({"error": str(e)}, 500)
                return
            if result is _STREAM_SENTINEL:
                return
            if result is None:
                return
            if isinstance(result, tuple):
                data, status = result
            else:
                data, status = result, 200
            self._send_json(data, status)
            return
        if not xwebd and (path.startswith("/api/assistant/") or path.startswith("/api/files/") or path == "/api/upload-progress"):
            self._send_json({"error": "xwebd not connected"}, 503)
            return
        logger.warning("未知API: %s %s", method, path)
        self._send_json({"error": f"Unknown: {method} {path}"}, 404)


def create_server(host="0.0.0.0", port=DEFAULT_PANEL_PORT, device_host=DEFAULT_DEVICE_HOST):
    """创建 HTTP 服务器实例

    始终使用 LIVE 模式，连接真实设备。
    设备离线时 API 请求会返回连接错误。

    Args:
        host: 服务器监听地址，默认 "0.0.0.0"（所有网卡）
        port: 服务器监听端口（默认 3000）
        device_host: 设备 IP 地址（默认从 XIAOZHI_DEVICE_HOST 环境变量读取）

    Returns:
        ThreadingHTTPServer: 已配置好的 HTTP 服务器实例
    """
    xwebd_api = XwebdAPI(device_host, XWEBD_PORT)
    xwebd_api.check_connection()
    set_apis(xwebd_api)

    logger.info("创建服务器: %s:%d, device=%s", host, port, device_host)
    server = ThreadingHTTPServer((host, port), ControlPanelHandler)
    return server


def start_server(host="0.0.0.0", port=DEFAULT_PANEL_PORT, device_host=DEFAULT_DEVICE_HOST, open_browser=True):
    """启动控制面板 HTTP 服务器

    创建服务器实例，打印启动信息，可选自动打开浏览器，
    然后进入服务循环。

    Args:
        host: 服务器监听地址，默认 "0.0.0.0"
        port: 服务器监听端口（默认 3000）
        device_host: 设备 IP 地址（默认从 XIAOZHI_DEVICE_HOST 环境变量读取）
        open_browser: 是否自动打开浏览器，默认 True
    """
    server = create_server(host, port, device_host)
    url = f"http://localhost:{port}"
    logger.info("服务器启动: %s", url)

    print(f"=== xiaozhi-zhiban 控制面板 v3.0 ===")
    print(f"  面板: {url}")
    print(f"  xwebd: {device_host}:{XWEBD_PORT}")
    print(f"  按 Ctrl+C 停止")
    print()

    if open_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        logger.info("服务器已停止")
        server.shutdown()
