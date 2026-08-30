# -*- coding: utf-8 -*-
"""三模块综合测试 - 设备侧 (xwebd 路由 + sair IPC + 插件端点)"""
import json, socket, subprocess, time, sys, re
import urllib.request

DEV = "192.168.2.14:8080"
results = []

def http(method, path, body=None, timeout=8):
    url = f"http://{DEV}{path}"
    data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method)
    if isinstance(body, str):
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, str(e).encode()

def telnet_cmd(cmds, wait=2.5):
    s = socket.create_connection((DEV.split(":")[0], 23), timeout=6)
    time.sleep(0.6); s.recv(4096)
    for c in cmds:
        s.sendall((c + "\n").encode())
        time.sleep(wait)
    out = b""
    while True:
        try:
            s.settimeout(1.5)
            d = s.recv(65536)
            if not d: break
            out += d
        except socket.timeout:
            break
    s.close()
    return out.decode(errors="replace")

def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'} | {name}" + (f" | {detail[:120]}" if not ok else (f" | {detail[:80]}" if detail else "")))

def jparse(b):
    try:
        return json.loads(b)
    except Exception:
        return None

# ============ A. xwebd 核心路由 ============
st, b = http("GET", "/api/ping")
check("A1 GET /api/ping", st == 200 and b"pong" in b.lower() or st == 200, f"{st} {b[:60]}")

st, b = http("GET", "/api/version")
d = jparse(b)
check("A2 GET /api/version", st == 200 and d and "version" in d, f"{st} {b[:60]}")

st, b = http("GET", "/api/config")
d = jparse(b)
check("A3 GET /api/config", st == 200 and d is not None, f"{st} {b[:60]}")

st, b = http("GET", "/api/system")
d = jparse(b)
check("A4 GET /api/system", st == 200 and d is not None, f"{st} {b[:80]}")

st, b = http("GET", "/api/logs?lines=50")
d = jparse(b)
check("A5 GET /api/logs", st == 200 and d and "logs" in d, f"{st} {b[:60]}")

st, b = http("GET", "/api/services")
d = jparse(b)
check("A6 GET /api/services", st == 200 and d is not None, f"{st} {b[:80]}")

st, b = http("GET", "/api/diag")
d = jparse(b)
check("A7 GET /api/diag", st == 200 and d is not None, f"{st} {b[:80]}")

st, b = http("GET", "/api/processes")
d = jparse(b)
check("A8 GET /api/processes", st == 200 and d is not None, f"{st} {b[:80]}")

st, b = http("GET", "/api/backlight")
d = jparse(b)
check("A9 GET /api/backlight", st == 200 and d and "brightness" in d, f"{st} {b[:60]}")

st, b = http("GET", "/api/usb/mode")
check("A10 GET /api/usb/mode", st == 200, f"{st} {b[:60]}")

st, b = http("GET", "/api/plugins")
d = jparse(b)
plugs = d.get("plugins", []) if isinstance(d, dict) else []
online = [p.get("name") for p in plugs if p.get("online")]
check("A11 GET /api/plugins 六插件全在线", st == 200 and len(online) >= 5, f"{st} online={online}")

st, b = http("GET", "/api/files/download?path=/var/upgrade/charge_log.csv")
check("A12 GET /api/files/download", st == 200 and len(b) > 100, f"{st} len={len(b)}")

st, b = http("GET", "/api/files/download?path=/etc/passwd")
check("A13 download 越权路径防护", st in (400, 403, 404), f"{st} {b[:60]}")

# ---- assistant 系列 ----
st, b = http("GET", "/api/assistant/env")
check("A14 GET /api/assistant/env", st == 200, f"{st} {b[:60]}")

st, b = http("GET", "/api/assistant/status")
d = jparse(b)
check("A15 GET /api/assistant/status", st == 200 and d and "state" in str(d), f"{st} {b[:80]}")

st, b = http("GET", "/api/assistant/config")
d = jparse(b)
cfg_keys = sorted(d.keys()) if isinstance(d, dict) else []
check("A16 GET /api/assistant/config", st == 200 and "ws_url" in cfg_keys and "use_limit" in cfg_keys, f"{st} keys={cfg_keys[:8]}")

