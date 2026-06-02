import os

DEFAULT_DEVICE_HOST = os.environ.get("XIAOZHI_DEVICE_HOST", "")
DEFAULT_PANEL_PORT = int(os.environ.get("XIAOZHI_PANEL_PORT", "3000"))
DEFAULT_XWEBD_PORT = int(os.environ.get("XIAOZHI_XWEBD_PORT", "8080"))
