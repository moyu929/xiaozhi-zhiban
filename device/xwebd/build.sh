#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"

SDK_PATH="${SDK_PATH:-$PROJECT_DIR/../../../工具链/uClibc-Cross-Compilers-master/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}

export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

COMMON_CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT -I$PROJECT_DIR/include -D_GNU_SOURCE"

mkdir -p "$BUILD_DIR"

echo "=== Building xwebd ==="
echo "CC: $CC"
echo "SYSROOT: $SYSROOT"

echo "[1/1] Compiling xwebd"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/xwebd.c" -o "$BUILD_DIR/xwebd.o"

echo "Linking xwebd"
$CC --sysroot=$SYSROOT \
    -mcpu=cortex-a5 \
    -mfloat-abi=soft \
    -no-pie \
    $BUILD_DIR/xwebd.o \
    -L$SYSROOT/usr/lib \
    -lpthread -lrt -lm \
    -o "$BUILD_DIR/xwebd"

STRIP=${STRIP:-arm-buildroot-linux-uclibcgnueabi-strip}
if command -v $STRIP &> /dev/null; then
    PRE_SIZE=$(stat -c%s "$BUILD_DIR/xwebd" 2>/dev/null || echo "?")
    $STRIP --strip-unneeded "$BUILD_DIR/xwebd"
    POST_SIZE=$(stat -c%s "$BUILD_DIR/xwebd" 2>/dev/null || echo "?")
    echo "Strip: ${PRE_SIZE} -> ${POST_SIZE} bytes"
else
    echo "Strip: $STRIP not found, skipping"
fi

echo ""
echo "=== Build successful! ==="
echo "Output: $BUILD_DIR/xwebd"
ls -la "$BUILD_DIR/xwebd"
