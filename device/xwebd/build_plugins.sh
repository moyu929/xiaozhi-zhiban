#!/bin/bash
# 插件批量编译: 编译 plugins/ 下所有 xwplug_*.c 到 build/plugins/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build/plugins"
mkdir -p "$BUILD_DIR"

SDK_PATH="${SDK_PATH:-$SCRIPT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}
export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT -I$SCRIPT_DIR/plugins"

STRIP=${STRIP:-arm-buildroot-linux-uclibcgnueabi-strip}

for src in "$SCRIPT_DIR"/plugins/xwplug_*.c; do
    [ -e "$src" ] || { echo "无插件源码"; exit 0; }
    name=$(basename "$src" .c | sed 's/^xwplug_//')
    out="$BUILD_DIR/xwplug-$name"
    echo "[编译] $name"
    $CC $CFLAGS "$src" -o "$out"
    if command -v $STRIP &> /dev/null; then
        $STRIP --strip-unneeded "$out"
    fi
done

echo "=== 插件编译完成 ==="
ls -la "$BUILD_DIR"
