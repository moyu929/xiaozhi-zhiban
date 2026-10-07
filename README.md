# 小智·智伴 (xiaozhi-zhiban) — Develop

<p align="center">
  <strong>智伴教育机器人1X（GS705B）的开源语音助手替代方案 — 开发者版本，包含完整源码</strong>
</p>

> **致敬** — 本项目参考并参照复刻了 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 开源项目（[开发文档](https://my.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb) | [作者 B站](https://space.bilibili.com/59357679)），在此向原作者及社区致敬。

---

## 📖 项目简介

小智·智伴是一个为**智伴教育机器人1X**（内部型号 GS705B）开发的开源语音助手替代方案。本项目是 **开发版 (develop)** 分支，包含完整的 C 源代码、编译脚本和开发文档，供开发者二次开发。

如果你只想使用预编译版本部署到设备，请使用 [main 分支](https://github.com/moyu929/xiaozhi-zhiban/tree/main)（[Gitee 镜像](https://gitee.com/beichen929/xiaozhi-zhiban/tree/main)）。

### ✨ 核心特性

- 🎙️ **语音唤醒与对话** — 唤醒词检测、ASR 语音识别、WebSocket 实时对话，可连接豆包、DeepSeek 等 LLM 模型进行智能对话与意图识别
- 🌐 **Web 控制面板** — 通过浏览器管理设备，支持 WiFi 和 USB 两种连接方式
- 🔄 **热更新** — assistant 支持 cmd.json + SIGUSR1 + execvp 热更新（PID 不变，秒级完成），xwebd 支持冷更新（替换二进制+重启设备）
- 🛡️ **安全回退** — 内置开机看门狗，连续启动失败自动回退到原版固件
- 🚨 **紧急修复** — 设备频繁重启时，高频轮询检测上线立即恢复出厂
- 📊 **实时监控** — 设备状态、日志、配置一览无余
- ⚙️ **运行时配置** — WebSocket 地址、超时参数、日志级别等均可在线调整
- 🔌 **双协议栈** — 支持 WebSocket 和 MQTT+UDP 双协议栈，MQTT 用于控制信令，UDP 用于 AES-128-CTR 加密音频传输；当前仅 WebSocket 模式可用（MQTT+UDP 因官方网关兼容性暂不可用）；assistant 与 xwebd 通过文件 IPC 通信
- ⚡ **低功耗优化** — 动态 poll 超时（空闲200ms/活跃50ms）、发送线程低频唤醒、WebSocket 帧掩码零文件I/O、MQTT 栈缓冲区、线程栈精简，最大化 CPU 深睡眠时间
- 🔗 **MCP 接入点** — 支持配置 xiaozhi.me 智能体专属 MCP 端点，实现工具调用能力扩展
- 🔇 **本地回声消除** — 定点 NLMS AEC（纯整数运算），mic0+mic1 混合消参考后单通道上行，Realtime 模式不再依赖云端 AEC
- 💬 **屏幕字幕** — TTS 回复文本经原生 0x23A 消息推送，屏幕底部显示回复内容
- ⏱️ **每日使用时长** — 按 TTS 播放时长累计，触限后唤醒仅播提醒音、不连服务器（默认关闭）
- 🌙 **会话息屏** — 唤醒期间可配置 N 秒后息屏省电，触摸/按键点亮，语音对话不受影响（默认关闭）
- 🎬 **原生关机链** — MCP 关机走设备原生 MSG_SHORTCUT_POWER 动画链（带关机动画与提示音）
- 💡 **亮度持久化** — xwebd 可选记忆亮度设置，开机自动应用（默认关闭）
- 🧪 **自检诊断** — 分层自检架构，部署前验证环境兼容性

### ⚠️ 已知限制

- **唤醒词不可自定义** — 本项目使用设备原生唤醒词模型（libduilite_fespl.so），暂不支持更改唤醒词
- **Realtime 模式 AEC 为软件兜底** — 设备原生 AEC 闭源无法调用，采用自研定点 NLMS 兜底（详见下方说明），消回声效果依赖实机声学环境，不理想时可切回 AutoStop 模式

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
                                              ┌──────────────┐
                                              │   云端服务    │
                                              │ xiaozhi.me   │
                                              └──────┬───────┘
                                                     │
                                              ┌──────┴───────┐
                                              │  WebSocket   │  ← 协议栈1: WS+Opus
                                              │  MQTT+UDP    │  ← 协议栈2: MQTT信令+AES-128-CTR加密音频
                                              └──────────────┘
```

### assistant 内部架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        assistant (sair)                         │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ 状态机    │  │ 唤醒模块  │  │ 音频调度  │  │ 协议处理器    │   │
│  │ 6状态     │  │ ASR引擎   │  │ 3通道录音 │  │ WebSocket    │   │
│  │ 线程安全  │  │ 唤醒词检测│  │ TTS播放   │  │ MQTT+UDP     │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘   │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ 配置管理  │  │ API服务   │  │ 诊断模块  │  │ 看门狗       │   │
│  │ OTA激活   │  │ 文件IPC   │  │ 4项检查   │  │ 喂狗续命     │   │
│  │ WiFi检查  │  │ set_config│  │ 运行时自检│  │ 崩溃保护     │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘   │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                     │
│  │ 触摸按键  │  │ MCP处理器 │  │ 音频预缓存│                     │
│  │ HOME/BACK │  │ 工具调用  │  │ TTS预加载 │                     │
│  └──────────┘  └──────────┘  └──────────┘                     │
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

### 智伴教育机器人1X（内部型号 GS705B）

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

### 兼容性提示

> ⚠️ Windows 7 系统、或设备固件版本低于 **2.0.01.200907**（如 2.0.00.190315 等早期批次）时，有一定概率无法成功查找到设备或部署。Win7 请使用 platform-tools **34.0.4 及以下版本**（35+ 的 adb 无法在 Win7 启动）并优先 USB 2.0 端口；旧固件设备建议先通过官方途径升级固件。

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

# Windows (WSL) — 请将路径替换为你的实际项目路径
wsl -e bash -c "cd /mnt/d/your-path/xiaozhi-zhiban-develop/device/assistant && bash build.sh"
```

编译产物：`device/assistant/build/sair`

### 编译 xwebd

```bash
# Linux
cd device/xwebd && bash build.sh

# Windows (WSL) — 请将路径替换为你的实际项目路径
wsl -e bash -c "cd /mnt/d/your-path/xiaozhi-zhiban-develop/device/xwebd && bash build.sh"
```

编译产物：`device/xwebd/build/xwebd`

> 💡 build.sh 默认从 `$PROJECT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot` 查找工具链，也可通过 `SDK_PATH` 环境变量指定：
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

### 前提条件

运行 Panel 控制面板需要：

- **[Python 3.8+](https://www.python.org/downloads/)**
- **[ADB](https://developer.android.com/tools/releases/platform-tools)**（Android Debug Bridge，USB 有线模式需要）
  - Windows 直链：[platform-tools-latest-windows.zip](https://dl.google.com/android/repository/platform-tools-latest-windows.zip)
  - 加入 PATH 即可被自动识别；若不想改 PATH，也可把解压得到的 `platform-tools` 整个文件夹放到 `xiaozhi-zhiban-develop/` 目录下（即 `xiaozhi-zhiban-develop/platform-tools/adb.exe`）
  - **Windows 7 用户**：请使用 platform-tools **34.0.4 及以下版本**（35+ 的 adb 无法在 Win7 启动）；Win7 首次有线连接可能需手动安装设备 USB 驱动，并优先使用 USB 2.0 端口

### 推荐方式：通过 Panel 控制面板

```bash
cd panel
python control_panel.py
```

浏览器打开 `http://localhost:3000`，在面板中完成设备连接和固件部署。

Panel 支持两种连接方式：
- **USB 连接（有线模式）** — 通过 ADB 端口转发，适合首次部署
- **WiFi 连接（无线模式）** — 通过 xwebd HTTP API，功能更完整

> 💡 **ADB 识别不到设备？** 先重启 ADB 服务，再重新进行有线连接：
> ```bash
> adb kill-server
> adb start-server
> adb devices          # 确认列表中出现设备
> ```
> 若 `adb devices` 列表仍为空，检查数据线是否支持数据传输（非纯充电线），并重新拔插 USB。

> ⚠️ **部署顺序**：新设备**必须先通过有线模式安装面板内核（xwebd）**，才能使用无线模式连接。推荐安装顺序：**先安装 xwebd → 再安装语音助手（sair）**。卸载时反向操作：**先卸载 sair → 再卸载 xwebd**，或直接使用有线模式下的「恢复出厂设置」功能一键清除。

### 配置小智 AI

部署固件后，还需要在小智平台上完成设备绑定和智能体配置，语音助手才能正常使用：

1. **注册账号** — 访问 [xiaozhi.me](https://xiaozhi.me/)，注册并登录账号
2. **添加设备** — 在 Panel 面板的无线模式中查看设备激活码，然后在小智控制台中点击「添加设备」，输入激活码完成绑定，并**关闭 OTA 升级**（防止官方固件覆盖自定义助手）
3. **配置 MCP 接入点** — 在小智控制台中点击「配置智能体角色」→ 找到「MCP 设置」→ 点击「获取 MCP 接入点」，将提供的 URL 复制到 Panel 面板中「语音助手管理」区域的 MCP 接入点输入框中，点击「保存配置」

> 💡 完成以上配置后，说唤醒词即可开始与 AI 对话。MCP 接入点决定了智能体使用的 LLM 模型（如豆包、DeepSeek 等）和工具调用能力，可在小智控制台中随时切换。

### 更新方式

| 模块 | 热更新 | 冷更新 |
|------|--------|--------|
| assistant | ✅ cmd.json + SIGUSR1 + execvp（PID 不变，秒级完成） | ✅ 替换二进制 + reboot |
| xwebd | ❌ 不支持 | ✅ 替换二进制 + reboot（看门狗+worker 架构自动恢复） |

#### assistant 热更新

推荐通过 Panel 面板操作（无线模式 → 语音助手管理 → 热更新），也可手动通过 xwebd API 触发：

```bash
# 通过 xwebd API 触发热更新（需先上传新二进制到 /var/upgrade/sair_new）
curl -X POST http://DEVICE_IP:8080/api/assistant/upgrade
```

#### 冷更新

推荐通过 Panel 面板操作（有线/无线模式均可），也可手动执行：

```bash
# 手动重启设备
python scripts/debug/force_reboot.py
```

#### assistant 热更新原理

```
xwebd 收到 /api/assistant/upgrade:
  1. rename sair→sair_old, sair_new→sair
  2. 写入 /tmp/sair_cmd.json: {"cmd":"upgrade"}
  3. kill(sair_pid, SIGUSR1) 通知 assistant 主循环
assistant 主循环检测到 g_hot_update_pending:
  1. 优雅停止资源（唤醒、录音、播放器、协议连接）
  2. 删除 IPC 文件（sair_status.json 等）
  3. 关闭所有文件描述符
  4. execvp("/var/upgrade/sair") 替换进程（PID不变）
新进程启动:
  1. 检测热更新标记（app_running_list 中 pid==getpid() 的 sair 条目）
  2. 清理 IPC 资源（sync_shm, /tmp/service/sair）
  3. applib_init → 正常启动
```

**为什么用 cmd.json + SIGUSR1 而非 SIGUSR2**：applib 框架内部拦截了 SIGUSR2 信号，导致 SIGUSR2 处理器永远不会被调用。SIGUSR1 是 applib 自身也在使用的信号，不会被屏蔽，因此热更新复用 cmd.json + SIGUSR1 通路（与唤醒/中止/激活走同一路径）。

**为什么不用 kill + restart**：kill 后 PID 变化 → Manager 检测 WIFSIGNALED → reboot；SCHED_RR 调度丢失；kill 到新进程启动有间隙 → 看门狗超时。

> ⚠️ **版本号自动递增**：build.sh 编译时自动生成 `version.h`，通过 `.version` 文件管理版本号，采用进位逻辑（每位到10进位，如 2.1.9 → 2.2.0）。当前 assistant 版本 2.4.x，xwebd 版本 1.2.x。
>
> `.version` 文件需提交到仓库，确保不同环境编译时版本号一致。

---

## 📂 项目结构

```
xiaozhi-zhiban-develop/
├── device/
│   ├── assistant/                    # 语音助手模块
│   │   ├── src/                      # C 源码（24个.c文件）
│   │   ├── include/                  # 头文件
│   │   │   ├── reverse/             # 逆向还原的设备原生 API 头文件
│   │   │   │   ├── applib_api.h
│   │   │   │   ├── audio_recorder_api.h
│   │   │   │   ├── audio_service_api.h
│   │   │   │   └── sair_asr_api.h
│   │   │   └── (其他25个.h文件)
│   │   ├── lib/                      # 第三方库头文件
│   │   │   ├── cJSON/
│   │   │   ├── mbedtls/
│   │   │   └── opus/
│   │   ├── lib_sf/                   # soft-float 预编译静态库
│   │   │   ├── libcjson.a
│   │   │   ├── libmbedtls.a
│   │   │   ├── libmbedx509.a
│   │   │   ├── libmbedcrypto.a
│   │   │   └── libopus.a
│   │   ├── prebuilt/                 # 预编译二进制（供 main 分支同步）
│   │   │   ├── sair
│   │   │   └── version.h
│   │   └── build.sh                  # 编译脚本（自动生成 version.h）
│   └── xwebd/                        # Web 控制守护进程
│       ├── src/
│       │   ├── xwebd.c
│       │   └── usb_helper.c
│       ├── include/
│       │   └── xwebd_config.h
│       ├── scripts/
│       │   ├── boot_watchdog.sh
│       │   └── watchdog_guard.sh
│       ├── prebuilt/
│       │   ├── xwebd
│       │   └── version.h
│       ├── build.sh
│       └── build_helper.sh
├── panel/                            # PC 端控制面板
│   ├── control_panel.py
│   ├── server.py
│   ├── device_api.py
│   ├── adb_manager.py
│   ├── config.py
│   ├── log_config.py
│   └── static/
│       ├── index.html
│       ├── app.js
│       └── style.css
├── scripts/                          # 开发调试脚本
│   ├── config_loader.py             # 项目参数加载模块
│   ├── debug/                       # 调试工具
│   │   ├── force_reboot.py          # 强制重启设备
│   │   ├── high_freq_monitor.py     # 高频监控设备状态
│   │   └── monitor_keys.sh          # 按键监测（设备端运行）
│   └── device_check/                # 设备检查工具
│       ├── emergency_fix.py         # 紧急修复循环重启
│       └── emergency_nuke.py        # 紧急恢复出厂
├── project_config.example.json       # 项目参数配置模板
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
| `/tmp/sair_cmd.json` | xwebd → assistant | 命令输入（set_config, wakeup, abort, activate, upgrade） |
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
| 0x23A | (字幕推送) | sair → wiki场景 | TTS 回复字幕（buf[0]=序号, +4=文本） |
| 0x232 | (字幕清屏) | sair → wiki场景 | 清除屏幕字幕 |
| 63 | MSG_SHORTCUT_POWER | 系统 → 全体 | 原生关机链（子码 200，经 set_timed_shutdown_time 触发，播放关机动画） |

> ⚠️ `broadcast_msg` 消息格式是 `int msg[N]`，不是 `applib_msg_t` 结构体！`msg[0]` 就是消息 ID。

---

## ⚙️ 配置系统

### 工具脚本配置（project_config.json）

`scripts/` 目录下的工具脚本通过 `config_loader.py` 读取项目根目录的 `project_config.json` 获取设备连接参数。

**首次使用前**，需要从模板创建配置文件：

```bash
cp project_config.example.json project_config.json
```

然后修改 `project_config.json` 中的设备IP等参数：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| device_ip | 192.168.1.1 | 设备局域网IP地址 |
| device_telnet_port | 23 | 设备Telnet端口 |

> `project_config.json` 已加入 `.gitignore`，不会被提交到仓库。如果文件不存在，脚本会自动使用 `project_config.example.json` 中的示例值并给出提示。

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
| transport_mode | 0 | 0=WebSocket, 1=MQTT+UDP | 传输模式（当前仅 WebSocket 可用） |
| listening_mode | autostop | realtime/autostop | 监听模式（默认 AutoStop，本地 AEC 效果有限时推荐；持久化于 /var/upgrade/.listening_mode） |
| custom_ws_url | (空) | 任意 URL | 自定义 WebSocket 地址 |

#### 设备侧配置（config.bin，经 MCP 工具修改或直改后重启）

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| USE_LIMIT_ENABLE | 0 | 每日使用时长限制开关 |
| USE_LIMIT_MINUTES | 60 | 每日可用分钟数（按 TTS 播放时长累计） |
| USE_LIMIT_PROMPT_ID | 14 | 触限提醒音 id（暂用设备已有音频占位） |
| USE_LIMIT_SPENT_SEC / USE_LIMIT_DAY | 0 / 当日 | 内部累计状态（勿手改） |
| SCREEN_OFF_IDLE_SEC | 0 | 唤醒期间息屏秒数（0=不息屏，触摸/按键点亮） |

### 编译时配置

定义在 `device/assistant/include/xiaozhi_config.h`：

| 宏 | 值 | 说明 |
|----|-----|------|
| XIAOZHI_VERSION | 自动生成 | 版本号（由 build.sh 从 version.h 注入，格式 major.minor.patch，如 2.1.1） |
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

### 🚨 紧急修复

当设备因自定义程序崩溃导致**频繁重启**时，常规手段难以操作（设备刚上线就又重启）。紧急修复功能通过**高频轮询**设备连接状态，一旦检测到设备上线，**立即执行恢复出厂设置**。

**使用场景**：设备不断重启、卡死无法正常操作时。

**使用方式**：

1. **Panel 控制面板**（推荐）：
   - 有线模式：点击「🚨 紧急修复」按钮
   - 无线模式：点击「🚨 紧急修复」按钮
   - 需二次确认，确认后自动高频轮询，检测到设备立即清理

2. **命令行脚本**：
   ```bash
   python scripts/device_check/emergency_nuke.py --adb          # ADB模式（推荐，更快）
   python scripts/device_check/emergency_nuke.py --http IP      # HTTP模式
   python scripts/device_check/emergency_nuke.py --auto IP      # 自动模式（先ADB后HTTP）
   ```

**原理**：
- ADB 模式：以 0.3 秒间隔轮询 `adb devices`，检测到设备后立即执行 `killall` + `rm` 清理所有自定义文件
- HTTP 模式：以 0.5 秒间隔轮询 xwebd API，检测到设备后依次调用卸载接口

> ⚠️ **重要**：此操作将删除设备上所有自定义程序（sair/xwebd）及相关配置，恢复到原生状态，不可撤销。

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
| 录音格式 | S32_LE / 16kHz / 3ch 交织（ch0/ch1=双麦，ch2=扬声器参考），audio_service 内部做 32→16bit 转换 |
| feed_data 传 3 通道原始数据 | 唤醒检测路径保持原生（duilite 内部波束成形）；ASR 上行链另行处理：mic0+mic1 混合 → 本地 NLMS 消参考（audioproc.c）→ 干净单声道供 Opus 编码 |

### ASR / 唤醒词

| 注意事项 | 详情 |
|----------|------|
| 唤醒词不可自定义 | 使用设备原生唤醒词模型（libduilite_fespl.so），暂不支持更改 |
| asr_engine_set_params 注册回调 | 不是 `asr_config_t.callback`！调用 `asr_engine_set_params(0, 4, callback)` |
| ASR 回调签名只有 2 个参数 | `void callback(int event_type, int result)` |
| ASR 事件类型 | 0=init_done, 2=wakeup, 3=diff_word, 4=vad_change, 5=vad_end, 6=vad_timeout |
| VAD/AEC/FFVP 库是纯存根 | `libsair_vad.so`/`libsair_aec.so`/`libsair_ffvp.so` 仅 2 条 ARM 指令，实际功能在 `libduilite_fespl.so` 内部 |
| AEC 模型存在但闭源 | 设备有 AEC 模型文件（`AEC_ch3-2-ch2_1ref_common_*.bin`），但封装在闭源引擎内，无法直接调用 |
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
| 亮度控制经 owl_backlight sysfs | /sys/class/backlight/owl_backlight/brightness，实测面板值域 >255（读到过 400）；xwebd /api/backlight PUT 暂 clamp 10-255 |
| 预设 WAV 文件为 32bit PCM | S32 数据直接传 audio_track_write_data，无需 S16→S32 转换 |
| plog() 持久化日志 | 写入 /var/upgrade/xiaozhi.log，每次 fsync，崩溃不丢日志 |
| 手动测试前需清理 /dev/shm/ | 否则 applib_init 报 errno=17 (EEXIST) |

---

## 🌐 云端服务

assistant 通过 WebSocket 连接云端 API 完成设备激活和语音对话。已认证的设备可直接使用，未认证设备需先完成激活流程。

**支持的 LLM 模型**：通过 xiaozhi.me 控制台配置智能体，可连接豆包、DeepSeek 等大语言模型进行智能对话与意图识别。不同模型提供不同的对话风格和能力，可在控制台中随时切换。

默认云端地址定义在 `device/assistant/include/xiaozhi_config.h`：
- `DEFAULT_OTA_URL` — OTA 激活接口
- `DEFAULT_WS_URL` — WebSocket 通信接口

如需对接自建服务端，修改上述宏定义即可。

### MCP 设备工具

智能体经 MCP 接入点可调用以下设备工具（mcp_handler.c）：

| 工具 | 说明 |
|------|------|
| self.reboot | 重启设备（带提示音） |
| self.poweroff | 关机（走原生动画链，8s 兜底硬关机） |
| self.limit_info | 查询每日使用时长状态（只读） |
| self.screen_off_set N | 设置唤醒期间息屏秒数（0=关闭） |

---

## 🔄 关于实时对话模式（Realtime Mode）

支持两种监听模式（`listening_mode`，持久化于 `/var/upgrade/.listening_mode`，**默认 AutoStop**——Realtime 依赖本地软 AEC，回声残留影响体验，效果改善后可切回）：

- **AutoStop（自动停止）模式** — TTS 播放时停止向云端上传音频，TTS 播放结束后恢复上传；用户可通过唤醒词打断当前播放
- **Realtime（实时对话）模式** — 播放期间持续上传音频，配合本地 AEC 实现边播边听、随时插话打断

### GS705B 设备的 AEC 方案演进

1. **设备原生有 AEC**：设备固件中包含 AEC 回声消除模型（`AEC_ch3-2-ch2_1ref_common_20181226_v0.9.4.bin`），原版 sair 通过闭源的 `libduilite_fespl.so` 内部调用
2. **闭源无法调用**：AEC 功能封装在闭源引擎内部，无公开 API，无法在自定义程序中直接使用
3. **云端 AEC 不可行**：参考信号时间同步精度不足，实测效果极差
4. **soft-float ABI 限制**：浮点运算在软浮点工具链下性能极差 → 自研 **定点 NLMS 实现**（Q13 权重、纯整数运算，audioproc.c）规避

自 v2.4 起，Realtime 模式采用**本地定点 NLMS AEC**：上行链取 mic0+mic1 混合、以扬声器参考通道（ch2）为参考做自适应滤波，输出干净单声道供 Opus 编码上行。效果属软件兜底级别，若实机回声残留明显，可切换回 AutoStop 模式。

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

### 基础接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/ping | 心跳检测 |
| GET | /api/version | 获取版本号 |
| GET | /api/config | 获取 xwebd 配置（upload_max_mb, log_level） |
| PUT | /api/config | 修改 xwebd 配置 |
| GET | /api/system | 系统信息（CPU、内存、内核） |
| GET | /api/backlight | 获取背光持久化配置（enable, brightness） |
| PUT | /api/backlight | 设置背光持久化（enable 默认 0，开启后开机自动应用亮度） |

### 设备管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/services | 获取服务状态列表 |
| POST | /api/services/toggle | 服务开关（telnet/autostart/led/usb_lun/boot_watchdog） |
| GET | /api/processes | 获取进程列表 |
| POST | /api/processes/control | 进程控制（stop/start/restart） |
| GET | /api/diag | 设备自检诊断 |
| POST | /api/reboot | 重启设备 |
| POST | /api/poweroff | 关机 |

### 日志

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/logs | 获取 xwebd 日志 |
| POST | /api/logs/clean?source=N | 清理日志（1=sair, 2=xwebd） |

### USB 模式

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/usb/mode | 获取 USB 模式 |
| POST | /api/usb/mode | 切换 USB 模式（mass_adb/adb） |

### 文件管理

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/upload | 上传文件（X-Filename 头指定文件名） |
| GET | /api/files?path= | 文件列表 |
| GET | /api/files/download?path= | 下载文件 |
| DELETE | /api/files?path= | 删除文件 |
| POST | /api/files/batch-delete | 批量删除 |
| POST | /api/files/cleanup | 垃圾清理（日志截断、临时文件、旧备份） |

### 助手管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/assistant/status | 助手状态（installed, running, version 等） |
| GET | /api/assistant/config | 助手配置 |
| PUT | /api/assistant/config | 修改配置 |
| GET | /api/assistant/env | 助手环境检查 |
| GET | /api/assistant/diag | 助手诊断 |
| GET | /api/assistant/logs | 助手日志 |
| POST | /api/assistant/logs/clear | 清除助手日志 |
| POST | /api/assistant/deploy | 部署助手（冷部署，设备重启） |
| POST | /api/assistant/update | 冷更新（设备重启） |
| POST | /api/assistant/upgrade | 热更新（cmd.json+SIGUSR1，不重启设备，秒级完成） |
| POST | /api/assistant/uninstall | 卸载助手 |
| POST | /api/assistant/activate | 激活设备 |
| POST | /api/assistant/wakeup | 唤醒助手 |
| POST | /api/assistant/abort | 中止助手对话 |

### 自更新

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/self-update | xwebd 冷更新（替换二进制 + reboot） |

---

## 📊 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `XIAOZHI_DEVICE_HOST` | (空) | 设备 IP 地址，Panel 启动时自动使用 |
| `XIAOZHI_PANEL_PORT` | 3000 | Panel 监听端口 |
| `XIAOZHI_XWEBD_PORT` | 8080 | xwebd 服务端口 |
| `SDK_PATH` | `../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot` | 交叉编译工具链路径 |

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

## 📝 变更记录

### v2.4.7 / xwebd v1.2.5（2026-08-27 · 第一批深度重构 + 实机联调修复）

依据对原生固件的深度逆向（消息链、音频链、看门狗布局、关机链），完成 16 项改造：

- **AutoStop 唤醒打断改善** — AI_STOP(1004) 抢占收敛、播放器 stop_with_wait 由 2s 忙轮询改为 300ms 条件等待、播放期唤醒检测保持运行
- **Realtime 本地 AEC** — 上行链 mic0+mic1 混合 + 定点 NLMS 消参考（audioproc.c），替代云端 AEC
- **原生关机链** — MCP `self.poweroff` 走 MSG_SHORTCUT_POWER 动画关机（8s 兜底硬关机）
- **每日使用时长** — 按 TTS 播放时长累计，触限后唤醒仅播提醒音不连服务器（默认关闭）
- **屏幕字幕** — TTS 回复文本经 0x23A 广播推送原生 wiki 场景插件（0x232 清屏）
- **会话息屏** — 唤醒期间可配置 N 秒息屏省电，触摸/按键点亮（默认关闭）
- **xwebd 增强** — 亮度持久化（/api/backlight，默认关闭）+ 由 assistant 宿主化守护自启
- **服务端联调修复（实机定位）** — hello 的 `features."aec":true` 被 tenclass 服务器静默丢弃导致无法进入对话（PC 复刻交叉矩阵定位），已移除该声明；唤醒回环自噬（msg_server 先发 AI_STOP 再发 AI_START，挂起停止误杀新会话）已修复
- **稳定性** — 看门狗 app_running_list 布局校准（0x28 头 + pid@+0x00）、plog WARN 即时 fsync/INFO 节流、触摸按键动态探测 goodix eventN、sair 服务端 RPC 最小集（1000/1001/1004/1006/1008/1012）

---

## 📄 许可证

MIT License

Copyright (c) 2025 xiaozhi-zhiban contributors

本项目使用 MIT 许可证开源。请注意：
- 设备固件中的闭源动态库（libapplib.so、libduilite_fespl.so 等）不属于本项目，它们由设备原生系统提供
- 第三方静态库（mbedtls、opus、cJSON）各自遵循其原始许可证
