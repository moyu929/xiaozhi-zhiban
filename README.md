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
- ARM soft-float uClibc交叉编译工具链
- 设备原生动态库（见下方说明）

### 获取工具链

从 [ChrisTheCoolHut/uClibc-Cross-Compilers](https://github.com/ChrisTheCoolHut/uClibc-Cross-Compilers) 下载 `arm-buildroot-linux-uclibcgnueabi` 对应的工具链，解压到项目根目录的 `toolchain/` 下：

```
toolchain/
└── arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/
    ├── bin/
    ├── arm-buildroot-linux-uclibcgnueabi/
    └── ...
```

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

编译时通过 `-Wl,--unresolved-symbols=ignore-all` 跳过这些库的链接检查，运行时由设备动态链接器自动加载。

### 云端服务

assistant 需要连接 `api.tenclass.net` 云端API完成设备激活和WebSocket通信。已认证的设备可直接使用，未认证设备需先完成激活流程。

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

> build.sh 默认从 `$PROJECT_DIR/../toolchain/` 查找工具链，也可通过 `SDK_PATH` 环境变量指定。

## 部署

通过Panel控制面板部署，无需手动操作设备。

1. 启动Panel：`cd panel && python control_panel.py`
2. 浏览器打开 `http://localhost:3000`
3. 在面板中完成设备连接和固件部署

也支持USB连接（通过ADB端口转发），面板会自动检测ADB设备。

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

- **工具链**: `arm-buildroot-linux-uclibcgnueabi-gcc`（soft-float ABI）
- **CPU**: `-mcpu=cortex-a5 -mfloat-abi=soft`
- **Opus**: 必须加 `-DFIXED_POINT=1`（soft-float ABI下调用hard-float libm会SIGFPE）
- **assistant链接**: `-rdynamic`（导出符号给dlopen）+ `-Wl,--unresolved-symbols=ignore-all`（设备原生库运行时提供）

## 许可证

MIT License
