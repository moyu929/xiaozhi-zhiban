# 小智·智伴 (xiaozhi-zhiban) — Develop

<p align="center">
  <strong>GS705B 早教机器人的开源替代固件 — 开发者版本，包含完整源码</strong>
</p>

---

## 📖 项目简介

小智·智伴是一个为 GS705B 早教机器人开发的开源语音助手替代方案。本项目是 **开发版 (develop)** 分支，包含完整的 C 源代码、编译脚本和开发文档，供开发者二次开发。

如果你只想使用预编译版本部署到设备，请使用 [main 分支](https://github.com/moyu929/xiaozhi-zhiban/tree/main)（[Gitee 镜像](https://gitee.com/beichen929/xiaozhi-zhiban/tree/main)）。

### ✨ 核心特性

- 🎙️ **语音唤醒与对话** — 支持唤醒词检测、ASR 语音识别、WebSocket 实时对话
- 🌐 **Web 控制面板** — 通过浏览器管理设备，支持 WiFi 和 USB 两种连接方式
- 🔄 **热更新** — SIGUSR2 + execvp 机制，无需重启设备即可更新助手，PID 不变
- 🛡️ **安全回退** — 内置开机看门狗，连续启动失败自动回退到原版固件
- 📊 **实时监控** — 设备状态、日志、配置一览无余
- ⚙️ **运行时配置** — WebSocket 地址、超时参数、日志级别等均可在线调整
- 🔌 **文件 IPC** — assistant 与 xwebd 通过文件系统通信，避免 TCP 开销
- 🔗 **MCP 接入点** — 支持配置 xiaozhi.me 智能体专属 MCP 端点，实现工具调用能力扩展
- 🧪 **自检诊断** — 分层自检架构，部署前验证环境兼容性

---

## 🏗️ 项目组成与架构

### 三模块架构

| 组件 | 语言 | 运行平台 | 说明 |
|------|------|----------|------|
| **assistant** (sair) | C | 设备端 | 语音助手模块，负责唤醒词检测、ASR、WebSocket 通信、状态管理、TTS 播放 |
| **xwebd** | C | 设备端 | Web 控制守护进程，提供 HTTP API 供 Panel 通信，管理设备端文件、音量、亮度等 |
| **panel** | Python | PC 端 | 控制面板，Web 界面统一管理设备，替代各种零散的调试/部署脚本 |

### 通信架构

```
┌─────────────┐     HTTP      ┌─────────────┐    文件IPC     ┌──────────────┐
│   Panel     │ ──────────→  │   xwebd     │ ──────────→  │  assistant   │
│  (PC:3000)  │ ←────────── │ (设备:8080)  │ ←──────────  │   (sair)     │
│  浏览器界面  │     JSON     │  HTTP API   │  sair_cmd.json │  语音助手    │
└─────────────┘              └─────────────┘  sair_status.json└──────────────┘
                                              sair_config.json
```

### assistant 内部架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        assistant (sair)                         │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ 状态机    │  │ 唤醒模块  │  │ 音频调度  │  │ 协议处理器    │   │
│  │ 6状态     │  │ ASR引擎   │  │ 3通道录音 │  │ WebSocket    │   │
│  │ 线程安全  │  │ 唤醒词检测│  │ TTS播放   │  │ Opus编解码   │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘   │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ 配置管理  │  │ API服务   │  │ 诊断模块  │  │ 看门狗       │   │
│  │ OTA激活   │  │ 文件IPC   │  │ 4项检查   │  │ 软看门狗续命 │   │
│  │ WiFi检查  │  │ set_config│  │ 运行时自检│  │ 崩溃保护     │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘   │
│                                                                 │
│  ┌──────────┐  ┌──────────┐                                    │
│  │ 触摸按键  │  │ MCP处理器 │                                    │
│  │ HOME/BACK │  │ 工具调用  │                                    │
│  └──────────┘  └──────────┘                                    │
└─────────────────────────────────────────────────────────────────┘
```

### 状态机

```
Starting → Activating → Idle → Connecting → Listening → Speaking
                          ↑         ↓            ↓           ↓
                          └─── Cleaning ←─────────┴───────────┘
```

| 状态 | 说明 |
|------|------|
| Starting | 初始化阶段，加载库和配置 |
| Activating | 等待设备激活（OTA 认证） |
| Idle | 等待唤醒，唤醒检测运行中 |
| Connecting | 建立 WebSocket 连接 |
| Listening | 监听用户语音 |
| Speaking | 播放 TTS 语音 |
| Cleaning | 会话清理，准备回到 Idle |

---

## 📱 支持设备

### GS705B 早教机器人

| 项目 | 规格 |
|------|------|
| **SoC** | ACTIONS OWL (ARM Cortex-A5 四核 900MHz) |
| **FPU** | VFPv4 + NEON + VFPd32 (硬件浮点，hard-float ABI) |
| **内存** | ~56MB (MemTotal: 57704 kB) |
| **存储** | NAND Flash，9.8MB 可写分区 (/var/upgrade, vfat) |
| **音频** | ALSA 驱动 (s900_link/ATC2603C)，2 麦克风 + 扬声器 |
| **网络** | WiFi (RTL8189FTV) |
| **C 库** | uClibc 0.9.33.2 |
| **内核** | Linux 3.10.52 |
| **浮点 ABI** | hard-float (ELF Flags: 0x5000402) |
| **Shell** | BusyBox v1.23.2 (ash) |

### 关键约束

| 约束 | 详情 | 解决方案 |
|------|------|----------|
| uClibc vs glibc | 动态链接 glibc 程序无法运行 | 使用 uClibc 工具链编译 |
| 浮点 ABI 不匹配 | 设备原生库为 hard-float，soft-float 程序无法调用其浮点函数 | 使用 soft-float 工具链 + FIXED_POINT=1 规避 |
| 内存紧张 | 总 56MB，sair VmSize~38MB，可用约 14MB | 注意内存使用 |
| 存储有限 | /var/upgrade 仅 9.8MB | 定期清理旧文件 |
| vfat 不支持符号链接 | /var/upgrade 是 vfat 分区 | 使用 cp 而非 ln -s |
| malloc 全局锁非递归 | uClibc 的 `__malloc_lock` 不是递归锁 | 子线程中禁止 cJSON_PrintUnformatted |

### 设备进程架构

```
init (PID 1)
  └─ manager (PID 140) ← 主控进程，启动和管理所有子进程
       ├─ launcher           - UI 显示、表情动画
       ├─ audio_service      - 音频服务（独占 ALSA）
       ├─ msg_server         - 消息转发
       ├─ sair               - 语音助手（本项目替换此进程）
       ├─ smart_player       - 多源音频控制器
       ├─ music_player       - 音乐播放
       ├─ wifiNetd           - WiFi 网络
       └─ ...
```

### 文件系统布局

| 分区 | 挂载点 | 大小 | 类型 | 说明 |
|------|--------|------|------|------|
| /dev/nand0p4 | / | 17.8MB | squashfs | 根文件系统（只读） |
| /dev/nand0p5 | /var/upgrade | 9.8MB | vfat | 用户数据分区（可写） |
| tmpfs | /tmp | 27.2MB | tmpfs | 临时文件（内存） |
| tmpfs | /dev/shm | 27.2MB | tmpfs | 共享内存（内存） |

---

## 🔨 编译

### 环境要求

- **Linux 环境**（WSL 即可，Windows 下通过 `wsl -e bash -c "..."` 调用）
- **ARM soft-float uClibc 交叉编译工具链**

### 获取工具链

从 [ChrisTheCoolHut/uClibc-Cross-Compilers](https://github.com/ChrisTheCoolHut/uClibc-Cross-Compilers) 下载 `arm-buildroot-linux-uclibcgnueabi_sdk-buildroot.tar.xz`（约 54MB），解压到项目根目录的 `toolchain/` 并重定位：

```bash
mkdir -p toolchain
tar -xf arm-buildroot-linux-uclibcgnueabi_sdk-buildroot.tar.xz -C toolchain/
toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/relocate-sdk.sh
```

完成后目录结构：

```
toolchain/
└── arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/
    ├── bin/
    │   └── arm-buildroot-linux-uclibcgnueabi-gcc    # 交叉编译器
    ├── arm-buildroot-linux-uclibcgnueabi/
    │   └── sysroot/                                  # 系统根目录
    └── relocate-sdk.sh
```

> ⚠️ `relocate-sdk.sh` 必须执行！它会将工具链内硬编码的绝对路径更新为当前解压位置。

### 设备原生库

assistant 运行时依赖设备固件自带的闭源动态库，由设备原生系统提供，无需手动安装：

| 库 | 来源 | 用途 |
|----|------|------|
| libapplib.so | /usr/lib | 应用框架（进程注册、消息分发、看门狗） |
| libapconfig.so | /usr/lib | 配置管理 |
| libconfigpart.so | /usr/lib | 配置分区 |
| libaudio_service_api.so | /usr/lib | 音频服务 API |
| libaudio_recorder.so | /usr/lib | 音频录制 |
| libdds.so | /usr/lib | DDS 消息服务（ASR 引擎依赖） |
| libsair_asr.so | /usr/lib | ASR 引擎封装（运行时 dlopen 加载） |

编译时 `build.sh` 会自动检测这些库是否存在于 sysroot 中。若不存在（全新工具链的默认情况），会自动创建空 stub `.so` 文件满足链接需求，配合 `-Wl,--unresolved-symbols=ignore-all` 跳过未定义符号检查。运行时由设备动态链接器加载真实实现。

### 编译 assistant

```bash
# Linux
cd device/assistant && bash build.sh

# Windows (WSL)
wsl -e bash -c "cd /mnt/d/小智ai/xiaozhi-zhiban-develop/device/assistant && bash build.sh"
```

编译产物：`device/assistant/build/sair`

### 编译 xwebd

```bash
# Linux
cd device/xwebd && bash build.sh

# Windows (WSL)
wsl -e bash -c "cd /mnt/d/小智ai/xiaozhi-zhiban-develop/device/xwebd && bash build.sh"
```

编译产物：`device/xwebd/build/xwebd`

> 💡 build.sh 默认从 `$PROJECT_DIR/../../toolchain/` 查找工具链，也可通过 `SDK_PATH` 环境变量指定：
> ```bash
> SDK_PATH=/path/to/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot bash build.sh
> ```

### 关键编译参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 工具链 | `arm-buildroot-linux-uclibcgnueabi-gcc` 7.4.0 | soft-float ABI |
| CPU | `-mcpu=cortex-a5 -mfloat-abi=soft` | Cortex-A5 soft-float |
| Opus | `-DFIXED_POINT=1` | **必须**！soft-float ABI 下调用 hard-float libm 会 SIGFPE |
| assistant 链接 | `-rdynamic` | 导出符号给 dlopen 使用 |
| 未定义符号 | `--unresolved-symbols=ignore-all` | 设备原生库运行时提供 |
| 静态库 | mbedtls, mbedx509, mbedcrypto, opus, cJSON | soft-float 预编译版，位于 `lib_sf/` |
| 动态库 | applib, apconfig, configpart, audio_service_api, audio_recorder, dds | 编译时 stub，运行时设备提供 |

---

## 🚀 部署

### 推荐方式：通过 Panel 控制面板

```bash
cd panel
python control_panel.py
```

浏览器打开 `http://localhost:3000`，在面板中完成设备连接和固件部署。

Panel 支持两种连接方式：
- **USB 连接（有线模式）** — 通过 ADB 端口转发，适合首次部署
- **WiFi 连接（无线模式）** — 通过 xwebd HTTP API，功能更完整

### 热更新（无需重启设备）

```bash
# 一键编译+上传+热更新
python scripts/transfer/hot_update.py

# 跳过编译，仅上传+热更新
python scripts/transfer/hot_update.py --no-build
```

### 冷更新（需重启设备）

```bash
python scripts/transfer/upload_nc_win.py device/assistant/build/sair /var/upgrade/sair
python scripts/debug/force_reboot.py
```

### 热更新原理

```
SIGUSR2 → 信号处理器中:
  1. stat("/var/upgrade/sair_new") 确认新文件存在
  2. rename sair→sair_old, sair_new→sair
  3. execvp("/var/upgrade/sair") 替换进程（PID不变）
→ 新进程启动:
  1. 检测热更新标记（app_running_list 中 pid==getpid() 的 sair 条目）
  2. 清理 IPC 资源（sync_shm, /tmp/service/sair）
  3. applib_init → 正常启动
```

**为什么不用 kill + restart**：kill 后 PID 变化 → Manager 检测 WIFSIGNALED → reboot；SCHED_RR 调度丢失；kill 到新进程启动有间隙 → 看门狗超时。

> ⚠️ **版本号必须更新**：热更新后 PID 不变（execvp 特性），Panel 通过版本号变化判断更新是否成功。每次发布新版本前，**务必修改** `device/assistant/include/xiaozhi_config.h` 中的 `XIAOZHI_VERSION` 宏，否则 Panel 无法检测到热更新成功。

---

## 📂 项目结构

```
xiaozhi-zhiban-develop/
├── device/
│   ├── assistant/                    # 语音助手模块
│   │   ├── src/                      # C 源码
│   │   │   ├── main.c               # 主程序（状态机、事件循环、IPC）
│   │   │   ├── state_machine.c      # 状态机实现
│   │   │   ├── wakeup_module.c      # 唤醒词检测模块
│   │   │   ├── audio_dispatcher.c   # 音频分发（3通道→ASR引擎）
│   │   │   ├── audio_player.c       # TTS 播放（audio_track API）
│   │   │   ├── audio_recorder.c     # 音频录制（audio_recorder API）
│   │   │   ├── protocol_handler.c   # WebSocket 协议处理
│   │   │   ├── websocket.c          # WebSocket 客户端
│   │   │   ├── tls_transport.c      # TLS 传输层（mbedtls）
│   │   │   ├── http_client.c        # HTTP 客户端（OTA 激活）
│   │   │   ├── config_manager.c     # 配置管理
│   │   │   ├── api_server.c         # 文件 IPC API 服务
│   │   │   ├── diag_module.c        # 诊断自检模块
│   │   │   ├── mcp_handler.c        # MCP 工具调用处理器
│   │   │   ├── touch_key.c          # 触摸按键驱动
│   │   │   ├── watchdog.c           # 看门狗管理
│   │   │   └── plog.c               # 持久化日志
│   │   ├── include/                  # 头文件
│   │   │   ├── app_context.h        # 应用上下文（全局状态）
│   │   │   ├── xiaozhi_config.h     # 配置常量（超时、消息ID等）
│   │   │   ├── state_machine.h      # 状态机接口
│   │   │   ├── wakeup_module.h      # 唤醒模块接口
│   │   │   ├── websocket.h          # WebSocket 接口
│   │   │   ├── protocol_handler.h   # 协议处理器接口
│   │   │   ├── audio_player.h       # 音频播放接口
│   │   │   ├── audio_recorder.h     # 音频录制接口
│   │   │   ├── audio_dispatcher.h   # 音频分发接口
│   │   │   ├── config_manager.h     # 配置管理接口
│   │   │   ├── api_server.h         # API 服务接口
│   │   │   ├── diag_module.h        # 诊断模块接口
│   │   │   ├── mcp_handler.h        # MCP 处理器接口
│   │   │   ├── touch_key.h          # 触摸按键接口
│   │   │   ├── watchdog.h           # 看门狗接口
│   │   │   ├── tls_transport.h      # TLS 传输接口
│   │   │   ├── http_client.h        # HTTP 客户端接口
│   │   │   ├── plog.h               # 日志接口
│   │   │   └── reverse/             # 逆向还原的设备原生 API 头文件
│   │   │       ├── applib_api.h     # applib 框架 API
│   │   │       ├── audio_recorder_api.h  # 音频录制 API
│   │   │       ├── audio_service_api.h   # 音频服务 API
│   │   │       └── sair_asr_api.h        # ASR 引擎 API
│   │   ├── lib/                      # 第三方库头文件
│   │   │   ├── cJSON/               # cJSON 库
│   │   │   ├── mbedtls/             # mbedTLS 库
│   │   │   └── opus/                # Opus 编解码库
│   │   ├── lib_sf/                   # soft-float 预编译静态库
│   │   │   ├── libcjson.a
│   │   │   ├── libmbedtls.a
│   │   │   ├── libmbedx509.a
│   │   │   ├── libmbedcrypto.a
│   │   │   └── libopus.a
│   │   ├── prebuilt/                 # 预编译二进制（供 main 分支同步）
│   │   └── build.sh                  # 编译脚本
│   └── xwebd/                        # Web 控制守护进程
│       ├── src/
│       │   └── xwebd.c              # xwebd 完整实现（单文件，~2000行）
│       ├── include/
│       │   └── xwebd_config.h       # 配置常量
│       ├── scripts/
│       │   └── boot_watchdog.sh     # 开机看门狗脚本
│       ├── prebuilt/
│       └── build.sh
├── panel/                            # PC 端控制面板
│   ├── control_panel.py             # 启动入口
│   ├── server.py                    # HTTP 后端服务
│   ├── device_api.py                # xwebd API 客户端
│   ├── adb_manager.py               # ADB 设备管理
│   ├── config.py                    # 配置（环境变量读取）
│   ├── log_config.py                # 日志配置
│   └── static/
│       ├── index.html               # 前端页面
│       ├── app.js                   # 前端逻辑
│       └── style.css                # 前端样式
├── scripts/                          # 开发调试脚本
│   ├── config_loader.py             # 项目参数加载模块
│   ├── transfer/                    # 文件传输工具
│   │   ├── hot_update.py            # 热更新（编译+上传+SIGUSR2）
│   │   ├── upload_nc_win.py         # NC方式上传文件
│   │   └── download_nc_win.py       # NC方式下载文件
│   ├── debug/                       # 调试工具
│   │   ├── force_reboot.py          # 强制重启设备
│   │   └── high_freq_monitor.py     # 高频监控设备状态
│   └── device_check/                # 设备检查工具
│       ├── emergency_fix.py         # 紧急修复循环重启
│       ├── diagnose_device.py       # 设备全面诊断
│       ├── diagnose_network.py      # 网络全面诊断
│       ├── restart_sair.py          # 重启sair服务
│       └── check_status_quick.py    # 快速状态检查
├── project_config.json              # 项目参数配置（设备IP/端口等）
├── toolchain/                        # 交叉编译工具链（需自行下载）
├── LICENSE
└── README.md
```

---

## 🔌 IPC 机制

### assistant ↔ xwebd 文件 IPC

assistant 与 xwebd 通过 `/tmp/` 下的 JSON 文件通信，避免 TCP 开销：

| 文件 | 方向 | 用途 |
|------|------|------|
| `/tmp/sair_status.json` | assistant → xwebd | 状态输出（state, version, activation_code） |
| `/tmp/sair_config.json` | assistant → xwebd | 配置输出（ws_url, ws_token, log_level, 超时参数, mcp_endpoint） |
| `/tmp/sair_cmd.json` | xwebd → assistant | 命令输入（set_config, wakeup, abort, activate） |
| `/tmp/sair_diag_request` | xwebd → assistant | 自检触发（空文件，创建即触发） |
| `/tmp/sair_diag.json` | assistant → xwebd | 自检结果 |

**通信流程**：xwebd 写入命令文件 → 发送 SIGUSR1 唤醒 assistant 主循环 → assistant 读取并执行命令 → 写入结果文件

### assistant ↔ 平台 SDK IPC

assistant 通过 applib 框架与设备其他进程通信：

| 机制 | 说明 |
|------|------|
| `get_msg()` / `dispatch_msg()` | 主消息循环，接收系统和业务消息 |
| `register_srv_dispatcher()` | 注册服务消息分发器 |
| `register_sys_dispatcher()` | 注册系统消息分发器 |
| `broadcast_msg()` | 广播消息（唤醒、会话结束、表情等） |

### 关键消息 ID

| ID | 宏名 | 方向 | 说明 |
|----|------|------|------|
| 0x235 | MSG_SAIR_AWAKE | sair → launcher | 唤醒（无角度） |
| 0x239 | MSG_SAIR_AWAKE_CMD | sair → launcher | 唤醒（带角度） |
| 0x236 | MSG_SAIR_EMOTION | sair → launcher | 表情动画 |
| 0x238 | MSG_SAIR_END | sair → launcher | 会话结束 |
| 0x23E | MSG_SAIR_ENABLE | 系统 → sair | 启用唤醒 |
| 0x23F | MSG_SAIR_DISABLE | 系统 → sair | 禁用唤醒 |
| 0x040 | MSG_KEY_HOME | 按键 → sair | HOME 键 (code=102) |
| 0x041 | MSG_KEY_BACK | 按键 → sair | BACK 键 (code=30) |

> ⚠️ `broadcast_msg` 消息格式是 `int msg[N]`，不是 `applib_msg_t` 结构体！`msg[0]` 就是消息 ID。

---

## ⚙️ 配置系统

### 运行时可调配置

通过 Panel 或 xwebd API 的 `set_config` 命令修改，无需重启：

| 配置项 | 默认值 | 范围 | 说明 |
|--------|--------|------|------|
| ws_url | wss://api.tenclass.net/xiaozhi/v1/ | 任意 URL | WebSocket 服务地址 |
| ws_token | (空) | 任意字符串 | WebSocket 认证令牌 |
| log_level | INFO | DEBUG/INFO/WARN/ERROR | 日志级别 |
| listen_timeout | 120000 | 10000-300000 ms | 监听超时 |
| session_timeout | 300000 | 30000-600000 ms | 会话超时 |
| wakeup_cooldown | 3000 | 500-10000 ms | 唤醒冷却时间 |
| ws_ping_interval | 25000 | 5000-120000 ms | WebSocket 心跳间隔 |
| mcp_endpoint | (空) | 任意 URL | MCP 接入点地址（从 xiaozhi.me 控制台获取的智能体专属端点） |

### 编译时配置

定义在 `device/assistant/include/xiaozhi_config.h`：

| 宏 | 值 | 说明 |
|----|-----|------|
| XIAOZHI_VERSION | "2.0.0" | 版本号 |
| DEFAULT_OTA_URL | https://api.tenclass.net/xiaozhi/ota/ | OTA 激活地址 |
| DEFAULT_WS_URL | wss://api.tenclass.net/xiaozhi/v1/ | WebSocket 地址 |
| GOODIX_KEY_HOME | 102 | 触摸屏 HOME 键码 |
| GOODIX_KEY_BACK | 30 | 触摸屏 BACK 键码 |

---

## 🛡️ 安全机制

### 开机看门狗 (boot_watchdog.sh)

部署到设备后，`boot_watchdog.sh` 在每次开机时自动检测启动是否成功：

1. 设备启动后 120 秒内创建「存活标记」
2. 如果重启时存活标记不存在，说明上次启动短命，计数器 +1
3. 连续 3 次短命启动 → 自动删除 `/var/upgrade/sair`，回退到原版固件
4. 正常启动 → 计数器清零

**不依赖系统时钟**：使用文件存在性而非时间戳判断，避免设备无 RTC 时的问题。

### 看门狗续命

assistant 通过 applib 框架的 `get_msg()` 循环自动续命（每 ~16.7 秒）。超时 50 秒未续命 → Manager 执行 `reboot`。

**调试时防重启**：
```bash
/var/upgrade/watchdog_guard.sh daemon   # 守护模式（推荐）
/var/upgrade/watchdog_guard.sh enable   # 一次性禁止
/var/upgrade/watchdog_guard.sh disable  # 恢复看门狗
```

### 紧急恢复

```bash
# 自动恢复（boot_watchdog.sh，设备级）
# 连续3次短命启动自动回退

# PC端脚本修复
python scripts/device_check/emergency_fix.py          # 自适应模式
python scripts/device_check/emergency_fix.py --fast   # 快速重启专用
python scripts/device_check/emergency_fix.py --slow   # 慢速重启专用

# 手动修复
adb shell "rm -f /var/upgrade/sair; reboot"
```

---

## 🔧 开发注意事项

### applib 框架

| 注意事项 | 详情 |
|----------|------|
| 函数名是 dispatcher 不是 dispatch | `register_srv_dispatcher()`, `register_sys_dispatcher()` |
| 参数是函数指针，不是字符串 | `register_srv_dispatcher(proc_srv_msg)` |
| applib_init 会覆盖信号处理 | 必须在 `applib_init` 之后用 `sigaction()` 重新注册所有信号 |
| uClibc 的 signal() 设置 SA_RESTART | 需要中断阻塞系统调用的信号必须用 `sigaction()` 直接注册，`sa_flags=0` |
| applib_init 快速路径问题 | `global_init_flag!=0` 时只做 strdup 返回 1，不设置 `g_this_app_info`。必须清理 `/dev/shm/` 确保走完整初始化 |
| config_so_init 不应单独调用 | `applib_init` 内部已通过 `applib_dir_cfg_init` 调用 |
| 必须用 get_config(key,value,size) | 不能用 `applib_dir_cfg_read`，后者参数是结构体指针 |

### 线程安全

| 注意事项 | 详情 |
|----------|------|
| cJSON_PrintUnformatted 在子线程中会卡死 | uClibc 的 `__malloc_lock` 不是递归锁，子线程调用可能死锁。用手动构建 JSON 字符串替代 |
| WebSocket 重配必须在主线程执行 | 子线程不能直接调用 `websocket_protocol_init`（会 memset 清零），只设标志位 |
| 状态机 mutex 必须覆盖 current_state 读写 | 保护范围必须包含状态变更 + 回调通知整个过程 |
| ws_send_frame 内不能调用 malloc | ws->mutex 锁内 malloc 可能与其他线程竞争 `__malloc_lock` 导致死锁，用栈缓冲区 |
| get_msg() 内部 select() 有 1 秒超时 | 被信号中断后 EINTR 返回 0（不重试），加上 1 秒超时保证主循环至少每秒被唤醒 |
| 使用 SIGUSR1 唤醒主线程 | applib 的 SIGUSR1 处理函数会写入 `g_notify_pipe` 唤醒 `select()` |

### 音频系统

| 注意事项 | 详情 |
|----------|------|
| 禁止直接访问 ALSA 设备 | 与 audio_service 冲突，必须通过 audio_recorder/audio_track API |
| TTS 播放使用 audio_track API | 流式播放：每收到一帧 Opus → 解码 → S16 直接 cast 转 S32 → `audio_track_write_data` |
| S16→S32 转换方式 | 直接 cast + rate=24000 → audio_service 内部重采样到 48kHz，音质正常 ✅ |
| shift_bits=0 表示不位移 | audio_track 直接将收到的 S32 值送往 DAC，左移 16 位会放大 65536 倍导致噪音 ❌ |
| audio_track_set_volume 范围 0-80 | 不是 0-100，超过 80 返回错误 |
| audio_get_volume 会导致崩溃 | 不能调用，audio_service 内部已根据系统设置控制音量 |
| 录音格式 | S32_LE / 16kHz / 2ch，audio_service 内部做 32→16bit 转换 |
| feed_data 传 3 通道原始数据 | 不做单声道提取，duilite 引擎内部做 AEC 和波束成形 |

### ASR / 唤醒词

| 注意事项 | 详情 |
|----------|------|
| asr_engine_set_params 注册回调 | 不是 `asr_config_t.callback`！调用 `asr_engine_set_params(0, 4, callback)` |
| ASR 回调签名只有 2 个参数 | `void callback(int event_type, int result)` |
| ASR 事件类型 | 0=init_done, 2=wakeup, 3=diff_word, 4=vad_change, 5=vad_end, 6=vad_timeout |
| VAD/AEC/FFVP 库是纯存根 | `libsair_vad.so`/`libsair_aec.so`/`libsair_ffvp.so` 仅 2 条 ARM 指令，实际功能在 `libduilite_fespl.so` 内部 |
| dump_open/dump_close/dump_write | `libsair_asr.so` 通过 PLT 引用，需在主程序提供 stub 实现 |

### 看门狗

| 注意事项 | 详情 |
|----------|------|
| applib_init 设置 50 秒超时 | `get_msg()` 循环中 `handle_timers()` 自动续命 |
| 主线程在 get_msg 外长时间操作 → 续命中断 → 超时 → reboot | |
| check_soft_watchdogs 每 10 秒调用 | 超时后 coredump_enabled → kill(pid,SIGSEGV)，否则 → system("reboot") |
| g_this_app_info 指针算术必须用 (char*) 转换 | 否则按 uint64_t* 偏移会写入错误位置 |
| 看门狗共享内存写入后需内存屏障 | ARM 存储可能停留在 store buffer 中，写入后调用 `__sync_synchronize()` |
| dlsym(RTLD_DEFAULT,...) 在 uClibc 下可能找不到已链接库的符号 | `sys_forbid_soft_watchdog` 等必须用 extern 声明直接调用 |

### 其他

| 注意事项 | 详情 |
|----------|------|
| LD_PRELOAD 绝对不能使用 | 无论 shell 还是 C 代码 setenv+execv，都会导致崩溃或影响其他进程 |
| opus_encode() 触发 SIGFPE | 根因是 soft-float 工具链编译的 Opus 调用 hard-float libm 函数 ABI 不兼容。必须用 FIXED_POINT=1 |
| g_this_app_info 必须在 sair 中定义 | stub libapplib.so 是空的，不提供该符号，运行时写入会 SIGSEGV |
| 看门狗函数用弱定义 | `set_soft_watchdog_timeout`/`sys_forbid_soft_watchdog` 等用 `__attribute__((weak))` 提供回退实现 |
| LED 名称是 led-power | /sys/class/leds/led-power/，最大亮度 1000（不是 255） |
| 亮度控制范围 0-900 | 通过 /sys/class/backlight/owl_backlight/brightness |
| 预设 WAV 文件为 32bit PCM | S32 数据直接传 audio_track_write_data，无需 S16→S32 转换 |
| plog() 持久化日志 | 写入 /var/upgrade/xiaozhi.log，每次 fsync，崩溃不丢日志 |
| 手动测试前需清理 /dev/shm/ | 否则 applib_init 报 errno=17 (EEXIST) |

---

## 🌐 云端服务

assistant 需要连接云端 API 完成设备激活和 WebSocket 通信。已认证的设备可直接使用，未认证设备需先完成激活流程。

默认云端地址定义在 `device/assistant/include/xiaozhi_config.h`：
- `DEFAULT_OTA_URL` — OTA 激活接口
- `DEFAULT_WS_URL` — WebSocket 通信接口

如需对接自建服务端，修改上述宏定义即可。

---

## 🔄 关于实时对话模式（Realtime Mode）

本项目使用 **AutoStop（自动停止）模式**：TTS 播放时停止向云端上传音频，TTS 播放结束后恢复上传。用户可以通过唤醒词打断当前播放。

在开发过程中，我们曾参考 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 实现了 Realtime（实时对话）模式，但最终移除了该功能。

### GS705B 设备的 AEC 困境

1. **无硬件 AEC**：ACTIONS OWL SoC 不像 ESP32 那样提供硬件 AEC 加速器
2. **无软件 AEC**：设备固件的闭源音频服务未暴露 AEC 接口
3. **云端 AEC 不可行**：参考信号时间同步精度不足，实测效果极差
4. **soft-float ABI 限制**：浮点运算性能极差，无法实时处理

对于 GS705B 这类无硬件 AEC 能力的嵌入式设备，AutoStop 模式是唯一可行的方案。

---

## 🔍 设备自检架构

设备自检遵循"谁部署谁检查"的原则：

| 检查类型 | 执行时机 | 执行者 | 检查方式 |
|----------|----------|--------|----------|
| xwebd 环境检查 | 部署 xwebd 前 | Panel（ADB 模式） | `adb shell` 命令 |
| xwebd 运行时检查 | xwebd 运行中 | xwebd 自身 | `/api/diag` |
| assistant 环境检查 | 部署 assistant 前 | xwebd | `/api/assistant/env` |
| assistant 运行时检查 | assistant 运行中 | assistant 自身 | `/api/assistant/diag` |

assistant 诊断模块保留 4 项独有检查：
1. 配置文件读写
2. 日志系统
3. 看门狗库访问
4. WebSocket 连接状态

---

## 🧪 xwebd HTTP API 参考

xwebd 监听设备 8080 端口，提供以下 API：

### 设备管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/services | 获取进程列表 |
| GET | /api/diag | 设备自检 |
| POST | /api/reboot | 重启设备 |
| GET | /api/wifi | WiFi 信息 |
| GET | /api/disk | 磁盘信息 |
| GET | /api/memory | 内存信息 |

### 音量/亮度

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/volume | 获取音量 |
| PUT | /api/volume | 设置音量 |
| GET | /api/brightness | 获取亮度 |
| PUT | /api/brightness | 设置亮度 |

### 文件管理

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/upload | 上传文件 |
| GET | /api/files | 文件列表 |
| DELETE | /api/files | 删除文件 |

### 助手管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/assistant/status | 助手状态 |
| GET | /api/assistant/config | 助手配置 |
| PUT | /api/assistant/config | 修改配置 |
| POST | /api/assistant/deploy | 部署助手（冷部署，设备重启） |
| POST | /api/assistant/update | 冷更新（设备重启） |
| POST | /api/assistant/upgrade | 热更新（SIGUSR2，不重启） |
| POST | /api/assistant/uninstall | 卸载助手 |
| POST | /api/assistant/activate | 激活设备 |
| GET | /api/assistant/env | 助手环境检查 |
| GET | /api/assistant/logs | 助手日志 |
| GET | /api/assistant/diag | 助手诊断 |

---

## 📊 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `XIAOZHI_DEVICE_HOST` | (空) | 设备 IP 地址，Panel 启动时自动使用 |
| `XIAOZHI_PANEL_PORT` | 3000 | Panel 监听端口 |
| `XIAOZHI_XWEBD_PORT` | 8080 | xwebd 服务端口 |
| `XIAOZHI_SAIR_API_PORT` | 8081 | assistant API 端口（旧版遗留，当前未使用） |
| `SDK_PATH` | `../../toolchain/...` | 交叉编译工具链路径 |

使用示例：
```bash
# Windows PowerShell
$env:XIAOZHI_DEVICE_HOST = "192.168.1.96"
python control_panel.py

# Linux/macOS
XIAOZHI_DEVICE_HOST=192.168.1.96 python control_panel.py

# 或通过命令行参数指定
python control_panel.py --device-host 192.168.1.96
```

---

## 📄 许可证

MIT License

Copyright (c) 2025 xiaozhi-zhiban contributors

本项目使用 MIT 许可证开源。请注意：
- 设备固件中的闭源动态库（libapplib.so、libduilite_fespl.so 等）不属于本项目，它们由设备原生系统提供
- 第三方静态库（mbedtls、opus、cJSON）各自遵循其原始许可证