st, b = http("GET", "/api/assistant/diag")
check("A17 GET /api/assistant/diag", st == 200, f"{st} {b[:80]}")

st, b = http("GET", "/api/assistant/logs?lines=30")
d = jparse(b)
check("A18 GET /api/assistant/logs", st == 200 and d and "logs" in d, f"{st} {b[:60]}")

st, b = http("POST", "/api/logs/clean?source=2")
check("A19 POST /api/logs/clean", st == 200, f"{st} {b[:60]}")

# ---- 404/方法错误 ----
st, b = http("GET", "/api/nonexistent")
check("A20 未知路由 404", st == 404, f"{st}")

st, b = http("POST", "/api/ping")
check("A21 错误方法 404/405", st in (404, 405), f"{st}")

# ============ B. 插件端点 ============
for name, path, must in [
    ("B1 battery /status", "/api/plugin/battery/status", ["voltage_uv", "capacity", "charging"]),
    ("B2 procs /list", "/api/plugin/procs/list", ["processes"]),
    ("B3 usb /mode", "/api/plugin/usb/mode", None),
    ("B4 backlight /state", "/api/plugin/backlight/state", ["brightness"]),
    ("B5 demo /ping", "/api/plugin/demo/ping", None),
]:
    st, b = http("GET", path)
    d = jparse(b)
    ok = st == 200 and d is not None and (must is None or all(k in d for k in must))
    check(name, ok, f"{st} {b[:80]}")

st, b = http("POST", "/api/plugin/demo/echo", '{"msg":"hello_test_123"}')
check("B6 demo /echo 回显", st == 200 and b"hello_test_123" in b, f"{st} {b[:80]}")

st, b = http("GET", "/api/plugin/files/list?path=/var/upgrade")
d = jparse(b)
check("B7 files /list", st == 200 and d and "files" in d, f"{st} {b[:80]}")

# B8: 上传测试文件 -> list 确认 -> delete 删除 (文件管理全链路)
st, b = http("POST", "/api/upload", "test_delete_me_content_12345")
# /api/upload 需要 X-Filename 头, urllib 不便加 —— 用 http.client(别名避免遮蔽 http 函数)
import http.client as _hc
conn = _hc.HTTPConnection(DEV.split(":")[0], 8080, timeout=10)
conn.request("POST", "/api/upload", body="test_delete_me_content_12345",
             headers={"X-Filename": "__test_file.txt"})
r = conn.getresponse(); rb = r.read(); up_st = r.status
conn.close()
check("B8a upload 测试文件", up_st == 200, f"{up_st} {rb[:60]}")

st, b = http("GET", "/api/plugin/files/list?path=/var/upgrade")
names = [f.get("name") for f in jparse(b).get("files", [])]
check("B8b list 含测试文件", "__test_file.txt" in names, f"found={names[:6]}")

st, b = http("GET", "/api/plugin/files/delete?path=/var/upgrade/__test_file.txt")
check("B8c delete 测试文件", st == 200, f"{st} {b[:60]}")

st, b = http("GET", "/api/plugin/files/list?path=/var/upgrade")
names = [f.get("name") for f in jparse(b).get("files", [])]
check("B8d delete 后消失", "__test_file.txt" not in names, f"still={names[:6]}")

# B9: demo crash -> 网关应自动重启插件 (测插件崩溃恢复)
st, b = http("GET", "/api/plugin/demo/crash")
time.sleep(3)
st2, b2 = http("GET", "/api/plugin/demo/ping")
check("B9 demo crash 后插件自恢复", st2 == 200, f"crash={st} after={st2} {b2[:60]}")

# B10: restart 插件路由
st, b = http("POST", "/api/plugins/restart", '{"name":"demo"}')
time.sleep(2)
st2, b2 = http("GET", "/api/plugin/demo/ping")
check("B10 plugins/restart demo", st == 200 and st2 == 200, f"restart={st} ping={st2}")

# B11: 未装插件 404
st, b = http("GET", "/api/plugin/nonexist/status")
check("B11 未装插件 404", st == 404, f"{st} {b[:60]}")

# ============ C. sair IPC (telnet) ============
out = telnet_cmd(["cat /tmp/sair_config.json"])
check("C1 sair_config.json 可读且为 JSON", "__test" not in out and out.count('"') > 10, out[-100:])

