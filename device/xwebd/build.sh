#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"

SDK_PATH="${SDK_PATH:-$PROJECT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}

export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

COMMON_CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT -I$PROJECT_DIR/include -D_GNU_SOURCE"

mkdir -p "$BUILD_DIR"

VERSION_FILE="$PROJECT_DIR/.version"
if [ -f "$VERSION_FILE" ]; then
    IFS='.' read -r V_MAJOR V_MINOR V_PATCH < "$VERSION_FILE"
else
    V_MAJOR=1; V_MINOR=1; V_PATCH=0
fi
V_PATCH=$((V_PATCH + 1))
if [ $V_PATCH -ge 10 ]; then
    V_PATCH=0
    V_MINOR=$((V_MINOR + 1))
    if [ $V_MINOR -ge 10 ]; then
        V_MINOR=0
        V_MAJOR=$((V_MAJOR + 1))
    fi
fi
echo "${V_MAJOR}.${V_MINOR}.${V_PATCH}" > "$VERSION_FILE"
BUILD_VERSION="${V_MAJOR}.${V_MINOR}.${V_PATCH}"
echo "#define XWEBD_VERSION \"$BUILD_VERSION\"" > "$BUILD_DIR/version.h"
COMMON_CFLAGS="$COMMON_CFLAGS -I$BUILD_DIR"

echo "=== Building xwebd ==="
echo "CC: $CC"
echo "SYSROOT: $SYSROOT"

echo "[1/2] Compiling xwebd"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/xwebd.c" -o "$BUILD_DIR/xwebd.o"

echo "[2/2] Compiling plugin_gateway"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/plugin_gateway.c" -o "$BUILD_DIR/plugin_gateway.o"

echo "Linking xwebd"
$CC --sysroot=$SYSROOT \
    -mcpu=cortex-a5 \
    -mfloat-abi=soft \
    -no-pie \
    $BUILD_DIR/xwebd.o \
    $BUILD_DIR/plugin_gateway.o \
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

PREBUILT_DIR="$PROJECT_DIR/prebuilt"
mkdir -p "$PREBUILT_DIR"
cp "$BUILD_DIR/xwebd" "$PREBUILT_DIR/xwebd"
cp "$BUILD_DIR/version.h" "$PREBUILT_DIR/version.h"
echo "Copied to: $PREBUILT_DIR/xwebd"
