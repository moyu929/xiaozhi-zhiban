#!/bin/sh
# watchdog_guard.sh - 看门狗守护，防止调试时设备重启（增强版）
# 部署位置: /var/upgrade/watchdog_guard.sh
#
# 原理:
#   sair的软件看门狗在app_info偏移0x78设置soft_watchdog_forbid标志
#   当此标志为1时，Manager的check_soft_watchdogs跳过该进程
#   本脚本通过写入共享内存来禁止看门狗
#
# 两种使用方式:
#   1. 一次性: watchdog_guard.sh enable/disable/status
#   2. 守护模式: watchdog_guard.sh daemon — 后台持续禁止看门狗
#      (sair重启后看门狗标志会重置，守护模式自动重新禁止)

ACTION="${1:-status}"
SHM_FILE="/dev/shm/app_running_list"
DAEMON_PID_FILE="/var/upgrade/.watchdog_guard_pid"
FORBIT_OFFSET=0x78

log_msg() {
    echo "[WD_GUARD] $1" > /dev/ttyprintk 2>/dev/null
}

get_sair_offset() {
    if [ ! -f "$SHM_FILE" ]; then
        return
    fi
    local hex_offset
    hex_offset=$(hexdump -C "$SHM_FILE" | grep -m1 "7361 6972" | awk '{print $1}')
    if [ -n "$hex_offset" ]; then
        echo $((16#$hex_offset))
    fi
}

do_forbid() {
    local offset=$(get_sair_offset)
    if [ -n "$offset" ]; then
        local target=$((offset + FORBIT_OFFSET))
        printf '\x01' | dd of="$SHM_FILE" bs=1 seek="$target" conv=notrunc 2>/dev/null
        return 0
    fi
    return 1
}

do_allow() {
    local offset=$(get_sair_offset)
    if [ -n "$offset" ]; then
        local target=$((offset + FORBIT_OFFSET))
        printf '\x00' | dd of="$SHM_FILE" bs=1 seek="$target" conv=notrunc 2>/dev/null
        return 0
    fi
    return 1
}

get_status() {
    local offset=$(get_sair_offset)
    if [ -n "$offset" ]; then
        local target=$((offset + FORBIT_OFFSET))
        local val
        val=$(dd if="$SHM_FILE" bs=1 skip="$target" count=1 2>/dev/null | hexdump -e '1/1 "%d"')
        echo "$val"
    else
        echo "-1"
    fi
}

stop_daemon() {
    if [ -f "$DAEMON_PID_FILE" ]; then
        local pid=$(cat "$DAEMON_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            echo "Daemon (PID $pid) stopped"
            log_msg "Daemon stopped"
        fi
        rm -f "$DAEMON_PID_FILE"
    fi
}

daemon_loop() {
    echo $$ > "$DAEMON_PID_FILE"
    log_msg "Daemon started (PID $$)"
    echo "Watchdog guard daemon started (PID $$)"
    echo "Will re-forbid watchdog every 10 seconds"
    echo "To stop: watchdog_guard.sh stop"

    while [ -f "$DAEMON_PID_FILE" ]; do
        pid=$(pidof sair 2>/dev/null || pidof xiaozhi-wakeup 2>/dev/null)
        if [ -n "$pid" ]; then
            local val=$(get_status)
            if [ "$val" != "1" ]; then
                if do_forbid; then
                    log_msg "Watchdog re-forbidden (sair PID $pid)"
                fi
            fi
        fi
        sleep 10
    done
    log_msg "Daemon exiting"
}

case "$ACTION" in
    enable|forbid)
        echo "Disabling software watchdog..."
        if do_forbid; then
            echo "Watchdog DISABLED (debug mode, no auto-reboot)"
            log_msg "Watchdog forbidden by user"
        else
            echo "Warning: Could not find sair in app_running_list"
            echo "  sair may not be running yet, try again later"
            echo "  Or use 'watchdog_guard.sh daemon' for auto-retry"
        fi
        ;;
    disable|allow)
        echo "Enabling software watchdog..."
        if do_allow; then
            echo "Watchdog ENABLED (normal mode)"
            log_msg "Watchdog allowed by user"
        else
            echo "Warning: Could not find sair in app_running_list"
        fi
        stop_daemon
        ;;
    status)
        echo "=== Watchdog Guard Status ==="
        pid=$(pidof sair 2>/dev/null || pidof xiaozhi-wakeup 2>/dev/null)
        if [ -n "$pid" ]; then
            echo "sair process: PID=$pid"
            val=$(get_status)
            if [ "$val" = "1" ]; then
                echo "Watchdog: DISABLED (debug mode)"
            elif [ "$val" = "0" ]; then
                echo "Watchdog: ENABLED (normal mode)"
            else
                echo "Watchdog: UNKNOWN (could not read shared memory)"
            fi
        else
            echo "sair not running"
        fi
        if [ -f "$DAEMON_PID_FILE" ]; then
            dpid=$(cat "$DAEMON_PID_FILE")
            if kill -0 "$dpid" 2>/dev/null; then
                echo "Daemon: RUNNING (PID $dpid)"
            else
                echo "Daemon: STALE (PID $dpid not running)"
                rm -f "$DAEMON_PID_FILE"
            fi
        else
            echo "Daemon: NOT RUNNING"
        fi
        ;;
    daemon)
        if [ -f "$DAEMON_PID_FILE" ]; then
            dpid=$(cat "$DAEMON_PID_FILE")
            if kill -0 "$dpid" 2>/dev/null; then
                echo "Daemon already running (PID $dpid), stopping first..."
                stop_daemon
                sleep 1
            else
                rm -f "$DAEMON_PID_FILE"
            fi
        fi
        daemon_loop &
        echo "Daemon launched in background"
        ;;
    stop)
        stop_daemon
        ;;
    *)
        echo "Usage: $0 {enable|disable|status|daemon|stop}"
        echo "  enable  - Disable watchdog (debug mode, no auto-reboot)"
        echo "  disable - Enable watchdog (normal mode)"
        echo "  status  - Show current watchdog state"
        echo "  daemon  - Run as daemon, continuously keep watchdog disabled"
        echo "  stop    - Stop the daemon"
        ;;
esac