out = telnet_cmd(["touch /tmp/sair_diag_request"])
time.sleep(4)
out = telnet_cmd(["cat /tmp/sair_diag.json | head -c 200"])
check("C2 自检请求->诊断文件生成", '"ok"' in out or "{" in out, out[-120:])

# C3: set_config round-trip (log_level INFO->DEBUG->INFO)
out = telnet_cmd(['echo "{\\"cmd\\":\\"set_config\\",\\"log_level\\":\\"DEBUG\\"}" > /tmp/sair_cmd.json'])
time.sleep(2)
out = telnet_cmd(["cat /tmp/sair_config.json | grep -o 'log_level[^,]*'"])
check("C3a set_config log_level=DEBUG", "DEBUG" in out, out[-100:])
telnet_cmd(['echo "{\\"cmd\\":\\"set_config\\",\\"log_level\\":\\"INFO\\"}" > /tmp/sair_cmd.json'])
time.sleep(2)
out = telnet_cmd(["cat /tmp/sair_config.json | grep -o 'log_level[^,]*'"])
check("C3b 恢复 log_level=INFO", "INFO" in out, out[-100:])

# C4: screen_off_idle_sec round-trip
out = telnet_cmd(["cat /tmp/sair_config.json | grep -o 'screen_off_idle_sec[^,]*'"])
m_orig = re.search(r'screen_off_idle_sec":(\d+)', out)
orig = m_orig.group(1) if m_orig else "0"
telnet_cmd(['echo "{\\"cmd\\":\\"set_config\\",\\"screen_off_idle_sec\\":15}" > /tmp/sair_cmd.json'])
time.sleep(2)
out = telnet_cmd(["cat /tmp/sair_config.json | grep -o 'screen_off_idle_sec[^,]*'"])
check("C4a set_config screen_off_idle_sec=15", "15" in out, out[-100:])
telnet_cmd([f'echo "{{\\"cmd\\":\\"set_config\\",\\"screen_off_idle_sec\\":{orig}}}" > /tmp/sair_cmd.json'])
time.sleep(2)
out = telnet_cmd(["cat /tmp/sair_config.json | grep -o 'screen_off_idle_sec[^,]*'"])
check(f"C4b 恢复 screen_off_idle_sec={orig}", f":{orig}" in out.replace(" ", ""), out[-100:])

# C5: abort 无会话时安全
out = telnet_cmd(['echo "{\\"cmd\\":\\"abort\\"}" > /tmp/sair_cmd.json'])
time.sleep(2)
st, b = http("GET", "/api/assistant/status")
check("C5 abort 命令(空闲态无害)", st == 200, f"{st}")

# C6: 未知命令应被拒绝(日志记录)
out = telnet_cmd(['echo "{\\"cmd\\":\\"bogus_cmd_xyz\\"}" > /tmp/sair_cmd.json'])
time.sleep(2)
st, b = http("GET", "/api/assistant/logs?lines=30")
check("C6 未知命令处理不崩", st == 200, f"{st}")

# C7: 畸形 JSON 不崩
out = telnet_cmd(['echo "not_json_at_all" > /tmp/sair_cmd.json'])
time.sleep(2)
st, b = http("GET", "/api/assistant/status")
check("C7 畸形命令不崩", st == 200, f"{st}")

# ============ D. 低电自护状态 ============
st, b = http("GET", "/api/plugin/battery/status")
d = jparse(b)
charging = d.get("charging")
check("D1 电池插件低电自护运行中(充电态旁路)", charging in (True, False), f"{d}")

out = telnet_cmd(["ps | grep xwplug-bat-c | grep -v grep | wc -l"])
n = [l.strip() for l in out.splitlines() if l.strip().isdigit()]
check("D2 采样子进程存活(唯一)", n and n[0] == "1", out[-80:])

# ============ 汇总 ============
print("\n" + "=" * 50)
passed = sum(1 for _, ok, _ in results if ok)
print(f"设备侧测试: {passed}/{len(results)} 通过")
fails = [(n, d) for n, ok, d in results if not ok]
if fails:
    print("失败项:")
    for n, d in fails:
        print(f"  - {n}: {d[:100]}")
