import telnetlib
import time

tn = telnetlib.Telnet('192.168.2.9', 23, timeout=10)
tn.read_until(b'# ', 5)

tn.write(b'readelf -d /var/upgrade/sair 2>/dev/null | grep NEEDED\n')
time.sleep(1)
out = tn.read_until(b'# ', 5).decode('utf-8', 'ignore')
print('=== sair NEEDED ===')
print(out.strip())

tn.write(b'readelf -d /var/upgrade/xwebd 2>/dev/null | grep NEEDED\n')
time.sleep(1)
out = tn.read_until(b'# ', 5).decode('utf-8', 'ignore')
print('=== xwebd NEEDED ===')
print(out.strip())

tn.write(b'readelf -s /var/upgrade/xwebd 2>/dev/null | grep ctype\n')
time.sleep(1)
out = tn.read_until(b'# ', 5).decode('utf-8', 'ignore')
print('=== xwebd ctype symbols ===')
print(out.strip())

tn.write(b'readelf -s /var/upgrade/sair 2>/dev/null | grep ctype\n')
time.sleep(1)
out = tn.read_until(b'# ', 5).decode('utf-8', 'ignore')
print('=== sair ctype symbols ===')
print(out.strip())

tn.close()
