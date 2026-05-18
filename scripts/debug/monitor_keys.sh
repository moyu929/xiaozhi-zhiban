#!/bin/sh
# 监测 /dev/input/event2 的按键事件
# 在设备上运行: sh /tmp/monitor_keys.sh
# 按 Ctrl+C 停止

echo "=== 监测 /dev/input/event2 按键事件 ==="
echo "请按设备上的音量加减键..."
echo "input_event 结构: time(8B) + type(2B) + code(2B) + value(4B) = 16字节"
echo ""

if [ -e /dev/input/event2 ]; then
    hexdump -C /dev/input/event2 2>/dev/null
else
    echo "错误: /dev/input/event2 不存在"
fi
