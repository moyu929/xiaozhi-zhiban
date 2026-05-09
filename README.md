# 小智·智伴 (xiaozhi-zhiban)

小智AI语音助手的开源替代固件，供开发者二次开发。

## 项目组成

| 组件 | 运行平台 | 说明 |
|------|----------|------|
| **assistant** | 设备端 | 语音助手模块（sair），负责语音唤醒、ASR、WebSocket通信、状态管理 |
| **xwebd** | 设备端 | Web控制守护进程，提供HTTP API供Panel通信，综合设备端零散脚本功能 |
| **panel** | PC端 | 控制面板，Web界面统一管理设备，替代各种零散的调试/部署脚本 |

## 支持设备

- GS705B（Cortex-A5, uClibc 0.9.33.2）

## 编译

### 环境要求

- Linux环境（WSL即可）
- ARM soft-float uClibc交叉编译工具链

### 获取工具链

从 [ChrisTheCoolHut/uClibc-Cross-Compilers](https://github.com/ChrisTheCoolHut/uClibc-Cross-Compilers) 下载 `arm-buildroot-linux-uclibcgnueabi` 对应的工具链，解压到项目根目录的 `toolchain/` 下：

```
toolchain/
└── arm-buildroot-linux-uclibcgnueabi_sdk-buildroot/
    ├── bin/
    ├── arm-buildroot-linux-uclibcgnueabi/
    └── ...
```

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
2. 浏览器打开 `http://localhost:8080`
3. 在面板中完成设备连接和固件部署

## 项目结构

```
device/
├── assistant/           # 语音助手模块
│   ├── src/             # C源码
│   ├── include/         # 头文件
│   │   └── reverse/     # 逆向还原的设备原生API头文件
│   ├── lib/             # 第三方库头文件（cJSON, mbedtls, opus）
│   ├── lib_sf/          # soft-float预编译静态库（.a）
│   ├── prebuilt/        # 预编译二进制（可直接部署）
│   └── build.sh         # 编译脚本
├── xwebd/               # Web控制守护进程
│   ├── src/             # C源码
│   ├── include/         # 头文件
│   ├── prebuilt/        # 预编译二进制
│   ├── scripts/         # 设备端脚本（看门狗等）
│   └── build.sh         # 编译脚本
panel/                    # PC端控制面板
├── server.py            # 后端服务
├── device_api.py         # xwebd API客户端
├── assistant_api.py      # assistant API客户端
├── adb_manager.py        # ADB设备管理
├── control_panel.py      # 控制面板逻辑
└── static/              # 前端（HTML/CSS/JS）
```

## 关键编译参数

- **工具链**: `arm-buildroot-linux-uclibcgnueabi-gcc`（soft-float ABI）
- **CPU**: `-mcpu=cortex-a5 -mfloat-abi=soft`
- **Opus**: 必须加 `-DFIXED_POINT=1`（soft-float ABI下调用hard-float libm会SIGFPE）
- **assistant链接**: `-rdynamic`（导出符号给dlopen）+ `-Wl,--unresolved-symbols=ignore-all`（设备原生库运行时提供）

## 许可证

MIT License
