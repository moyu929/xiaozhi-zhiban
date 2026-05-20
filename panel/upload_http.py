import urllib.request, sys

binary_path = sys.argv[1] if len(sys.argv) > 1 else r'd:\小智ai\xiaozhi-zhiban-develop\device\xwebd\build\usb_helper'
filename = sys.argv[2] if len(sys.argv) > 2 else 'usb_helper'

with open(binary_path, 'rb') as f:
    data = f.read()

req = urllib.request.Request(
    'http://192.168.2.9:8080/api/upload',
    data=data,
    method='POST'
)
req.add_header('Content-Type', 'application/octet-stream')
req.add_header('X-Filename', filename)
req.add_header('Content-Length', str(len(data)))

try:
    r = urllib.request.urlopen(req, timeout=30)
    print(f"Upload OK: {r.read().decode()}")
except Exception as e:
    print(f"Upload failed: {e}")
