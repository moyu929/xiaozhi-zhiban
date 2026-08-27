#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"

SDK_PATH="${SDK_PATH:-$PROJECT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC=${CC:-arm-buildroot-linux-uclibcgnueabi-gcc}

export PATH="$SDK_PATH/bin:$PATH"
SYSROOT="$SDK_PATH/arm-buildroot-linux-uclibcgnueabi/sysroot"

REVERSE_INCLUDE="$PROJECT_DIR/include/reverse"
MBEDTLS_DIR="$PROJECT_DIR/lib/mbedtls"
CJSON_DIR="$PROJECT_DIR/lib/cJSON"
OPUS_DIR="$PROJECT_DIR/lib/opus"
LIB_DIR="$PROJECT_DIR/lib_sf"

COMMON_CFLAGS="-Wall -Os -g0 -mcpu=cortex-a5 -mfloat-abi=soft -no-pie --sysroot=$SYSROOT -I$PROJECT_DIR/include -I$REVERSE_INCLUDE -I$MBEDTLS_DIR/include -I$CJSON_DIR -I$OPUS_DIR/include -D_GNU_SOURCE -DFIXED_POINT=1"

STUB_DIR="$BUILD_DIR/stubs"
mkdir -p "$BUILD_DIR" "$STUB_DIR"

VERSION_FILE="$PROJECT_DIR/.version"
if [ -f "$VERSION_FILE" ]; then
    IFS='.' read -r V_MAJOR V_MINOR V_PATCH < "$VERSION_FILE"
else
    V_MAJOR=2; V_MINOR=1; V_PATCH=0
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
echo "#define XIAOZHI_VERSION \"$BUILD_VERSION\"" > "$BUILD_DIR/version.h"
COMMON_CFLAGS="$COMMON_CFLAGS -I$BUILD_DIR"

DEVICE_LIBS="applib apconfig configpart audio_service_api audio_recorder dds"
for lib in $DEVICE_LIBS; do
    if [ ! -f "$SYSROOT/usr/lib/lib${lib}.so" ] && [ ! -f "$LIB_DIR/lib${lib}.so" ]; then
        echo "Creating stub: lib${lib}.so"
        $CC -shared -Wl,-soname,lib${lib}.so -o "$STUB_DIR/lib${lib}.so" -mcpu=cortex-a5 -mfloat-abi=soft
    fi
done

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

echo "[11/20] Compiling websocket.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/websocket.c" -o "$BUILD_DIR/websocket.o"

echo "[12/20] Compiling mqtt_client.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/mqtt_client.c" -o "$BUILD_DIR/mqtt_client.o"

echo "[13/20] Compiling udp_audio.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/udp_audio.c" -o "$BUILD_DIR/udp_audio.o"

echo "[14/20] Compiling protocol_handler.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/protocol_handler.c" -o "$BUILD_DIR/protocol_handler.o"

echo "[15/20] Compiling audio_player.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_player.c" -o "$BUILD_DIR/audio_player.o"

echo "[16/20] Compiling audio_recorder.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_recorder.c" -o "$BUILD_DIR/audio_recorder.o"

echo "[16b/20] Compiling audio_precache.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audio_precache.c" -o "$BUILD_DIR/audio_precache.o"

echo "[17/20] Compiling mcp_handler.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/mcp_handler.c" -o "$BUILD_DIR/mcp_handler.o"

echo "[18/20] Compiling api_server.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/api_server.c" -o "$BUILD_DIR/api_server.o"

echo "[19/20] Compiling diag_module.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/diag_module.c" -o "$BUILD_DIR/diag_module.o"
echo "[19a] Compiling audioproc.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/audioproc.c" -o "$BUILD_DIR/audioproc.o"
echo "[19b] Compiling platform_power.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/platform_power.c" -o "$BUILD_DIR/platform_power.o"
echo "[19c] Compiling use_limit.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/use_limit.c" -o "$BUILD_DIR/use_limit.o"
echo "[19d] Compiling display_ctrl.o"
$CC $COMMON_CFLAGS -c "$PROJECT_DIR/src/display_ctrl.c" -o "$BUILD_DIR/display_ctrl.o"

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
    $BUILD_DIR/mqtt_client.o \
    $BUILD_DIR/udp_audio.o \
    $BUILD_DIR/protocol_handler.o \
    $BUILD_DIR/audio_player.o \
    $BUILD_DIR/audio_recorder.o \
    $BUILD_DIR/audio_precache.o \
    $BUILD_DIR/mcp_handler.o \
    $BUILD_DIR/api_server.o \
    $BUILD_DIR/diag_module.o \
    $BUILD_DIR/audioproc.o \
    $BUILD_DIR/platform_power.o \
    $BUILD_DIR/use_limit.o \
    $BUILD_DIR/display_ctrl.o \
    -L$STUB_DIR \
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

PREBUILT_DIR="$PROJECT_DIR/prebuilt"
mkdir -p "$PREBUILT_DIR"
cp "$BUILD_DIR/sair" "$PREBUILT_DIR/sair"
cp "$BUILD_DIR/version.h" "$PREBUILT_DIR/version.h"
echo "Copied to: $PREBUILT_DIR/sair"
