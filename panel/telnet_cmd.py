import telnetlib, time, sys

tn = telnetlib.Telnet('192.168.2.9', 23, timeout=5)
time.sleep(1)
tn.read_very_eager()

cmds = sys.argv[1] if len(sys.argv) > 1 else 'echo hello'
tn.write((cmds + '; echo ===END===\n').encode())
time.sleep(3)
data = tn.read_very_eager().decode('utf-8', errors='replace')
print(data)
tn.close()
