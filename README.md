# 小智·智伴 (xiaozhi-zhiban)

<p align="center">
  <strong>GS705B 早教机器人的开源替代固件 — 开箱即用，直接部署</strong>
</p>

> **致敬** — 本项目参考并参照复刻了 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 开源项目（[开发文档](https://my.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb) | [作者 B站](https://space.bilibili.com/59357679)），在此向原作者及社区致敬。

---

## 📖 项目简介

小智·智伴是一个为 GS705B 早教机器人开发的开源语音助手替代方案。它替换设备原版的 `sair` 语音助手进程，提供更灵活的云端对接能力和设备管理功能。

本项目是 **预编译二进制版**，无需编译环境，下载即可部署到设备。如需二次开发，请使用 [develop 分支](https://github.com/moyu929/xiaozhi-zhiban/tree/develop)（[Gitee 镜像](https://gitee.com/beichen929/xiaozhi-zhiban/tree/develop)）。

### ✨ 核心特性

- 🎙️ **语音唤醒与对话** — 支持唤醒词检测、ASR 语音识别、WebSocket 实时对话
- 🌐 **Web 控制面板** — 通过浏览器管理设备，支持 WiFi 和 USB 两种连接方式
- 🔄 **热更新** — 无需重启设备即可更新语音助手，秒级完成（通过版本号变化检测更新成功）
- 🛡️ **安全回退** — 内置开机看门狗，连续启动失败自动回退到原版固件
- 📊 **实时监控** — 设备状态、日志、配置一览无余
- ⚙️ **运行时配置** — WebSocket 地址、超时参数、日志级别等均可在线调整
- 🔗 **MCP 接入点** — 支持配置 xiaozhi.me 智能体专属 MCP 端点，实现工具调用能力扩展

---

## 🏗️ 项目组成

| 组件 | 运行平台 | 说明 |
|------|----------|------|
| **assistant** (sair) | 设备端 | 语音助手模块，负责唤醒词检测、ASR、WebSocket 通信、状态管理、TTS 播放 |
| **xwebd** | 设备端 | Web 控制守护进程，提供 HTTP API 供 Panel 通信，管理设备端文件、音量、亮度等 |
| **panel** | PC 端 | 控制面板，Web 界面统一管理设备，替代各种零散的调试/部署脚本 |

### 三模块通信架构

```
┌─────────────┐     HTTP      ┌─────────────┐    文件IPC     ┌──────────────┐
│   Panel     │ ──────────→  │   xwebd     │ ──────────→  │  assistant   │
│  (PC:3000)  │ ←────────── │ (设备:8080)  │ ←──────────  │   (sair)     │
│  浏览器界面  │     JSON     │  HTTP API   │  sair_cmd.json │  语音助手    │
└─────────────┘              └─────────────┘  sair_status.json└──────────────┘
                                              sair_config.json
```

---

## 📱 支持设备

### GS705B 早教机器人

| 项目 | 规格 |
|------|------|
| **SoC** | ACTIONS OWL (ARM Cortex-A5 四核 900MHz) |
| **内存** | ~56MB |
| **存储** | NAND Flash，9.8MB 可写分区 |
| **音频** | ALSA 驱动，2 麦克风 + 扬声器 |
| **网络** | WiFi (RTL8189FTV) |
| **C 库** | uClibc 0.9.33.2 |
| **内核** | Linux 3.10.52 |
| **浮点 ABI** | hard-float (VFPv4 + NEON) |

---

## 🚀 快速开始

### 前提条件

- **Python 3.8+**（运行 Panel 控制面板）
- **ADB**（Android Debug Bridge，用于 USB 连接模式）
- **USB 数据线**（连接设备和电脑）或 **WiFi**（设备和电脑在同一局域网）

### 第 1 步：获取项目

```bash
git clone -b main https://github.com/moyu929/xiaozhi-zhiban.git
# 或使用 Gitee 镜像：
# git clone -b main https://gitee.com/beichen929/xiaozhi-zhiban.git
cd xiaozhi-zhiban
```

### 第 2 步：启动 Panel 控制面板

```bash
cd panel
python control_panel.py
```

> Panel 仅使用 Python 标准库，无需安装额外依赖。

启动后浏览器会自动打开 `http://localhost:3000`。

> 💡 也可以通过命令行参数指定设备 IP：
> ```bash
> python control_panel.py --device-host 192.168.1.96
> python control_panel.py --no-browser     # 不自动打开浏览器
> python control_panel.py --port 8080      # 指定监听端口
> ```

### 第 3 步：连接设备

Panel 支持两种连接方式：

#### 方式一：USB 连接（有线模式）

1. 用 USB 数据线连接设备和电脑
2. 确保设备已开启 ADB 调试（设备默认已开启）
3. 在 Panel 中点击「连接」

USB 模式功能包括：文件管理、进程查看、固件部署、设备重启等。

#### 方式二：WiFi 连接（无线模式）

1. 确保设备和电脑在同一局域网
2. 在 Panel 中输入设备 IP 地址（如 `192.168.1.96`）
3. 点击「连接」

> ⚠️ 新设备**必须先通过有线模式安装面板内核（xwebd）**，才能使用无线模式连接。

无线模式功能更完整，除 USB 模式的所有功能外，还支持：语音助手管理、实时日志查看、配置修改、热更新等。

### 第 4 步：部署固件

在 Panel 界面中操作：

1. **部署 xwebd** — 在「内核管理」区域点击「部署」，将 `device/xwebd/prebuilt/xwebd` 上传到设备
2. **部署 assistant** — 在「语音助手」区域点击「部署」，将 `device/assistant/prebuilt/sair` 上传到设备
3. 设备会自动重启，重启后新固件生效

> ⚠️ **部署顺序**：必须**先安装 xwebd → 再安装语音助手（sair）**。卸载时反向操作：**先卸载 sair → 再卸载 xwebd**，或直接使用有线模式下的「恢复出厂设置」功能一键清除。

---

## 📂 文件说明

```
xiaozhi-zhiban/
├── device/
│   ├── assistant/
│   │   └── prebuilt/
│   │       └── sair                  # 语音助手二进制（替换设备原版sair）
│   └── xwebd/
│       ├── prebuilt/
│       │   └── xwebd                 # Web控制守护进程二进制
│       └── scripts/
│           └── boot_watchdog.sh      # 开机看门狗脚本（自动部署）
├── panel/
│   ├── control_panel.py              # 控制面板启动入口
│   ├── server.py                     # HTTP 后端服务
│   ├── device_api.py                 # xwebd API 客户端
│   ├── adb_manager.py                # ADB 设备管理
│   ├── config.py                     # 配置（环境变量读取）
│   ├── log_config.py                 # 日志配置
│   ├── _adb_check.py                 # ADB 连接诊断工具
│   └── static/
│       ├── index.html                # 前端页面
│       ├── app.js                    # 前端逻辑
│       └── style.css                 # 前端样式
├── LICENSE
└── README.md
```

---

## 🎛️ Panel 功能一览

### 设备管理

| 功能 | 有线模式 | 无线模式 |
|------|----------|----------|
| 设备连接/断开 | ✅ | ✅ |
| 设备重启 | ✅ | ✅ |
| 文件上传/下载 | ✅ | ✅ |
| 进程查看 | ✅ | ✅ |

### 语音助手管理

| 功能 | 有线模式 | 无线模式 |
|------|----------|----------|
| 部署助手 | ✅ | ✅ |
| 热更新（不重启设备） | ✅ | ✅ |
| 冷更新（重启设备） | ✅ | ✅ |
| 卸载助手 | ✅ | ✅ |
| 激活设备 | ❌ | ✅ |
| 唤醒/中止对话 | ❌ | ✅ |
| 修改配置 | ❌ | ✅ |
| 实时日志 | ❌ | ✅ |
| 状态监控 | ❌ | ✅ |

### xwebd 管理

| 功能 | 有线模式 | 无线模式 |
|------|----------|----------|
| 部署 xwebd | ✅ | ✅ |
| 更新 xwebd | ✅ | ✅ |
| 音量/亮度控制 | ❌ | ✅ |
| WiFi 信息 | ❌ | ✅ |
| 磁盘/内存信息 | ❌ | ✅ |
| 自检诊断 | ❌ | ✅ |

---

## ⚙️ 配置说明

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `XIAOZHI_DEVICE_HOST` | (空) | 设备 IP 地址 |
| `XIAOZHI_PANEL_PORT` | 3000 | Panel 监听端口 |
| `XIAOZHI_XWEBD_PORT` | 8080 | xwebd 服务端口 |

### 命令行参数

```
python control_panel.py [选项]

选项:
  --host HOST         监听地址 (默认: 0.0.0.0)
  --port PORT         监听端口 (默认: 3000)
  --device-host IP    设备IP地址
  --no-browser        不自动打开浏览器
  --live              强制Live模式（连接真实设备）
```

---

## 🛡️ 安全机制

### 开机看门狗 (boot_watchdog.sh)

部署到设备后，`boot_watchdog.sh` 会在每次开机时自动检测启动是否成功：

1. 设备启动后 120 秒内创建「存活标记」
2. 如果重启时存活标记不存在，说明上次启动短命，计数器 +1
3. 连续 3 次短命启动 → 自动删除 `/var/upgrade/sair`，回退到原版固件
4. 正常启动 → 计数器清零

这意味着即使部署了有问题的固件，设备也能自动恢复，不会变砖。

### 受保护文件

以下文件通过 xwebd API 禁止删除，防止误操作导致设备无法连接：

- `xwebd` — Web 控制守护进程
- `sair` / `sair_backup` — 语音助手
- `boot_watchdog.sh` — 开机看门狗
- `test.sh` — 自启动脚本
- `xiaozhi.log` — 持久化日志

---

## 🔧 常见问题

### Q: 设备连接不上？

1. 检查设备是否开机，WiFi 是否正常
2. USB 模式：确认 ADB 已识别设备（`adb devices`）
3. WiFi 模式：确认设备 IP 正确，尝试 ping 设备
4. 检查设备是否已部署 xwebd（WiFi 模式需要 xwebd 运行）

### Q: 部署后设备循环重启？

这是开机看门狗的保护机制，连续 3 次短命启动后会自动回退到原版固件。如果未自动回退：

```bash
adb shell "rm -f /var/upgrade/sair; reboot"
```

### Q: 如何回退到原版固件？

删除 `/var/upgrade/sair` 文件后重启设备，manager 会自动启动原版 `/usr/bin/sair`：

```bash
# 通过 Panel 操作：语音助手 → 卸载
# 或通过 ADB：
adb shell "rm -f /var/upgrade/sair /var/upgrade/sair_backup; reboot"
```

### Q: 如何查看设备日志？

- **通过 Panel**：无线模式连接后，在「日志」区域查看实时日志
- **通过 ADB**：`adb shell cat /var/upgrade/xiaozhi.log`
- **崩溃日志**：`adb shell cat /var/upgrade/crash.log`

### Q: 语音助手无法唤醒？

1. 确认助手已部署且状态为「运行中」
2. 检查 WebSocket 连接是否正常（Panel 中查看状态）
3. 如果状态为「激活中」，点击「激活」按钮完成设备激活
4. 查看日志中是否有 ASR 初始化错误

---

## 🔄 关于实时对话模式

本项目使用 **AutoStop（自动停止）模式**：TTS 播放时停止向云端上传音频，TTS 播放结束后恢复上传。用户可以通过唤醒词打断当前播放。

由于 GS705B 设备无硬件 AEC（回声消除）能力，且软件 AEC 在 soft-float ABI 下性能不足，Realtime（实时对话）模式不可用。这是硬件限制，非软件问题。

---

## 🧑‍💻 开发者

如需修改源码或二次开发，请使用 [develop 分支](https://github.com/moyu929/xiaozhi-zhiban/tree/develop)（[Gitee 镜像](https://gitee.com/beichen929/xiaozhi-zhiban/tree/develop)），其中包含完整的源代码、编译脚本和开发文档。

---

## 📄 许可证

MIT License

Copyright (c) 2025 xiaozhi-zhiban contributors

本项目使用 MIT 许可证开源。请注意，设备固件中的闭源动态库（libapplib.so、libduilite_fespl.so 等）不属于本项目，它们由设备原生系统提供。
