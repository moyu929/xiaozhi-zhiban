import json
import os
import sys

_config = None

_DEFAULTS = {
    "device_ip": "192.168.1.1",
    "device_telnet_port": 23,
    "project_dir": "device/assistant",
    "build_dir": "build",
    "remote_sair_path": "/var/upgrade/sair",
    "remote_sair_new_path": "/var/upgrade/sair_new",
    "nc_upload_port": 19090,
    "http_upload_port": 18080,
    "nc_download_port": 19091,
    "nc_download_port_range": [19090, 19100],
    "http_download_port": 8002,
}

def _find_workspace_root():
    this_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(this_dir)

def _windows_to_wsl_path(win_path):
    win_path = os.path.normpath(win_path)
    drive = win_path[0].lower()
    rest = win_path[2:].replace("\\", "/")
    return f"/mnt/{drive}{rest}"

def get_config():
    global _config
    if _config is not None:
        return _config

    workspace_root = _find_workspace_root()
    config_path = os.path.join(workspace_root, "project_config.json")
    example_path = os.path.join(workspace_root, "project_config.example.json")

    if os.path.exists(config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            _config = json.load(f)
    elif os.path.exists(example_path):
        print(f"提示: 未找到 {config_path}，使用示例配置。")
        print(f"  请复制 project_config.example.json 为 project_config.json 并修改设备IP等参数。")
        with open(example_path, "r", encoding="utf-8") as f:
            _config = json.load(f)
    else:
        print(f"警告: 未找到配置文件，使用内置默认值。")
        print(f"  请创建 {config_path}（可参考 project_config.example.json）。")
        _config = dict(_DEFAULTS)

    _config["_workspace_root"] = workspace_root
    _config["_project_path"] = os.path.join(workspace_root, _config["project_dir"])
    _config["_build_path"] = os.path.join(workspace_root, _config["project_dir"], _config["build_dir"])
    _config["_sair_local"] = os.path.join(
        workspace_root, _config["project_dir"], _config["build_dir"], "sair"
    )
    _config["_wsl_project_path"] = _windows_to_wsl_path(_config["_project_path"])

    return _config
