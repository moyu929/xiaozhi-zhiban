#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"

SDK_PATH="${SDK_PATH:-$PROJECT_DIR/../../../工具链/uClibc-Cross-Compilers-master/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}

export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT"
LDFLAGS="-ldl"

mkdir -p "$BUILD_DIR"

echo "=== Building usb_helper ==="
$CC $CFLAGS $LDFLAGS "$PROJECT_DIR/src/usb_helper.c" -o "$BUILD_DIR/usb_helper"

STRIP=${STRIP:-arm-buildroot-linux-uclibcgnueabi-strip}
$STRIP --strip-unneeded "$BUILD_DIR/usb_helper"

echo "Output: $BUILD_DIR/usb_helper"
ls -la "$BUILD_DIR/usb_helper"

echo "=== Checking NEEDED libs ==="
arm-buildroot-linux-uclibcgnueabi-readelf -d "$BUILD_DIR/usb_helper" | grep NEEDED
