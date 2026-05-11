# 小智·智伴 (xiaozhi-zhiban)

小智AI语音助手的开源替代固件，供开发者二次开发。

## 项目组成

| 组件 | 运行平台 | 说明 |
|------|----------|------|
| **assistant** | 设备端 | 语音助手模块（sair），负责语音唤醒、ASR、WebSocket通信、状态管理 |
| **xwebd** | 设备端 | Web控制守护进程，提供HTTP API供Panel通信，综合设备端零散脚本功能 |
| **panel** | PC端 | 控制面板，Web界面统一管理设备，替代各种零散的调试/部署脚本 |

## 支持设备

- **GS705B** 早教机器人（ACTIONS OWL SoC, ARM Cortex-A5 四核 900MHz）
- 内存: ~56MB | 存储: NAND Flash 9.8MB可写分区 | 网络: WiFi
- C库: uClibc 0.9.33.2 | 内核: Linux 3.10.52 | 浮点ABI: hard-float

## 编译

### 环境要求

- Linux环境（WSL即可）
- ARM soft-float uClibc交叉编译工具链（见下方获取方式）

### 获取工具链

从 [ChrisTheCoolHut/uClibc-Cross-Compilers](https://github.com/ChrisTheCoolHut/uClibc-Cross-Compilers) 下载 `arm-buildroot-linux-uclibcgnueabi_sdk-buildroot.tar.xz`（约54MB），解压到项目根目录的 `toolchain/` 并重定位：

```bash
mkdir -p toolchain
tar -xf arm-buildroot-linux-uclibcgnueabi_sdk-buildroot.tar.xz -C toolchain/
toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/relocate-sdk.sh
```

完成后目录结构如下：

```
toolchain/
└── arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/
    ├── bin/                                    # 交叉编译器
    │   └── arm-buildroot-linux-uclibcgnueabi-gcc
    ├── arm-buildroot-linux-uclibcgnueabi/      # sysroot
    └── relocate-sdk.sh                         # 路径重定位脚本
```

> **注意**：`relocate-sdk.sh` 必须执行，它会将工具链内硬编码的绝对路径更新为当前解压位置。不执行可能导致编译时找不到头文件或库。

### 设备原生库

assistant 运行时依赖设备固件自带的闭源动态库，这些库由设备原生系统提供，无需手动安装：

| 库 | 来源 | 用途 |
|----|------|------|
| libapplib.so | /usr/lib | 应用框架 |
| libapconfig.so | /usr/lib | 配置管理 |
| libconfigpart.so | /usr/lib | 配置分区 |
| libaudio_service_api.so | /usr/lib | 音频服务 |
| libaudio_recorder.so | /usr/lib | 音频录制 |
| libdds.so | /usr/lib | DDS消息服务 |
| libsair_asr.so | /usr/lib | ASR引擎（运行时dlopen加载） |

编译时 build.sh 会自动检测这些库是否存在于 sysroot 中。若不存在（全新工具链的默认情况），会自动创建空 stub `.so` 文件满足链接需求，配合 `-Wl,--unresolved-symbols=ignore-all` 跳过未定义符号检查。运行时由设备动态链接器加载真实实现。

### 云端服务

assistant 需要连接云端API完成设备激活和WebSocket通信。已认证的设备可直接使用，未认证设备需先完成激活流程。

默认云端地址定义在 `device/assistant/include/xiaozhi_config.h`：
- `DEFAULT_OTA_URL` — OTA激活接口
- `DEFAULT_WS_URL` — WebSocket通信接口

如需对接自建服务端，修改上述宏定义即可。

### 编译 assistant

```bash
cd device/assistant
bash build.sh
```

编译产物：`device/assistant/build/sair`

### 编译 xwebd

```bash
cd device/xwebd
bash build.sh
```

编译产物：`device/xwebd/build/xwebd`

> build.sh 默认从 `$PROJECT_DIR/../../toolchain/` 查找工具链，也可通过 `SDK_PATH` 环境变量指定：
> ```bash
> SDK_PATH=/path/to/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot bash build.sh
> ```

## 部署

通过Panel控制面板部署，无需手动操作设备。

1. 启动Panel：`cd panel && python control_panel.py`
2. 浏览器打开 `http://localhost:3000`
3. 在面板中完成设备连接和固件部署

也支持USB连接（通过ADB端口转发），面板会自动检测ADB设备。

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `XIAOZHI_DEVICE_HOST` | (空) | 设备IP地址，Panel启动时自动使用 |
| `XIAOZHI_PANEL_PORT` | 3000 | Panel监听端口 |
| `XIAOZHI_XWEBD_PORT` | 8080 | xwebd服务端口 |
| `XIAOZHI_SAIR_API_PORT` | 8081 | assistant API端口 |
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

## 项目结构

```
device/
├── assistant/           # 语音助手模块
│   ├── src/             # C源码
│   ├── include/         # 头文件
│   │   └── reverse/     # 逆向还原的设备原生API头文件
│   ├── lib/             # 第三方库头文件（cJSON, mbedtls, opus）
│   ├── lib_sf/          # soft-float预编译静态库（.a）
│   └── build.sh         # 编译脚本
├── xwebd/               # Web控制守护进程
│   ├── src/             # C源码
│   ├── include/         # 头文件
│   ├── scripts/         # 设备端脚本（看门狗等）
│   └── build.sh         # 编译脚本
panel/                    # PC端控制面板
├── server.py            # 后端服务
├── device_api.py         # xwebd API客户端
├── assistant_api.py      # assistant API客户端
├── adb_manager.py        # ADB设备管理
├── control_panel.py      # 控制面板启动入口
└── static/              # 前端（HTML/CSS/JS）
```

## 关键编译参数

- **工具链**: `arm-buildroot-linux-uclibcgnueabi-gcc` 7.4.0（soft-float ABI）
- **CPU**: `-mcpu=cortex-a5 -mfloat-abi=soft`
- **Opus**: 必须加 `-DFIXED_POINT=1`（soft-float ABI下调用hard-float libm会SIGFPE）
- **assistant链接**: `-rdynamic`（导出符号给dlopen）+ `-Wl,--unresolved-symbols=ignore-all`（设备原生库运行时提供）
- **设备原生库stub**: 编译时自动为缺失的设备原生库创建空 `.so` stub，确保链接器生成正确的 DT_NEEDED 条目

## 关于实时对话模式（Realtime Mode）

本项目的语音助手目前仅使用 **AutoStop（自动停止）模式**，即用户在TTS播放期间说出唤醒词可打断当前播放，重新进入监听状态。

在开发过程中，我们曾参考 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 实现了 Realtime（实时对话）模式，但最终移除了该功能。以下是研究发现和结论：

### Realtime 模式原理

xiaozhi-esp32 支持两种监听模式，由 AEC（回声消除）能力决定默认模式：

| 模式 | 触发条件 | 行为 |
|------|----------|------|
| **AutoStop** | AEC 关闭 | TTS播放时恢复唤醒词检测，用户说唤醒词打断 |
| **Realtime** | AEC 开启（设备端或云端） | TTS播放时暂停唤醒词检测，依赖云端VAD检测用户语音打断 |

AEC 的作用是消除扬声器播放的TTS音频对麦克风输入的干扰，使云端能够在播放期间识别用户语音。xiaozhi-esp32 支持三种 AEC 模式：
- `kAecOff`：无AEC，默认AutoStop模式
- `kAecOnServerSide`：云端AEC，设备发送参考音频给服务器
- `kAecOnDeviceSide`：设备端AEC，ESP32有硬件AEC支持

### GS705B 设备的 AEC 困境

GS705B 设备无法实现可用的 Realtime 模式，原因如下：

1. **无硬件AEC**：ACTIONS OWL SoC 不像 ESP32 那样提供硬件AEC加速器
2. **无软件AEC**：设备固件的闭源音频服务（`libaudio_service_api.so`）未暴露AEC接口，无法在设备端实现回声消除
3. **云端AEC不可行**：云端AEC要求设备发送参考信号（TTS播放的音频）给服务器做回声消除。虽然我们的录音器支持3通道（2mic+1ref），参考信号可以采集到，但：
   - 上行带宽受限（NAND Flash设备通常通过WiFi连接，上行带宽有限）
   - 参考信号与麦克风信号的时间同步精度不足（无硬件AEC时，软件采集的参考信号延迟不稳定）
   - 实测中云端AEC效果极差，无法可靠地消除回声，导致用户语音被TTS回声淹没

4. **soft-float ABI限制**：即使尝试软件AEC算法，soft-float ABI下的浮点运算性能极差（Cortex-A5 900MHz + soft-float），无法实时处理

### 结论

对于 GS705B 这类无硬件AEC能力的嵌入式设备，AutoStop 模式是唯一可行的方案。Realtime 模式需要可靠的回声消除作为前提，而该设备在硬件和软件层面均无法满足此要求。

如果未来设备固件更新提供了AEC接口，或云端AEC算法对低质量参考信号的鲁棒性提升，可以重新考虑实现 Realtime 模式。

## 设备自检架构

设备自检遵循"谁部署谁检查"的原则，确保在部署前验证环境兼容性：

| 检查类型 | 执行时机 | 执行者 | 检查方式 |
|----------|----------|--------|----------|
| xwebd 环境检查 | 部署 xwebd 前 | Panel（ADB模式） | `adb shell` 命令 |
| xwebd 运行时检查 | xwebd 运行中 | xwebd 自身 | `/api/diag` |
| assistant 环境检查 | 部署 assistant 前 | xwebd | `/api/assistant/env` |
| assistant 运行时检查 | assistant 运行中 | assistant 自身 | `/api/assistant/diag`（文件IPC触发） |

### 自检流程

1. **ADB连接设备** → Panel 通过 ADB shell 检查 xwebd 运行环境（分区、磁盘、音频、WiFi等）
2. **部署 xwebd** → xwebd 自检确认自身运行正常
3. **切换无线模式** → 通过 xwebd `/api/assistant/env` 检查 assistant 运行环境（进程、库文件、依赖等）
4. **部署 assistant** → assistant 运行时健康检查（配置、日志、网络、WebSocket等）

### IPC 机制

assistant 与 xwebd 通过文件系统通信（替代原 HTTP 方案），避免 TCP 开销：

| 文件 | 方向 | 用途 |
|------|------|------|
| `/tmp/sair_status.json` | assistant → xwebd | 状态输出（state, version, activation_code） |
| `/tmp/sair_config.json` | assistant → xwebd | 配置输出（ws_url, ws_token, log_level） |
| `/tmp/sair_cmd.json` | xwebd → assistant | 命令输入（set_config, wakeup, abort, upgrade） |
| `/tmp/sair_diag_request` | xwebd → assistant | 自检触发（空文件，创建即触发） |
| `/tmp/sair_diag.json` | assistant → xwebd | 自检结果 |

**通信流程**：xwebd 写入命令文件 → 发送 SIGUSR1 唤醒 assistant 主循环 → assistant 读取并执行命令 → 写入结果文件

## 许可证

MIT License
