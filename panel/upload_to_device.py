import telnetlib, time, base64, sys

tn = telnetlib.Telnet('192.168.2.9', 23, timeout=5)
time.sleep(1)
tn.read_very_eager()

binary_path = sys.argv[1] if len(sys.argv) > 1 else r'd:\小智ai\xiaozhi-zhiban-develop\device\xwebd\build\usb_helper'
remote_path = sys.argv[2] if len(sys.argv) > 2 else '/tmp/usb_helper'

with open(binary_path, 'rb') as f:
    data = base64.b64encode(f.read()).decode()

tn.write(b'cat > /tmp/_upload_b64 << "EOFB64"\n')
time.sleep(0.5)
tn.read_very_eager()

chunk_size = 512
for i in range(0, len(data), chunk_size):
    chunk = data[i:i+chunk_size]
    tn.write((chunk + '\n').encode())
    time.sleep(0.05)

tn.write(b'EOFB64\n')
time.sleep(1)
tn.read_very_eager()

tn.write(('base64 -d /tmp/_upload_b64 > ' + remote_path + '; chmod +x ' + remote_path + '; ls -la ' + remote_path + '; rm /tmp/_upload_b64; echo ===END===\n').encode())
time.sleep(2)
print(tn.read_very_eager().decode('utf-8', errors='replace'))
tn.close()
