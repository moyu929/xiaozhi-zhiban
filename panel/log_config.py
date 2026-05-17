"""
log_config.py — Panel 日志配置

统一配置 Panel 所有模块的日志输出，包括：
1. 控制台输出（带颜色和时间戳）
2. 文件输出（写入 panel.log，自动轮转）

使用方式：
    from log_config import setup_logging
    setup_logging()  # 在程序入口调用一次

各模块使用：
    import logging
    logger = logging.getLogger(__name__)
"""

import logging
import os
import sys


LOG_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(LOG_DIR, "panel.log")


class _ColorFormatter(logging.Formatter):
    _COLORS = {
        logging.DEBUG: "\033[36m",
        logging.INFO: "\033[32m",
        logging.WARNING: "\033[33m",
        logging.ERROR: "\033[31m",
        logging.CRITICAL: "\033[35m",
    }
    _RESET = "\033[0m"

    def format(self, record):
        original = record.levelname
        color = self._COLORS.get(record.levelno, "")
        record.levelname = f"{color}{record.levelname:<8}{self._RESET}"
        result = super().format(record)
        record.levelname = original
        return result


def setup_logging(level=logging.DEBUG):
    root = logging.getLogger("panel")
    root.setLevel(level)

    if root.handlers:
        return

    fmt = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    datefmt = "%H:%M:%S"

    console = logging.StreamHandler(sys.stdout)
    console.setLevel(level)
    console.setFormatter(_ColorFormatter(fmt, datefmt=datefmt))
    root.addHandler(console)

    try:
        file_handler = logging.FileHandler(LOG_FILE, encoding="utf-8")
        file_handler.setLevel(level)
        file_handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)s %(name)s: %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        ))
        root.addHandler(file_handler)
    except Exception:
        pass

    logging.getLogger(__name__).info("日志系统已初始化, level=%s, file=%s",
                                     logging.getLevelName(level), LOG_FILE)
