import telnetlib, time, sys, struct

tn = telnetlib.Telnet('192.168.2.9', 23, timeout=5)
time.sleep(1)
tn.read_very_eager()

binary_path = sys.argv[1] if len(sys.argv) > 1 else r'd:\小智ai\xiaozhi-zhiban-develop\device\xwebd\build\usb_helper'
remote_path = sys.argv[2] if len(sys.argv) > 2 else '/tmp/usb_helper'

with open(binary_path, 'rb') as f:
    data = f.read()

print(f"Uploading {len(data)} bytes to {remote_path}...")

tn.write(('rm -f ' + remote_path + '\n').encode())
time.sleep(0.3)
tn.read_very_eager()

chunk_size = 256
offset = 0
while offset < len(data):
    chunk = data[offset:offset + chunk_size]
    hex_str = chunk.hex()
    cmd = f'printf "%s" "$(echo {hex_str} | busybox xxd -r -p)" >> {remote_path}\n'
    tn.write(cmd.encode())
    offset += chunk_size
    time.sleep(0.05)
    if offset % 4096 == 0:
        tn.read_very_eager()
        print(f"  {offset}/{len(data)} bytes...")

time.sleep(1)
tn.read_very_eager()

tn.write(('chmod +x ' + remote_path + '; ls -la ' + remote_path + '; echo ===END===\n').encode())
time.sleep(2)
result = tn.read_very_eager().decode('utf-8', errors='replace')
print(result)
tn.close()
