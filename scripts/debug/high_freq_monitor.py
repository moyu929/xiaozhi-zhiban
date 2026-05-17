import telnetlib
import time
import threading
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

IP = cfg["device_ip"]
results = []
stop_flag = False
start_time = time.time()

def poll_device():
    global stop_flag
    while not stop_flag:
        try:
            tn = telnetlib.Telnet(IP, 23, timeout=3)
            tn.read_very_eager()
            
            tn.write(b'cat /proc/stat | head -5; echo "==="; cat /proc/meminfo | head -3; echo "==="; cat /proc/$(pidof sair)/stat 2>/dev/null; echo "==="; cat /proc/$(pidof sair)/status 2>/dev/null | grep -E "VmRSS|Threads|voluntary"; echo "==="; cat /proc/loadavg; echo "==="; cat /proc/$(pidof sair)/wchan 2>/dev/null; echo "===END"\n')
            time.sleep(1.5)
            data = tn.read_very_eager().decode('utf-8', 'replace')
            
            elapsed = time.time() - start_time
            entry = {
                't': round(elapsed, 1),
                'ts': time.strftime('%H:%M:%S'),
                'raw': data.strip()
            }
            results.append(entry)
            
            ts = time.strftime('%H:%M:%S')
            lines = [l.strip() for l in data.strip().split('\n') if l.strip() and 'BusyBox' not in l and '~ #' not in l and '===' not in l]
            mem_line = [l for l in lines if 'MemFree' in l]
            load_line = [l for l in lines if l[0:1].isdigit() and 'up' not in l]
            rss_line = [l for l in lines if 'VmRSS' in l]
            wchan_line = [l for l in lines if l and not any(k in l for k in ['cpu','Mem','VmR','Thre','volun','===','END','grep','cat'])]
            
            mem_free = mem_line[0].split(':')[1].strip() if mem_line else '?'
            load = load_line[0].split()[0:3] if load_line else '?'
            rss = rss_line[0].split(':')[1].strip() if rss_line else '?'
            
            print(f'[{ts} +{elapsed:.1f}s] load={load} memfree={mem_free} sair_rss={rss} wchan={wchan_line[0] if wchan_line else "?"}')
            
            tn.close()
        except Exception as e:
            elapsed = time.time() - start_time
            ts = time.strftime('%H:%M:%S')
            print(f'[{ts} +{elapsed:.1f}s] *** LOST: {e} ***')
            results.append({'t': round(elapsed, 1), 'ts': ts, 'raw': f'CONNECTION_LOST: {e}'})
        time.sleep(0.5)

t = threading.Thread(target=poll_device, daemon=True)
t.start()

print("=== High-frequency monitor started. Press Ctrl+C to stop. ===")
print("=== Say wake word NOW! ===")
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    stop_flag = True
    t.join(timeout=5)

out_file = os.path.join(os.path.dirname(__file__), 'monitor_result.json')
with open(out_file, 'w', encoding='utf-8') as f:
    json.dump(results, f, ensure_ascii=False, indent=1)
print(f"\nSaved {len(results)} samples to {out_file}")
