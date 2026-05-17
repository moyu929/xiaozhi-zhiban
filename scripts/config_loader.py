import json
import os

_config = None

def _find_workspace_root():
    this_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(this_dir)

def get_config():
    global _config
    if _config is not None:
        return _config

    workspace_root = _find_workspace_root()
    config_path = os.path.join(workspace_root, "project_config.json")

    with open(config_path, "r", encoding="utf-8") as f:
        _config = json.load(f)

    _config["_workspace_root"] = workspace_root
    _config["_project_path"] = os.path.join(workspace_root, _config["project_dir"])
    _config["_build_path"] = os.path.join(workspace_root, _config["project_dir"], _config["build_dir"])
    _config["_sair_local"] = os.path.join(
        workspace_root, _config["project_dir"], _config["build_dir"], "sair"
    )
    _config["_wsl_project_path"] = _config["wsl_base_path"] + "/" + _config["project_dir"]

    return _config
