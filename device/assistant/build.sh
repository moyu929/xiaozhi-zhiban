#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"

SDK_PATH="${SDK_PATH:-$PROJECT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}

export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

REVERSE_INCLUDE="$PROJECT_DIR/include/reverse"
MBEDTLS_DIR="$PROJECT_DIR/lib/mbedtls"
CJSON_DIR="$PROJECT_DIR/lib/cJSON"
OPUS_DIR="$PROJECT_DIR/lib/opus"
LIB_DIR="$PROJECT_DIR/lib_sf"

COMMON_CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT -I$PROJECT_DIR/include -I$REVERSE_INCLUDE -I$MBEDTLS_DIR/include -I$CJSON_DIR -I$OPUS_DIR/include -D_GNU_SOURCE -DFIXED_POINT=1"

mkdir -p "$BUILD_DIR"

echo "=== Building xiaozhi-assistant ==="
echo "CC: $CC"
echo "SYSROOT: $SYSROOT"
echo "PROJECT_DIR: $PROJECT_DIR"

echo "[1/18] Compiling main.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/main.c" -o "$BUILD_DIR/main.o"

echo "[2/18] Compiling state_machine.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/state_machine.c" -o "$BUILD_DIR/state_machine.o"

echo "[3/18] Compiling watchdog.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/watchdog.c" -o "$BUILD_DIR/watchdog.o"

echo "[4/18] Compiling config_manager.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/config_manager.c" -o "$BUILD_DIR/config_manager.o"

echo "[5/18] Compiling plog.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/plog.c" -o "$BUILD_DIR/plog.o"

echo "[6/18] Compiling wakeup_module.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/wakeup_module.c" -o "$BUILD_DIR/wakeup_module.o"

echo "[7/18] Compiling audio_dispatcher.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_dispatcher.c" -o "$BUILD_DIR/audio_dispatcher.o"

echo "[8/18] Compiling touch_key.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/touch_key.c" -o "$BUILD_DIR/touch_key.o"

echo "[9/18] Compiling tls_transport.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/tls_transport.c" -o "$BUILD_DIR/tls_transport.o"

echo "[10/18] Compiling http_client.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/http_client.c" -o "$BUILD_DIR/http_client.o"

echo "[11/18] Compiling websocket.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/websocket.c" -o "$BUILD_DIR/websocket.o"

echo "[12/18] Compiling protocol_handler.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/protocol_handler.c" -o "$BUILD_DIR/protocol_handler.o"

echo "[13/18] Compiling audio_player.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_player.c" -o "$BUILD_DIR/audio_player.o"

echo "[14/18] Compiling audio_recorder.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_recorder.c" -o "$BUILD_DIR/audio_recorder.o"

echo "[15/18] Compiling mcp_handler.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/mcp_handler.c" -o "$BUILD_DIR/mcp_handler.o"

echo "[16/18] Compiling api_server.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/api_server.c" -o "$BUILD_DIR/api_server.o"

echo "[17/18] Compiling diag_module.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/diag_module.c" -o "$BUILD_DIR/diag_module.o"

echo "Linking sair"
$CC --sysroot=$SYSROOT \
    -mcpu=cortex-a5 \
    -mfloat-abi=soft \
    -no-pie \
    -rdynamic \
    -Wl,--no-as-needed \
    $BUILD_DIR/main.o \
    $BUILD_DIR/state_machine.o \
    $BUILD_DIR/watchdog.o \
    $BUILD_DIR/config_manager.o \
    $BUILD_DIR/plog.o \
    $BUILD_DIR/wakeup_module.o \
    $BUILD_DIR/audio_dispatcher.o \
    $BUILD_DIR/touch_key.o \
    $BUILD_DIR/tls_transport.o \
    $BUILD_DIR/http_client.o \
    $BUILD_DIR/websocket.o \
    $BUILD_DIR/protocol_handler.o \
    $BUILD_DIR/audio_player.o \
    $BUILD_DIR/audio_recorder.o \
    $BUILD_DIR/mcp_handler.o \
    $BUILD_DIR/api_server.o \
    $BUILD_DIR/diag_module.o \
    -L$LIB_DIR \
    -L$SYSROOT/usr/lib \
    -lmbedtls -lmbedx509 -lmbedcrypto \
    -lcjson \
    -lopus \
    -lpthread -lrt -ldl -lm \
    -lapplib -lapconfig -lconfigpart -laudio_service_api -laudio_recorder -ldds \
    -Wl,--unresolved-symbols=ignore-all \
    -o "$BUILD_DIR/sair"

STRIP=${STRIP:-arm-buildroot-linux-uclibcgnueabi-strip}
if command -v $STRIP &> /dev/null; then
    PRE_SIZE=$(stat -c%s "$BUILD_DIR/sair" 2>/dev/null || echo "?")
    $STRIP --strip-unneeded "$BUILD_DIR/sair"
    POST_SIZE=$(stat -c%s "$BUILD_DIR/sair" 2>/dev/null || echo "?")
    echo "Strip: ${PRE_SIZE} -> ${POST_SIZE} bytes"
else
    echo "Strip: $STRIP not found, skipping"
fi

echo ""
echo "=== Build successful! ==="
echo "Output: $BUILD_DIR/sair"
ls -la "$BUILD_DIR/sair"
