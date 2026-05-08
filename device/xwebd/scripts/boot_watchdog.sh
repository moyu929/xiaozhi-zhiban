#!/bin/sh
# boot_watchdog.sh - 开机频率检测与自动回退（v2 - 不依赖时钟）
# 部署位置: /var/upgrade/boot_watchdog.sh
# 由 /var/upgrade/test.sh 调用 (rcS hook，manager启动sair之前执行)
#
# 两种循环重启场景：
#   场景1(快速): wrapper脚本崩溃/程序秒退，几秒内重启
#   场景2(慢速): 程序启动但未进入消息循环，看门狗50秒超时后重启
#
# 工作原理（不依赖系统时钟）：
#   1. 每次启动时，检查"存活标记"是否存在
#      - 存活标记在上次启动后由延迟任务创建（启动120秒后）
#      - 如果存活标记不存在，说明上次启动存活不到120秒 → 短命启动
#   2. 短命启动计数器累加
#   3. 计数器达到阈值(3次连续短命) → 触发回退
#   4. 正常启动（存活超过120秒）→ 计数器清零
#
# 优势：完全不依赖 date/RTC/uptime 时钟，只依赖文件是否存在

CRASH_COUNT_FILE="/var/upgrade/.boot_crash_count"
ALIVE_MARKER="/var/upgrade/.boot_alive_marker"
BOOT_HAPPENED="/var/upgrade/.boot_happened"
RECOVERY_LOG="/var/upgrade/.boot_recovery_log"
SAIR_WRAPPER="/var/upgrade/sair"
SAIR_BACKUP="/var/upgrade/sair_backup"
MARKER="/var/upgrade/.watchdog_triggered"
MAX_CRASHES=3
ALIVE_SECONDS=120

log_msg() {
    echo "[BOOT_WATCHDOG] $1" > /dev/ttyprintk 2>/dev/null
}

# --- 第1步：判断上次启动是否短命 ---
# 三种情况：
#   A. ALIVE_MARKER存在 → 上次启动存活超过120秒 → 正常，计数器清零
#   B. ALIVE_MARKER不存在 + BOOT_HAPPENED存在 → 上次启动短命 → 计数器+1
#   C. ALIVE_MARKER不存在 + BOOT_HAPPENED不存在 → 首次启动 → 计数器不变

if [ -f "$ALIVE_MARKER" ]; then
    rm -f "$ALIVE_MARKER"
    count=0
    echo "$count" > "$CRASH_COUNT_FILE"
    log_msg "Last boot was healthy, crash counter reset to 0"
elif [ -f "$BOOT_HAPPENED" ]; then
    if [ -f "$CRASH_COUNT_FILE" ]; then
        count=$(cat "$CRASH_COUNT_FILE" 2>/dev/null)
        case "$count" in
            *[!0-9]*) count=0 ;;
        esac
    else
        count=0
    fi
    count=$((count + 1))
    echo "$count" > "$CRASH_COUNT_FILE"
    log_msg "Last boot was short-lived, crash counter: $count"
else
    count=0
    echo "$count" > "$CRASH_COUNT_FILE"
    log_msg "First boot detected, crash counter: 0"
fi

# 标记本次启动已发生（供下次启动判断）
touch "$BOOT_HAPPENED"

# --- 第2步：判断是否触发紧急回退 ---
if [ "$count" -ge "$MAX_CRASHES" ]; then
    log_msg "EMERGENCY: $count consecutive short-lived boots (threshold: $MAX_CRASHES)"

    if [ -f "$SAIR_WRAPPER" ]; then
        log_msg "Removing $SAIR_WRAPPER"
        rm -f "$SAIR_WRAPPER"
    fi

    if [ -f "$SAIR_BACKUP" ]; then
        log_msg "Removing $SAIR_BACKUP"
        rm -f "$SAIR_BACKUP"
    fi

    echo "triggered" > "$MARKER"

    uptime_str=$(cat /proc/uptime 2>/dev/null | awk '{print $1}')
    echo "RECOVERY: crash_count=$count, uptime=$uptime_str" >> "$RECOVERY_LOG"

    count=0
    echo "$count" > "$CRASH_COUNT_FILE"

    log_msg "Recovery complete, will boot with original sair"
fi

# --- 第3步：设置延迟存活标记 ---
# 在后台等待 ALIVE_SECONDS 秒后创建存活标记
# 如果设备在 ALIVE_SECONDS 内重启，标记不会被创建 → 下次启动检测到短命
# 快速重启场景：几秒内重启 → 标记不会创建 → 计数器+1
# 慢速重启场景：50秒后看门狗重启 → 50 < 120 → 标记不会创建 → 计数器+1
# 正常运行：超过120秒 → 标记创建 → 下次启动时计数器清零
(sleep "$ALIVE_SECONDS" && touch "$ALIVE_MARKER") &
