import telnetlib
import time

tn = telnetlib.Telnet('192.168.2.14', 23, timeout=5)
tn.read_until(b'# ', timeout=3)
cmd = "cat /var/upgrade/xiaozhi.log | grep -E 'WAKEUP|ASR|Idle|cooldown|startup|protect|guard' | tail -60\n"
tn.write(cmd.encode())
time.sleep(2)
print(tn.read_very_eager().decode('utf-8', errors='replace'))
tn.close()
