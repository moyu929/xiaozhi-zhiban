"""
Check device status after sair kill
"""
import telnetlib
import time
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from config_loader import get_config

cfg = get_config()

DEVICE = cfg["device_ip"]
PORT = cfg["device_telnet_port"]
TIMEOUT = 10

def run_cmd(tn, cmd, wait=2):
    tn.write(f"{cmd}\n".encode('ascii'))
    time.sleep(wait)
    return tn.read_very_eager().decode('ascii', errors='replace')

try:
    tn = telnetlib.Telnet(DEVICE, PORT, TIMEOUT)
    time.sleep(1)
    tn.read_very_eager()
    
    print("=" * 60)
    print("Device Status Check")
    print("=" * 60)
    
    out = run_cmd(tn, "uptime")
    print(f"Uptime: {out}")
    
    out = run_cmd(tn, "ps | grep sair | grep -v grep")
    print(f"Sair: {out}")
    
    out = run_cmd(tn, "ps | grep manager | grep -v grep")
    print(f"Manager: {out}")
    
    out = run_cmd(tn, "dmesg | grep -iE 'reboot|restart|watchdog' | tail -10")
    print(f"Recent logs: {out}")
    
    tn.close()
    
except Exception as e:
    print(f"Connection failed: {e}")
    print("Device may have rebooted")
