# -*- coding: utf-8 -*-
"""三模块综合测试 - panel 侧 (代理路由 + 静态资源 + 业务链路)"""
import json, urllib.request, time

PANEL = "127.0.0.1:3001"
results = []

def http(method, path, body=None, timeout=10):
    url = f"http://{PANEL}{path}"
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

def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'} | {name}" + (f" | {str(detail)[:110]}" if not ok else ""))

def jp(b):
    try:
        return json.loads(b)
    except Exception:
        return None

# ============ E. panel 基础与连接 ============
st, b = http("GET", "/api/info")
d = jp(b)
check("E1 GET /api/info LIVE+已连设备", st == 200 and d and d.get("xwebd_connected"), f"{st} {b[:80]}")

st, b = http("GET", "/api/status")
d = jp(b)
ok = st == 200 and d is not None and ("xwebd" in str(d) or "sair" in str(d) or "battery" in str(d))
check("E2 GET /api/status 聚合状态", ok, f"{st} {str(b)[:100]}")

# ============ F. 代理路由(只读) ============
ro = [
    ("F1 services", "/api/services"),
    ("F2 backlight", "/api/backlight"),
    ("F3 usb/mode", "/api/usb/mode"),
    ("F4 battery/status", "/api/plugin/battery/status"),
    ("F5 plugins", "/api/plugins"),
    ("F6 xwebd/diag", "/api/xwebd/diag"),
    ("F7 xwebd/version", "/api/xwebd/version"),
    ("F8 local-versions", "/api/local-versions"),
    ("F9 assistant/diag", "/api/assistant/diag"),
    ("F10 assistant/env", "/api/assistant/env"),
    ("F11 assistant/config", "/api/assistant/config"),
    ("F12 xwebd/config", "/api/xwebd/config"),
    ("F13 config", "/api/config"),
    ("F14 assistant/status", "/api/assistant/status"),
    ("F15 processes", "/api/processes"),
    ("F16 logs", "/api/logs?lines=20"),
    ("F17 panel/logs", "/api/panel/logs?lines=10"),
    ("F18 upload-progress", "/api/upload-progress"),
    ("F19 adb/devices", "/api/adb/devices"),
]
for name, path in ro:
    st, b = http("GET", path)
    ok = st == 200 and b and b[:1] in (b"{", b"[")
    check(name, ok, f"{st} {str(b)[:80]}")

# ============ G. 设备配置读写链路 (panel -> xwebd -> sair) ============
st, b = http("GET", "/api/assistant/config")
d = jp(b)
keys = sorted(d.keys()) if isinstance(d, dict) else []
need = ["ws_url", "volume", "screen_off_idle_sec", "boot_push_disable", "use_limit", "aec_mode", "listening_mode"]
missing = [k for k in need if k not in keys]
check("G1 assistant/config 字段完整(面板重构项)", not missing, f"缺={missing}")

# G2: 音量 round-trip
st, b = http("GET", "/api/assistant/config")
orig_vol = jp(b).get("volume", -1)
if orig_vol >= 0:
    new_vol = max(0, orig_vol - 1)
    st, b = http("PUT", "/api/assistant/config", json.dumps({"volume": new_vol}))
    time.sleep(1.5)
    st, b = http("GET", "/api/assistant/config")
    now_vol = jp(b).get("volume")
    ok1 = now_vol == new_vol
    http("PUT", "/api/assistant/config", json.dumps({"volume": orig_vol}))
    time.sleep(1.5)
    st, b = http("GET", "/api/assistant/config")
    ok2 = jp(b).get("volume") == orig_vol
    check("G2 音量读写 round-trip", ok1 and ok2, f"orig={orig_vol} set={new_vol} got={now_vol} 恢复={ok2}")
else:
    check("G2 音量读写 round-trip", False, f"volume 不可读: {orig_vol}")

# G3: screen_off_idle_sec round-trip
st, b = http("GET", "/api/assistant/config")
orig_so = jp(b).get("screen_off_idle_sec", -1)
http("PUT", "/api/assistant/config", json.dumps({"screen_off_idle_sec": 20}))
time.sleep(1.5)
st, b = http("GET", "/api/assistant/config")
ok1 = jp(b).get("screen_off_idle_sec") == 20
http("PUT", "/api/assistant/config", json.dumps({"screen_off_idle_sec": orig_so}))
time.sleep(1.5)
st, b = http("GET", "/api/assistant/config")
ok2 = jp(b).get("screen_off_idle_sec") == orig_so
check("G3 会话息屏秒数 round-trip", ok1 and ok2, f"orig={orig_so} ok1={ok1} ok2={ok2}")

# G4: use_limit round-trip
st, b = http("GET", "/api/assistant/config")
ul_orig = jp(b).get("use_limit", {})
http("PUT", "/api/assistant/config", json.dumps({"use_limit_enable": 0}))
time.sleep(1.5)
st, b = http("GET", "/api/assistant/config")
ok1 = jp(b).get("use_limit", {}).get("enable") == 0
http("PUT", "/api/assistant/config", json.dumps({"use_limit_enable": ul_orig.get("enable", 0)}))
time.sleep(1.5)
check("G4 use_limit 开关 round-trip", ok1, f"ok1={ok1}")

# G5: 背光滑条 round-trip (backlight PUT)
st, b = http("GET", "/api/backlight")
bl = jp(b)
orig_bl = bl.get("brightness", 0)
if orig_bl > 0:
    http("PUT", "/api/backlight", json.dumps({"enable": 1, "brightness": orig_bl}))
    time.sleep(1)
    st, b = http("GET", "/api/backlight")
    check("G5 背光亮度设置回读", st == 200 and jp(b).get("brightness") == orig_bl,
          f"{st} {str(b)[:80]}")
else:
    check("G5 背光亮度设置回读", False, f"brightness={orig_bl}")

# ============ H. 静态资源 ============
for name, path, must in [
    ("H1 首页 index.html", "/", b"<!DOCTYPE"),
    ("H2 app.js", "/app.js?v=33", b"refreshXwebdServices"),
    ("H3 style.css", "/style.css?v=26", b"settings-group-title"),
]:
    st, b = http("GET", path)
    check(name, st == 200 and must in b, f"{st} len={len(b)}")

st, b = http("GET", "/static/nonexistent.js")
check("H4 静态 404", st == 404, f"{st}")

# ============ I. 恶意/边界 ============
st, b = http("GET", "/api/files/download?path=/etc/passwd")
check("I1 越权路径拒绝", st in (400, 403, 404), f"{st}")

st, b = http("PUT", "/api/assistant/config", "not_json")
check("I2 畸形 JSON 不崩", st >= 400, f"{st}")

st, b = http("GET", "/api/assistant/status")
check("I3 畸形后服务仍正常", st == 200, f"{st}")

# ============ 汇总 ============
print("\n" + "=" * 50)
passed = sum(1 for _, ok, _ in results if ok)
print(f"panel 侧测试: {passed}/{len(results)} 通过")
fails = [(n, d) for n, ok, d in results if not ok]
if fails:
    print("失败项:")
    for n, d in fails:
        print(f"  - {n}: {str(d)[:100]}")
