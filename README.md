# 小智·智伴 (xiaozhi-zhiban)

小智AI语音助手的预编译固件，开箱即用，直接部署到设备。

## 项目组成

| 组件 | 说明 |
|------|------|
| **assistant** | 语音助手模块（sair），替换设备原版sair进程 |
| **xwebd** | Web控制守护进程，提供HTTP API供Panel通信 |
| **panel** | PC端控制面板，Web界面管理设备 |

## 支持设备

- GS705B（Cortex-A5, uClibc 0.9.33.2）

## 快速开始

### 1. 启动Panel

```bash
cd panel
pip install -r requirements.txt  # 首次运行
python control_panel.py
```

### 2. 连接设备

1. 确保设备通过USB连接到电脑
2. 浏览器打开 `http://localhost:3000`
3. 在面板中完成设备连接

### 3. 部署固件

在Panel界面中操作：
1. 上传 `device/assistant/prebuilt/sair` 和 `device/xwebd/prebuilt/xwebd` 到设备
2. 上传 `device/xwebd/scripts/boot_watchdog.sh` 到设备
3. 通过Panel执行热更新或重启设备

## 文件说明

```
device/
├── assistant/
│   └── prebuilt/sair          # 语音助手二进制
├── xwebd/
│   ├── prebuilt/xwebd         # Web守护进程二进制
│   └── scripts/boot_watchdog.sh  # 开机看门狗脚本
panel/                          # PC端控制面板
├── server.py                  # 后端服务
├── control_panel.py           # 主入口
├── device_api.py              # xwebd API客户端
├── assistant_api.py           # assistant API客户端
├── adb_manager.py             # ADB设备管理
└── static/                    # 前端文件
```

## 开发者

如需二次开发，请使用 [xiaozhi-zhiban-develop](https://github.com/你的用户名/xiaozhi-zhiban/tree/develop) 分支。

## 许可证

MIT License
