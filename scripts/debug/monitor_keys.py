import telnetlib
import time
import signal
import sys

DEVICE_HOST = '192.168.2.14'
DEVICE_PORT = 23

tn = telnetlib.Telnet(DEVICE_HOST, DEVICE_PORT, timeout=5)
tn.read_until(b'# ', timeout=3)

print("=== 停止 sair 进程 ===")
tn.write(b'kill $(pidof sair) 2>/dev/null\n')
time.sleep(2)
tn.read_very_eager()

print("=== 监测 /dev/input/event2 ===")
print("请按设备上的音量加减键 (15秒内)...")
print("input_event: time(8B) type(2B) code(2B) value(4B) = 16 bytes")
print("")

tn.write(b'hexdump -C /dev/input/event2\n')
time.sleep(0.5)

start = time.time()
collected = b''
while time.time() - start < 15:
    time.sleep(0.2)
    data = tn.read_very_eager()
    if data:
        collected += data
        text = data.decode('utf-8', errors='replace')
        if text.strip():
            print(text, end='')

tn.write(b'\x03')
time.sleep(0.5)
tn.read_very_eager()

print("")
print("=== 重启 sair ===")
tn.write(b'cd /var/upgrade && LD_LIBRARY_PATH=/usr/lib:/lib ./sair >> sair_boot.log 2>&1 &\n')
time.sleep(1)
tn.read_very_eager()
tn.close()
print("=== 完成 ===")
