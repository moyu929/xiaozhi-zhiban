var S = {
    mode: 'wired',
    adb: { serial: null, connected: false },
    wl: { host: '', connected: false, xwebd: false, sair: false },
    currentPath: '/var/upgrade',
    busy: false,
    timers: { status: null, adbInfo: null, reboot: null, panelLogPoll: null },
    rebootStart: 0,
    confirmResolve: null,
    panelSSE: null,
    deviceSSE: {},
    xwebdVersion: '',
    localVersions: {}
};

/* 设备助手固件内置 MCP 工具（与 sair mcp_handler.c 同步维护，只读参考） */
var MCP_TOOLS = [
    { name: 'self.get_device_status', desc: '查设备状态：电量、音量、CPU、内存' },
    { name: 'self.audio_speaker.volume_up', desc: '音量调大一格（原生音量条）' },
    { name: 'self.audio_speaker.volume_down', desc: '音量调小一格（原生音量条）' },
    { name: 'self.get_system_info', desc: '查版本、音量、电量等系统信息' },
    { name: 'self.clean_junk', desc: '清理临时文件与缓存，释放内存' },
    { name: 'self.get_mcp_tools', desc: '列出全部可用 MCP 工具' },
    { name: 'self.reboot', desc: '重启设备（仅响应明确的"重启"指令）' },
    { name: 'self.poweroff', desc: '关机（原生关机动画，"关机/不玩了"）' },
    { name: 'self.limit_info', desc: '查今日使用时长：已用/上限/剩余' },
    { name: 'self.limit_delay', desc: '达限后延长使用 N 分钟（需家长开启）' },
    { name: 'self.screen_off_set', desc: '设置会话中自动息屏秒数（0=不息屏）' },
    { name: 'self.screen_off_now', desc: '立即息屏，语音对话继续' },
    { name: 'self.screen_on_now', desc: '亮屏（"亮屏/打开屏幕"）' }
];

/* 已知插件的中文名与用途说明（未知插件显示原名称） */
var PLUGIN_META = {
    files:     { cn: '文件管理',   desc: '文件列表/删除/批量清理（上传下载为核心功能，装插件才有列表管理）' },
    procs:     { cn: '进程管理',   desc: '进程清单查看与启停控制（不装则无进程管理）' },
    usb:       { cn: 'USB 模式',   desc: 'USB 存储模式查询与重选、存储卡开关（不装则无 USB 控制）' },
    backlight: { cn: '背光控制',   desc: '屏幕背光调节与开机恢复（不装则设备状态卡的背光滑条不可用）' },
    battery:   { cn: '电池守护',   desc: '电池采样记录 + 伪低电关机守卫（建议常驻）', must: true },
    demo:      { cn: '框架演示',   desc: '插件框架演示与崩溃隔离测试，可安全卸载' }
};

var LOG = {
    xwebd: { clearLine: -1, lastCount: 0 },
    assistant: { clearLine: -1, lastCount: 0 },
    panel: { clearLine: -1, lastCount: 0 }
};

var STATE_MAP = {
    'Idle': '空闲',
    'Connecting': '连接中',
    'Listening': '监听中',
    'Speaking': '说话中',
    'Cleaning': '清理中',
    'Starting': '启动中',
    'Activating': '激活中'
};

// ==================== Utilities ====================

function $(id) { return document.getElementById(id); }

function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function stripAnsi(s) {
    return s.replace(/\x1b\[[0-9;]*m/g, '').replace(/\[[\d;]*m/g, function(m) {
        if (/^\[\d+m$/.test(m)) return '';
        return m;
    });
}

function toast(msg, type) {
    type = type || 'info';
    var el = $('toast');
    el.textContent = msg;
    el.className = 'toast ' + type + ' toast-show';
    el.style.display = 'flex';
    clearTimeout(el._t);
    el._t = setTimeout(function() {
        el.className = 'toast ' + type + ' toast-hide';
        el._t2 = setTimeout(function() { el.style.display = 'none'; }, 400);
    }, 2500);
}

function flashEl(el) {
    if (!el) return;
    el.classList.add('flash');
    setTimeout(function() { el.classList.remove('flash'); }, 400);
}

function showOverlay(id) { document.getElementById(id).style.display = 'flex'; }
function hideOverlay(id) { document.getElementById(id).style.display = 'none'; }

async function api(path, opts) {
    opts = opts || {};
    var controller = new AbortController();
    var timer = setTimeout(function() { controller.abort(); }, 15000);
    try {
        var r = await fetch(path, {
            method: opts.method || 'GET',
            headers: opts.headers || {},
            body: opts.body || undefined,
            signal: controller.signal,
        });
        clearTimeout(timer);
        var data = await r.json();
        return data;
    } catch (e) {
        clearTimeout(timer);
        if (e.name === 'AbortError') return { ok: false, error: '请求超时' };
        return { ok: false, error: e.message };
    }
}

// ==================== Confirm Dialog ====================

function copyToClipboard(text) {
    if (!text) return;
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text);
    } else {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
    }
}

function renderActivationCode(code, activated) {
    var el = $('assistantActivation');
    if (!el) return;
    if (activated && code) {
        el.innerHTML = '<span class="activation-code-wrap"><span class="code-text">' + escapeHtml(code) + '</span>' +
            '<button class="btn-copy" data-code="' + escapeHtml(code).replace(/"/g, '&quot;') + '">复制</button></span>';
    } else if (activated) {
        el.innerHTML = '<span class="activation-code-wrap"><span style="color:var(--accent)">已激活</span></span>';
    } else if (code) {
        el.innerHTML = '<span class="activation-code-wrap"><span class="code-text">' + escapeHtml(code) + '</span>' +
            '<button class="btn-copy" data-code="' + escapeHtml(code).replace(/"/g, '&quot;') + '">复制</button></span>';
    } else {
        el.textContent = '未激活';
        el.style.color = 'var(--text-muted)';
    }
}

function copyActivationCode(btn) {
    var code = btn.getAttribute('data-code');
    if (!code) return;
    copyToClipboard(code);
    btn.textContent = '已复制';
    btn.classList.add('copied');
    setTimeout(function() { btn.textContent = '复制'; btn.classList.remove('copied'); }, 1500);
    toast('激活码已复制到剪贴板', 'success');
}

// ==================== MCP Tools Management ====================

function renderMcpTools() {
    var container = $('mcpToolsList');
    if (!container) return;
    var html = '';
    for (var i = 0; i < MCP_TOOLS.length; i++) {
        var t = MCP_TOOLS[i];
        html += '<div class="mcp-tool-item" title="' + escapeHtml(t.name) + '">';
        html += '<span class="mcp-tool-name">' + escapeHtml(t.name) + '</span>';
        html += '<span class="mcp-tool-desc">' + escapeHtml(t.desc) + '</span>';
        html += '</div>';
    }
    container.innerHTML = html;
}

function showConfirm(msg, opts) {
    opts = opts || {};
    return new Promise(function(resolve) {
        S.confirmResolve = resolve;
        $('confirmMessage').textContent = msg;
        $('confirmIcon').textContent = opts.icon || '⚠️';
        var okBtn = $('confirmOk');
        okBtn.textContent = opts.okText || '确定';
        okBtn.className = opts.danger ? 'btn btn-danger' : 'btn btn-primary';
        $('confirmOverlay').style.display = 'flex';
    });
}

function resolveConfirm(result) {
    $('confirmOverlay').style.display = 'none';
    if (S.confirmResolve) {
        S.confirmResolve(result);
        S.confirmResolve = null;
    }
}

// ==================== Particles ====================

(function initParticles() {
    var canvas = document.getElementById('particles');
    if (!canvas) return;
    var ctx = canvas.getContext('2d');
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    var particles = [];
    for (var i = 0; i < 120; i++) {
        particles.push({
            x: Math.random() * canvas.width,
            y: Math.random() * canvas.height,
            r: Math.random() * 2.5 + 0.5,
            alpha: Math.random() * 0.4 + 0.05,
            speed: Math.random() * 0.3 + 0.1,
            phase: Math.random() * Math.PI * 2,
        });
    }
    function animate() {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        var t = Date.now() * 0.001;
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            var a = p.alpha * (0.3 + 0.7 * Math.sin(t * p.speed * 5 + p.phase));
            ctx.beginPath();
            ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
            ctx.fillStyle = 'rgba(50, 240, 140, ' + a + ')';
            ctx.fill();
        }
        requestAnimationFrame(animate);
    }
    animate();
    window.addEventListener('resize', function() {
        canvas.width = window.innerWidth;
        canvas.height = window.innerHeight;
    });
})();

// ==================== Mode Switching ====================

function toggleMode() {
    S.mode = S.mode === 'wired' ? 'wireless' : 'wired';
    applyMode();
}

function applyMode() {
    var track = $('modeTrack');
    var slider = $('panelsSlider');
    if (S.mode === 'wireless') {
        track.classList.add('wireless');
        slider.classList.add('show-wireless');
    } else {
        track.classList.remove('wireless');
        slider.classList.remove('show-wireless');
    }
    updateViewportHeight();
}

function updateViewportHeight() {
    var viewport = document.querySelector('.panels-viewport');
    var pageId = S.mode === 'wired' ? 'wiredPage' : 'wirelessPage';
    var page = $(pageId);
    if (viewport && page) {
        viewport.style.height = page.scrollHeight + 'px';
    }
}

// ==================== Formatting ====================

function formatUptime(seconds) {
    if (!seconds && seconds !== 0) return '--';
    var d = Math.floor(seconds / 86400);
    var h = Math.floor((seconds % 86400) / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return d + '天' + h + '时';
    if (h > 0) return h + '时' + m + '分';
    return m + '分钟';
}

function formatMem(freeKb, totalKb, cachedKb) {
    if (!totalKb) return '--';
    var usedMb = Math.round((totalKb - freeKb - (cachedKb || 0)) / 1024);
    var totalMb = Math.round(totalKb / 1024);
    return usedMb + '/' + totalMb + ' MB';
}

function formatDisk(usedKb, totalKb) {
    if (!totalKb) return '--';
    return Math.round(usedKb / 1024) + '/' + Math.round(totalKb / 1024) + ' MB';
}

function formatFileTime(t) {
    if (!t) return '--';
    var d = typeof t === 'number' ? new Date(t * 1000) : new Date(t);
    if (isNaN(d.getTime())) return t;
    var pad = function(n) { return n < 10 ? '0' + n : n; };
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}

function formatSize(bytes) {
    if (!bytes && bytes !== 0) return '--';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / 1024 / 1024).toFixed(1) + ' MB';
}

// ==================== Log Rendering ====================

function getLogClass(l) {
    if (l.indexOf('[E]') >= 0 || l.indexOf('ERROR') >= 0 || l.indexOf('CRITICAL') >= 0) return 'log-error';
    if (l.indexOf('[W]') >= 0 || l.indexOf('WARNING') >= 0 || l.indexOf('WARN') >= 0) return 'log-warn';
    if (l.indexOf('[D]') >= 0 || l.indexOf('DEBUG') >= 0) return 'log-debug';
    if (l.indexOf('[I]') >= 0 || l.indexOf('INFO') >= 0) return 'log-info';
    return 'log-info';
}

function renderLogLine(l) {
    var clean = stripAnsi(l);
    var m = clean.match(/^(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})\s+(DEBUG|INFO|WARNING|WARN|ERROR|CRITICAL)\s+(\S+):\s+(.*)$/);
    if (m) {
        var dateHtml = '<span style="color:#6a9955">' + escapeHtml(m[1]) + '</span>';
        var levelColors = {DEBUG:'#569cd6', INFO:'#d4d4d4', WARNING:'#e5c07b', WARN:'#e5c07b', ERROR:'#f44747', CRITICAL:'#ff4cff'};
        var lc = levelColors[m[2]] || '#d4d4d4';
        var levelHtml = '<span style="color:' + lc + ';font-weight:600">' + escapeHtml(m[2]) + '</span>';
        var sourceHtml = '<span style="color:#dcdcaa">' + escapeHtml(m[3]) + '</span>';
        var msgHtml = '<span style="color:' + lc + '">' + escapeHtml(m[4]) + '</span>';
        return dateHtml + ' ' + levelHtml + ' ' + sourceHtml + ': ' + msgHtml;
    }
    var cls = getLogClass(clean);
    var colors = {logInfo:'#d4d4d4', logError:'#f44747', logWarn:'#e5c07b', logDebug:'#569cd6', logCritical:'#ff4cff'};
    return '<span style="color:' + (colors[cls] || '#d4d4d4') + '">' + escapeHtml(clean) + '</span>';
}

// ==================== ADB (Wired Mode) ====================

async function adbDetect() {
    var btn = $('btnAdbDetect');
    btn.disabled = true;
    btn.textContent = '扫描中...';
    var r = await api('/api/adb/devices');
    btn.disabled = false;
    btn.textContent = '扫描设备';

    if (r.error) {
        $('adbDeviceList').innerHTML = '<div class="empty-state">ADB 不可用：' + escapeHtml(r.error) + '</div>';
        toast('ADB 不可用', 'error');
        return;
    }

    var devices = r.devices || [];
    if (!devices.length) {
        $('adbDeviceList').innerHTML = '<div class="empty-state">未检测到设备<br><small>请确认：①USB线支持数据传输 ②设备已开启USB调试 ③设备已授权此电脑</small></div>';
        toast('未检测到ADB设备', 'info');
        return;
    }

    var html = '';
    for (var i = 0; i < devices.length; i++) {
        var d = devices[i];
        var selected = S.adb.serial === d.serial ? ' selected' : '';
        var stateLabel = (d.state === 'device' || !d.state || d.state === 'unknown') ? '' : (d.state === 'unauthorized' ? '（未授权，请在设备上确认）' : '（' + d.state + '）');
        var stateCls = (d.state === 'device' || !d.state || d.state === 'unknown') ? '' : ' svc-off';
        var xwebdLabel = d.xwebd_installed ? (d.xwebd_status && d.xwebd_status.running ? '内核运行中' : '内核已安装') : '未安装内核';
        var xwebdCls = d.xwebd_installed ? (d.xwebd_status && d.xwebd_status.running ? 'svc-on' : 'svc-off') : 'svc-unknown';
        var initLabel = '';
        if (d.state === 'device' || d.state === 'unknown' || !d.state) {
            if (!(d.initialized || {}).initialized) initLabel = ' · 新设备';
        }
        var model = (d.model && d.model !== 'unknown') ? d.model : ((d.device && d.device !== 'unknown') ? d.device : null);
        var deviceName = model || d.serial;
        var deviceSub = model ? d.serial : '';
        var clickable = (d.state === 'device' || d.state === 'unknown' || !d.state) ? '" data-serial="' + escapeHtml(d.serial) + '"' : ' device-unauthorized"';
        html += '<div class="adb-device-item' + selected + clickable + '">';
        html += '<div class="adb-device-info">';
        html += '<div class="adb-device-serial">' + escapeHtml(deviceName) + '</div>';
        if (deviceSub) html += '<div class="adb-device-model">' + escapeHtml(deviceSub) + stateLabel + '</div>';
        else html += '<div class="adb-device-model">' + stateLabel + '</div>';
        html += '</div>';
        html += '<div class="adb-device-status-col">';
        if (d.state === 'device' || d.state === 'unknown' || !d.state) {
            html += '<div class="adb-device-status svc-status ' + xwebdCls + '">' + xwebdLabel + initLabel + '</div>';
        } else {
            html += '<div class="adb-device-status svc-status' + stateCls + '">' + d.state + '</div>';
        }
        html += '</div></div>';
    }
    $('adbDeviceList').innerHTML = html;

    var items = $('adbDeviceList').querySelectorAll('.adb-device-item[data-serial]');
    for (var idx = 0; idx < items.length; idx++) {
        items[idx].addEventListener('click', (function(el) {
            return function() { selectAdbDevice(el.getAttribute('data-serial')); };
        })(items[idx]));
    }

    if (!S.adb.serial && devices.length === 1) selectAdbDevice(devices[0].serial);
    toast('检测到 ' + devices.length + ' 台设备', 'success');
    updateViewportHeight();
}

function adbDisconnect() {
    if (S.timers.adbInfo) { clearInterval(S.timers.adbInfo); S.timers.adbInfo = null; }
    S.adb.serial = null;
    S.adb.connected = false;
    $('btnAdbDisconnect').style.display = 'none';
    $('adbDeviceInfo').style.display = 'none';
    $('adbXwebdStatus').textContent = '未检测';
    $('adbXwebdStatus').className = 'svc-status svc-unknown';
    $('adbSairStatus').textContent = '未检测';
    $('adbSairStatus').className = 'svc-status svc-unknown';
    $('btnAdbDeployXwebd').style.display = '';
    $('btnAdbStartXwebd').style.display = 'none';
    $('btnAdbRestartXwebd').style.display = 'none';
    $('btnAdbRemoveXwebd').style.display = 'none';
    $('wiredDiagContainer').innerHTML = '<div class="empty-state">部署前点击「运行自检」检测设备是否满足 xwebd 运行环境</div>';
    $('adbDeviceList').innerHTML = '<div class="empty-state">点击「扫描设备」检测通过USB连接的设备</div>';
    toast('已断开设备连接', 'info');
}

async function selectAdbDevice(serial) {
    if (S.wl.connected) {
        stopPolling();
        S.wl.connected = false;
        S.wl.xwebd = false;
        S.wl.sair = false;
        updateConnUI(false);
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        $('btnConnect').textContent = '连接';
        $('btnConnect').className = 'btn btn-primary';
        $('btnConnect').disabled = false;
        resetWirelessUI();
    }
    S.adb.serial = serial;
    S.adb.connected = true;
    $('btnAdbDisconnect').style.display = '';
    var items = document.querySelectorAll('.adb-device-item');
    for (var i = 0; i < items.length; i++) {
        items[i].classList.toggle('selected', items[i].getAttribute('data-serial') === serial);
    }
    await adbRefreshStatus();
    await adbRefreshDeviceInfo();
    await adbRefreshLogs();
    if (S.timers.adbInfo) clearInterval(S.timers.adbInfo);
    S.timers.adbInfo = setInterval(adbRefreshDeviceInfo, 10000);
}

async function adbRefreshStatus() {
    if (!S.adb.serial) return;
    var r = await api('/api/adb/sair-status?serial=' + encodeURIComponent(S.adb.serial));
    if (r.error) return;

    var sair = r.sair || {};
    var xwebd = r.xwebd || {};
    var xwebdEl = $('adbXwebdStatus');

    if (xwebd.running) {
        xwebdEl.textContent = '运行中';
        xwebdEl.className = 'svc-status svc-on';
        $('btnAdbDeployXwebd').style.display = 'none';
        $('btnAdbStartXwebd').style.display = 'none';
        $('btnAdbRestartXwebd').style.display = '';
        $('btnAdbRemoveXwebd').style.display = '';
    } else if (sair.installed || xwebdEl.textContent !== '未检测') {
        var installed = await api('/api/adb/check?serial=' + encodeURIComponent(S.adb.serial));
        if (installed.xwebd_installed) {
            xwebdEl.textContent = '已安装';
            xwebdEl.className = 'svc-status svc-off';
            $('btnAdbDeployXwebd').style.display = 'none';
            $('btnAdbStartXwebd').style.display = '';
            $('btnAdbRestartXwebd').style.display = '';
            $('btnAdbRemoveXwebd').style.display = '';
        } else {
            xwebdEl.textContent = '未安装';
            xwebdEl.className = 'svc-status svc-unknown';
            $('btnAdbDeployXwebd').style.display = '';
            $('btnAdbStartXwebd').style.display = 'none';
            $('btnAdbRestartXwebd').style.display = 'none';
            $('btnAdbRemoveXwebd').style.display = 'none';
        }
    }

    var sairEl = $('adbSairStatus');
    var sairVer = sair.version || '';
    if (sair.custom_running) {
        sairEl.textContent = sairVer ? '运行中 ' + sairVer : '运行中';
        sairEl.className = 'svc-status svc-on';
        $('btnAdbDeploySairCold').textContent = '部署';
        $('btnAdbRemoveSair').style.display = '';
    } else if (sair.native_running) {
        sairEl.textContent = '原生运行中';
        sairEl.className = 'svc-status svc-off';
        $('btnAdbDeploySairCold').textContent = '部署';
        $('btnAdbRemoveSair').style.display = 'none';
    } else if (sair.custom_installed) {
        sairEl.textContent = sairVer ? '已安装 ' + sairVer : '已安装';
        sairEl.className = 'svc-status svc-off';
        $('btnAdbDeploySairCold').textContent = '部署';
        $('btnAdbRemoveSair').style.display = '';
    } else {
        sairEl.textContent = '未安装';
        sairEl.className = 'svc-status svc-unknown';
        $('btnAdbDeploySairCold').textContent = '部署';
        $('btnAdbRemoveSair').style.display = 'none';
    }
}

async function adbRefreshDeviceInfo() {
    if (!S.adb.serial) return;
    var r = await api('/api/adb/device-info?serial=' + encodeURIComponent(S.adb.serial));
    if (!r || r.error) return;
    $('adbDeviceInfo').style.display = '';
    $('adbModel').textContent = r.model || '--';
    $('adbKernel').textContent = r.kernel || '--';
    $('adbCpu').textContent = r.cpu || '--';
    $('adbIp').textContent = r.wifi_ip || '--';
    $('adbWifi').textContent = r.wifi_connected ? '已连接' : '未连接';
    $('adbUptime').textContent = formatUptime(r.uptime_s);
    $('adbMem').textContent = formatMem(r.mem_free_kb, r.mem_total_kb, r.mem_cached_kb);
    $('adbDisk').textContent = formatDisk(r.disk_used_kb, r.disk_total_kb);
    updateViewportHeight();
}

async function adbDeployXwebd() {
    var initR = await api('/api/adb/init-status?serial=' + encodeURIComponent(S.adb.serial || ''));
    var isInit = initR.initialized;
    var confirmMsg = isInit ? '确定部署面板内核？' : '检测到新设备，部署将自动完成初始化（创建启动脚本、配置ADB等）。确定继续？';
    if (!await showConfirm(confirmMsg)) return;
    var progress = $('adbXwebdProgress');
    var bar = $('adbXwebdBar');
    var label = $('adbXwebdProgressLabel');
    progress.style.display = 'inline-flex';
    bar.style.width = '5%';
    label.textContent = isInit ? '部署中...' : '初始化设备...';
    $('btnAdbDeployXwebd').disabled = true;
    bar.style.width = '15%';
    if (isInit) label.textContent = '部署中...';

    var r = await api('/api/deploy/xwebd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });

    bar.style.width = '100%';
    $('btnAdbDeployXwebd').disabled = false;
    if (r.ok) {
        label.textContent = '完成';
        toast('面板内核部署成功', 'success');
        if (r.ip) $('deviceHost').value = r.ip;
        setTimeout(function() { progress.style.display = 'none'; adbRefreshStatus(); }, 1500);
    } else {
        label.textContent = '失败';
        toast('面板内核部署失败', 'error');
    }
}

async function adbStartXwebd() {
    toast('正在启动面板内核...', 'info');
    var r = await api('/api/xwebd/restart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) { toast('面板内核已启动', 'success'); adbRefreshStatus(); }
    else toast('启动失败', 'error');
}

async function adbRestartXwebd() {
    if (!await showConfirm('确定重启面板内核？')) return;
    toast('正在重启面板内核...', 'info');
    var r = await api('/api/xwebd/restart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) toast('面板内核已重启', 'success');
    else toast('重启失败', 'error');
    adbRefreshStatus();
}

async function adbRemoveXwebd() {
    if (!await showConfirm('确定卸载面板内核？', {danger: true})) return;
    toast('正在卸载面板内核...', 'info');
    var r = await api('/api/xwebd/remove', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) { toast('面板内核已卸载', 'success'); adbRefreshStatus(); }
    else toast('卸载失败', 'error');
}

async function adbDeploySair(mode) {
    if (!await showConfirm('确定部署语音助手？\n\n部署过程中设备将重启一次')) return;
    var progress = $('adbSairProgress');
    var bar = $('adbSairBar');
    var label = $('adbSairProgressLabel');
    progress.style.display = 'inline-flex';
    bar.style.width = '10%';
    label.textContent = '部署中...';
    $('btnAdbDeploySairCold').disabled = true;

    var r = await api('/api/adb/deploy-sair', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial, mode: 'cold' }),
    });

    bar.style.width = '100%';
    $('btnAdbDeploySairCold').disabled = false;

    if (r.ok) {
        label.textContent = '完成';
        if (r.rebooting) {
            toast('语音助手部署成功，设备重启中...', 'success');
            setTimeout(function() { progress.style.display = 'none'; adbWaitForReconnect(); }, 1500);
        } else {
            toast('语音助手部署成功', 'success');
            setTimeout(function() { progress.style.display = 'none'; adbRefreshStatus(); }, 1500);
        }
    } else {
        label.textContent = '失败';
        toast('语音助手部署失败: ' + (r.error || ''), 'error');
    }
}

async function adbRemoveSair() {
    if (!await showConfirm('确定卸载语音助手？', {danger: true})) return;
    toast('正在卸载语音助手...', 'info');
    var r = await api('/api/adb/uninstall-sair', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) { toast('语音助手已卸载', 'success'); adbRefreshStatus(); }
    else toast('卸载失败: ' + (r.error || ''), 'error');
}

async function adbPoweroff() {
    if (!await showConfirm('确定关机？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/adb/poweroff', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    adbDisconnect();
}

async function adbFactoryReset() {
    if (!await showConfirm('确定恢复出厂设置？\n\n⚠️ 仅清除小智·智伴项目相关文件（/var/upgrade目录下的自定义程序、脚本和日志），并非对设备本身恢复出厂设置。清除后设备将恢复为原生状态。', {danger: true})) return;
    toast('正在恢复出厂设置...', 'info');
    var r = await api('/api/adb/factory-reset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) {
        stopPolling();
        S.wl.connected = false;
        S.wl.xwebd = false;
        S.wl.sair = false;
        updateConnUI(false);
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        $('btnConnect').textContent = '连接';
        $('btnConnect').className = 'btn btn-primary';
        $('btnConnect').disabled = false;
        resetWirelessUI();
        toast('恢复出厂设置完成，设备将重启', 'success');
        showWiredOverlays();
        waitForReconnect(_adbReconnectOpts());
    } else {
        toast('恢复出厂设置失败: ' + (r.error || '未知错误'), 'error');
    }
}

async function adbRefreshLogs() {
    await adbRefreshLogType('xwebd');
    await adbRefreshLogType('sair');
}

async function adbRefreshLogType(type) {
    if (!S.adb.serial) return;
    var r = await api('/api/adb/logs?serial=' + encodeURIComponent(S.adb.serial) + '&type=' + type + '&lines=80');
    if (!r || r.error) return;
    var lines = (r.logs || {})[type] || [];
    var containerId = type === 'xwebd' ? 'adbLogXwebd' : 'adbLogSair';
    var container = $(containerId);
    if (!container) return;
    if (!lines.length) { container.innerHTML = '<div class="empty-state">暂无日志</div>'; return; }
    var html = '';
    for (var i = 0; i < lines.length; i++) {
        if (lines[i]) html += '<div class="log-line">' + renderLogLine(lines[i]) + '</div>';
    }
    container.innerHTML = html;
    requestAnimationFrame(function() { container.scrollTop = container.scrollHeight; });
}

// ==================== Wireless Mode ====================

async function connectDevice() {
    var btn = $('btnConnect');
    if (S.wl.connected) {
        stopPolling();
        S.wl.connected = false;
        S.wl.xwebd = false;
        S.wl.sair = false;
        updateConnUI(false);
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        $('btnConnect').textContent = '连接';
        $('btnConnect').className = 'btn btn-primary';
        $('btnConnect').disabled = false;
        resetWirelessUI();
        toast('已断开连接', 'info');
        return;
    }
    var host = $('deviceHost').value.trim();
    if (!host) { toast('请输入设备IP地址', 'error'); return; }
    S.wl.host = host;
    btn.disabled = true;
    btn.textContent = '连接中...';
    flashEl(btn);
    showWirelessOverlays();

    if (S.adb.serial) {
        if (S.timers.adbInfo) { clearInterval(S.timers.adbInfo); S.timers.adbInfo = null; }
        S.adb.serial = null;
        S.adb.connected = false;
        resetWiredUI();
    }

    var r = await api('/api/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ host: host }),
    });

    if (r.ok && r.xwebd_connected) {
        S.wl.connected = true;
        S.wl.xwebd = true;
        updateConnUI(true);
        btn.textContent = '断开';
        btn.className = 'btn btn-danger';
        btn.disabled = false;
        toast('连接设备成功', 'success');
        startPolling();
        refreshAll();
        updateConnStatus('xwebdConnStatus', true);

        var ar = await api('/api/assistant/status');
        var ad = ar.data || ar;
        if (!ar.error && ad.running) {
            S.wl.sair = true;
            updateConnStatus('assistantConnStatus', true);
            toast('语音助手已连接', 'success');
        } else {
            S.wl.sair = false;
            updateConnStatus('assistantConnStatus', false);
            toast('语音助手未运行', 'info');
        }
        updateSairLocks();
    } else {
        S.wl.xwebd = false;
        S.wl.sair = false;
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        updateSairLocks();
        toast(r.error ? '连接失败：' + r.error : '连接失败', 'error');
    }

    hideWirelessOverlays();
    if (!S.wl.connected) { btn.disabled = false; btn.textContent = '连接'; btn.className = 'btn btn-primary'; }
}

function resetWirelessUI() {
    $('devModel').textContent = '--';
    $('devKernel').textContent = '--';
    $('devCpu').textContent = '--';
    $('devCpuUsage').textContent = '--';
    $('devWifi').textContent = '--';
    $('devBattery').textContent = '--';
    $('devUptime').textContent = '--';
    $('devMem').textContent = '--';
    $('devDisk').textContent = '--';
    $('assistantInstalled').textContent = '--';
    $('assistantVersion').textContent = '--';
    $('assistantPid').textContent = '--';
    $('assistantActivation').textContent = '--';
    $('assistantState').textContent = '空闲';
    $('assistantState').className = 'status-pill state-idle';
    $('xwebdStatus').textContent = '--';
    $('xwebdVersion').textContent = '--';
    $('cfgMcpEndpoint').value = '';
    $('cfgSairLogLevel').value = '';
    $('cfgListeningMode').value = 'autostop';
    $('cfgAecMode').value = 'local';
    updateAecVisibility();
    $('cfgCustomWsUrl').value = '';
    $('curWsUrl').value = '--';
    $('cfgBootPushDisable').checked = false;
    $('cfgLimitEnable').checked = false;
    $('cfgLimitMinutes').value = '';
    $('cfgLimitDelayTool').checked = false;
    $('cfgLimitSched').checked = false;
    $('cfgLimitSpan1').value = '';
    $('cfgLimitSpan2').value = '';
    document.querySelectorAll('#schedDays input[type=checkbox]').forEach(function(cb) { cb.checked = false; });
    $('limitStat').textContent = '今日已用 -- 分钟（未启用）';
    $('cfgScreenOffSec').value = '';
    $('blSlider').value = 150;
    $('blValue').textContent = '--';
    $('blPersist').checked = false;
    $('volSlider').value = 20;
    $('volValue').textContent = '--';
    $('devUsbMode').textContent = '--';
    var xdiag = $('xwebdDiagContainer');
    if (xdiag) { xdiag.style.display = 'none'; xdiag.innerHTML = ''; }
    $('cfgListenTimeout').value = '';
    $('cfgSessionTimeout').value = '';
    $('cfgWakeupCooldown').value = '';
    $('cfgWsPingInterval').value = '';
    $('cfgLogLevel').value = '';
    $('cfgUploadMax').value = '10';
    $('diagContainer').innerHTML = '<div class="empty-state">点击「运行自检」检测设备是否满足语音助手运行环境</div>';
    $('fileContainer').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logXwebd').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logAssistant').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logPanel').innerHTML = '<div class="empty-state">暂无日志</div>';
    $('xwebdSvcList').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('cfgPrecache').checked = false;
    S.svcData = null;
    $('processContainer').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('processSummary').textContent = '';
    _procCache = [];
    _procFilterCategory = '';
    _procFilterAction = '';
    $('btnXwebdRestart').style.display = '';
    $('btnXwebdRemove').style.display = '';
    $('btnXwebdUpdate').style.display = '';
    $('btnDeploy').disabled = false;
    $('btnUpdate').disabled = false;
    var btnActivate = $('btn-activate');
    if (btnActivate) btnActivate.disabled = true;
    var upgradeProgress = $('upgradeProgress');
    if (upgradeProgress) upgradeProgress.style.display = 'none';
    $('footerInfo').textContent = '--';
    S.wl.sairInstalled = false;
    S.wl.sairNativeRunning = false;
    S.mcpEditingIdx = -1;
    renderMcpTools();
    updateSairLocks();
}

function resetWiredUI() {
    $('adbDeviceList').innerHTML = '<div class="empty-state">点击「扫描设备」检测通过USB连接的设备</div>';
    $('adbDeviceInfo').style.display = 'none';
    $('adbModel').textContent = '--';
    $('adbKernel').textContent = '--';
    $('adbCpu').textContent = '--';
    $('adbIp').textContent = '--';
    $('adbWifi').textContent = '--';
    $('adbUptime').textContent = '--';
    $('adbMem').textContent = '--';
    $('adbDisk').textContent = '--';
    $('adbXwebdStatus').textContent = '未检测';
    $('adbXwebdStatus').className = 'svc-status svc-unknown';
    $('adbSairStatus').textContent = '未检测';
    $('adbSairStatus').className = 'svc-status svc-unknown';
    $('btnAdbDeployXwebd').style.display = '';
    $('btnAdbStartXwebd').style.display = 'none';
    $('btnAdbRestartXwebd').style.display = 'none';
    $('btnAdbRemoveXwebd').style.display = 'none';
    $('wiredDiagContainer').innerHTML = '<div class="empty-state">部署前点击「运行自检」检测设备是否满足 xwebd 运行环境</div>';
    $('logPanelWired').innerHTML = '<div class="empty-state">暂无日志</div>';
}

function updateConnUI(online) {
    var pill = $('connStatus');
    if (online) {
        pill.className = 'status-pill online';
        pill.innerHTML = '<span class="status-dot online"></span>在线';
    } else {
        pill.className = 'status-pill offline';
        pill.innerHTML = '<span class="status-dot offline"></span>离线';
    }
}

function updateConnStatus(id, state) {
    var el = document.getElementById(id);
    var dot = el.querySelector('.conn-dot');
    if (state === true || state === 'connected') {
        dot.className = 'conn-dot connected';
        el.lastChild.textContent = '已连接';
    } else if (state === 'not_installed') {
        dot.className = 'conn-dot not-installed';
        el.lastChild.textContent = '未安装';
    } else {
        dot.className = 'conn-dot disconnected';
        el.lastChild.textContent = '未连接';
    }
}

function updateSairLocks() {
    document.querySelectorAll('.sair-required').forEach(function(el) {
        el.classList.toggle('sair-locked', !S.wl.sair);
    });
}

function startPolling() {
    stopPolling();
    S.timers.status = setInterval(refreshStatus, 5000);
    connectDeviceSSE('xwebd');
    connectDeviceSSE('assistant');
}

function stopPolling() {
    if (S.timers.status) { clearInterval(S.timers.status); S.timers.status = null; }
    disconnectDeviceSSE('xwebd');
    disconnectDeviceSSE('assistant');
}

function refreshAll() {
    refreshStatus();
    refreshAssistantStatus();
    refreshXwebdStatus();
    refreshXwebdServices();
    refreshPlugins();
    refreshProcesses();
    refreshFiles();
    refreshBacklight();
    refreshUsbMode();
    refreshBatteryDetail();
    refreshLogPanel('panel', false);
    refreshLogPanel('xwebd', false);
    refreshLogPanel('assistant', false);
    refreshConfig();
}

// ==================== 音量 / USB 模式 / 电池详情（sair 原生 sound + battery 插件） ====================

function volOnInput() {
    var s = $('volSlider');
    $('volValue').textContent = s.value;
    var pct = (s.value - s.min) / (s.max - s.min) * 100;
    s.style.setProperty('--fill', pct + '%');
}

async function volSave() {
    if (!S.wl.connected) return;
    var v = parseInt($('volSlider').value, 10);
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ volume: v }),
    });
    if (r.ok || !r.error) toast('音量已设置为 ' + v + '/40', 'success');
    else toast('音量设置失败: ' + (r.error || ''), 'error');
}

/* 低频同步音量滑条(语音 volume_up/down 或设备物理按键改过) */
async function refreshVolumeOnly() {
    if (!S.wl.connected) return;
    var r = await api('/api/assistant/config');
    if (r.error || r.volume === undefined || r.volume < 0) return;
    if (parseInt($('volSlider').value, 10) !== r.volume) {
        $('volSlider').value = r.volume;
        volOnInput();
    }
}

var USB_MODE_MAP = {
    adb: 'ADB 调试',
    mass_adb: '存储+ADB',
    charge: '仅充电',
    disabled: '已关闭'
};

async function refreshUsbMode() {
    if (!S.wl.connected) return;
    var r = await api('/api/usb/mode');
    if (r.error) return;
    var d = r.data || r;
    if (d.mode) $('devUsbMode').textContent = USB_MODE_MAP[d.mode] || d.mode;
}

async function refreshBatteryDetail() {
    if (!S.wl.connected) return;
    /* battery 插件在线才可读; 未装/离线保持 /api/status 的纯百分比 */
    var r = await api('/api/plugin/battery/status');
    if (r.error) return;
    var d = r.data || r;
    var base = $('devBattery').textContent.replace(/\s*·.*$/, '');
    var txt = base;
    if (d.voltage_uv > 0) txt += ' · ' + (d.voltage_uv / 1000000).toFixed(2) + 'V';
    if (d.charging) txt += ' · 充电中';
    else if (d.status === 'Full') txt += ' · 已充满';
    if (d.fake_low_flag) txt += ' · 伪低电标记';
    $('devBattery').textContent = txt;
}

async function runXwebdDiag() {
    var container = $('xwebdDiagContainer');
    var btn = $('btnXwebdDiag');
    if (!S.wl.connected) { toast('请先连接设备', 'error'); return; }
    btn.disabled = true;
    btn.textContent = '检测中...';
    var result = null;
    try {
        var r = await api('/api/diag');
        result = r.items ? r : (r.data ? r.data : null);
    } catch(e) {}
    if (!result || !result.items || !result.items.length) {
        toast('自检失败: 未获取到结果', 'error');
        btn.disabled = false;
        btn.textContent = '运行自检';
        return;
    }
    container.style.display = '';
    var items = result.items;
    var html = '<div class="diag-items">';
    for (var i = 0; i < items.length; i++) {
        html += '<div class="diag-item diag-item-pending" data-xdiag-idx="' + i + '">';
        html += '<span class="diag-item-icon diag-icon-spinner"></span>';
        html += '<span class="diag-item-name">' + escapeHtml(items[i].name || '') + '</span>';
        html += '<span class="diag-item-msg">检测中...</span>';
        html += '</div>';
    }
    html += '</div>';
    container.innerHTML = html;
    for (var m = 0; m < items.length; m++) {
        await new Promise(function(resolve) { setTimeout(resolve, 80 + Math.random() * 120); });
        var el = container.querySelector('[data-xdiag-idx="' + m + '"]');
        if (!el) continue;
        el.className = 'diag-item ' + (items[m].ok ? 'diag-item-ok' : 'diag-item-fail');
        el.querySelector('.diag-item-icon').className = 'diag-item-icon';
        el.querySelector('.diag-item-icon').innerHTML = items[m].ok ? '&#10003;' : '&#10007;';
        el.querySelector('.diag-item-msg').textContent = items[m].message || '';
    }
    btn.disabled = false;
    btn.textContent = '运行自检';
}

// ==================== 屏幕背光（xwplug-backlight / 内置回落） ====================

async function refreshBacklight() {
    if (!S.wl.connected) return;
    var r = await api('/api/backlight');
    if (r.error) return;
    var d = r.data || r;
    if (d.brightness) {
        $('blSlider').value = d.brightness;
        $('blValue').textContent = d.brightness;
    }
    $('blPersist').checked = !!d.enable;
}

function blOnInput() {
    var s = $('blSlider');
    $('blValue').textContent = s.value;
    var pct = (s.value - s.min) / (s.max - s.min) * 100;
    s.style.setProperty('--fill', pct + '%');
}

async function blSave() {
    if (!S.wl.connected) return;
    var v = parseInt($('blSlider').value, 10);
    var r = await api('/api/backlight', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ brightness: v }),
    });
    if (r.ok || !r.error) toast('背光已设置为 ' + v, 'success');
    else toast('背光设置失败: ' + (r.error || ''), 'error');
}

async function blSavePersist() {
    if (!S.wl.connected) return;
    var en = $('blPersist').checked ? 1 : 0;
    var r = await api('/api/backlight', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enable: en }),
    });
    if (r.ok || !r.error) {
        toast(en ? '已开启开机恢复（重启后恢复当前亮度）' : '已关闭开机恢复', 'success');
        var d = r.data || r;
        if (d.brightness) { $('blSlider').value = d.brightness; $('blValue').textContent = d.brightness; }
    } else {
        toast('设置失败: ' + (r.error || ''), 'error');
        $('blPersist').checked = !en;
    }
}

async function refreshStatus() {
    if (!S.wl.connected) return;
    try {
        var r = await api('/api/status');
        if (r.error) {
            $('footerInfo').textContent = '状态获取失败';
            return;
        }
        var d = r.data || r;
        $('devModel').textContent = d.model || '--';
        $('devKernel').textContent = d.kernel || '--';
        $('devCpu').textContent = d.cpu || '--';
        $('devCpuUsage').textContent = (d.cpu_usage >= 0) ? d.cpu_usage + '%' : '--';
        var wifiOk = d.wifi_connected;
        if (S.wl.connected && !wifiOk && d.wifi_ip) wifiOk = true;
        $('devWifi').textContent = wifiOk ? '已连接' : '未连接';
        $('devBattery').textContent = d.battery_cap != null ? (d.battery_cap >= 0 && d.battery_cap <= 100 ? d.battery_cap + '%' : 'USB供电') : '--';
        $('devUptime').textContent = formatUptime(d.uptime_s);
        $('devMem').textContent = formatMem(d.mem_free_kb, d.mem_total_kb, d.mem_cached_kb);
        $('devDisk').textContent = formatDisk(d.disk_used_kb, d.disk_total_kb);
        refreshBatteryDetail();
        S._tick = (S._tick || 0) + 1;
        if (S._tick % 6 === 0) refreshVolumeOnly(); /* 30s 拉一次音量(语音调音量后同步) */
        if (d.state) {
            var stateEl = $('assistantState');
            if (stateEl) {
                stateEl.textContent = STATE_MAP[d.state] || d.state;
                stateEl.className = 'status-pill state-' + d.state.toLowerCase();
            }
        }
        $('footerInfo').textContent = (d.model || '?') + ' · ' + (d.wifi_ip || S.wl.host || '?');
    } catch(e) {
        $('footerInfo').textContent = '状态获取异常';
    }
}

async function refreshAssistantStatus() {
    if (!S.wl.connected) return;
    var r = await api('/api/assistant/status');
    if (r.error) return;
    var d = r.data || r;
    var label = '未安装';
    if (d.installed && d.running) label = '运行中';
    else if (d.native_running) label = '原生运行中';
    else if (d.installed) label = '已安装 (未运行)';
    $('assistantInstalled').textContent = label;
    $('assistantVersion').textContent = d.version || '--';
    $('assistantPid').textContent = d.pid || '--';
    $('btnDeploy').disabled = d.installed && d.running;
    $('btnUpdate').disabled = !d.installed;
    var btnActivate = $('btn-activate');
    if (btnActivate) btnActivate.disabled = !d.running;
    if (d.activated || d.activation_code) {
        renderActivationCode(d.activation_code || '', d.activated);
    } else {
        var actEl = $('assistantActivation');
        actEl.textContent = '未激活';
        actEl.style.color = 'var(--text-muted)';
    }
    if (d.log_level) $('cfgSairLogLevel').value = d.log_level;
    if (d.listen_timeout) $('cfgListenTimeout').value = Math.round(d.listen_timeout / 1000);
    if (d.session_timeout) $('cfgSessionTimeout').value = Math.round(d.session_timeout / 1000);
    if (d.wakeup_cooldown) $('cfgWakeupCooldown').value = Math.round(d.wakeup_cooldown / 1000);
    if (d.ws_ping_interval) $('cfgWsPingInterval').value = Math.round(d.ws_ping_interval / 1000);
    var wasConnected = S.wl.sair;
    S.wl.sair = d.running;
    S.wl.sairInstalled = d.installed;
    S.wl.sairNativeRunning = d.native_running || false;
    if (d.running) updateConnStatus('assistantConnStatus', true);
    else if (d.installed) updateConnStatus('assistantConnStatus', false);
    else if (d.native_running) updateConnStatus('assistantConnStatus', false);
    else updateConnStatus('assistantConnStatus', 'not_installed');
    if (wasConnected !== S.wl.sair) {
        updateSairLocks();
        if (S.wl.sair) refreshConfig();
    }
    var notInstalledEl = $('assistantNotInstalledOverlay');
    if (notInstalledEl) {
        var showOverlay = !d.installed || d.native_running;
        notInstalledEl.style.display = showOverlay ? 'flex' : 'none';
        var textEl = $('notInstalledText');
        if (textEl) {
            if (d.native_running) textEl.textContent = '原生语音助手运行中';
            else textEl.textContent = '未安装语音助手';
        }
    }
}

async function refreshXwebdStatus() {
    if (!S.wl.connected) return;
    try {
        var vr = await api('/api/xwebd/version');
        if (!vr.error && vr.version) {
            S.xwebdVersion = vr.version;
            $('xwebdVersion').textContent = 'v' + vr.version;
        }
    } catch(e) {}
    try {
        var sr = await api('/api/services');
        if (!sr.error && sr.telnet) {
            $('xwebdStatus').textContent = '运行中';
            updateConnStatus('xwebdConnStatus', true);
        }
    } catch(e) {}
}

/* ==================== xwebd 服务与功能（原服务状态卡拆解并入各卡） ====================
 * 自启动/看门狗为必开安全机制(关掉=重启失联/崩溃循环无回退), 只显示状态不给开关;
 * telnet/呼吸灯/按键背光为 xwebd 域可选项, 渲染在面板内核管理卡;
 * usb_lun 渲染在插件管理卡 usb 条目; 音频预缓存(sair 域)在语音助手管理卡。 */
async function refreshXwebdServices() {
    if (!S.wl.connected) return;
    var r = await api('/api/services');
    if (r.error) return;
    var d = r.data || r;
    S.svcData = d;

    var html = '';
    /* 必开项: 状态徽标 */
    var autoOk = d.xwebd && d.xwebd.autostart;
    html += '<div class="svc-item"><span class="svc-name">开机自启动</span><span class="svc-status ' + (autoOk ? 'svc-on' : 'svc-off') + '">' + (autoOk ? '已启用' : '未启用') + '</span><span class="svc-hint">核心机制</span></div>';
    var wd = d.boot_watchdog || {};
    var wdOk = wd.running || wd.deployed;
    html += '<div class="svc-item"><span class="svc-name">启动看门狗</span><span class="svc-status ' + (wdOk ? 'svc-on' : 'svc-off') + '">' + (wd.running ? '运行中' : (wd.deployed ? '已部署' : '未部署')) + '</span><span class="svc-hint">崩溃回退保护</span></div>';
    /* 可选项: 开关 */
    var togglable = [
        { name: 'Telnet 终端', service: 'telnet', on: d.telnet && d.telnet.running, hint: '调试用' },
        { name: '呼吸灯', service: 'led', on: d.led && d.led.enabled, hint: '重启保留' },
        { name: '按键背光', service: 'key_backlight', on: d.key_backlight && d.key_backlight.enabled === true, hint: '重启保留' }
    ];
    togglable.forEach(function(item) {
        html += '<div class="svc-item">';
        html += '<span class="svc-name">' + item.name + '</span>';
        html += '<label class="svc-toggle">';
        html += '<input type="checkbox"' + (item.on ? ' checked' : '') + ' onchange="toggleService(\'' + item.service + '\', this.checked)">';
        html += '<span class="svc-toggle-track"><span class="svc-toggle-thumb"></span></span>';
        html += '</label>';
        if (item.hint) html += '<span class="svc-hint">' + item.hint + '</span>';
        html += '</div>';
    });
    var container = $('xwebdSvcList');
    if (container) container.innerHTML = html;
}

async function toggleService(service, enable) {
    if (!S.wl.connected) return;
    var action = enable ? 'enable' : 'disable';
    var r = await api('/api/services/toggle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ service: service, action: action }),
    });
    if (r.ok) {
        toast(service + ' 已' + (enable ? '开启' : '关闭'), 'success');
    } else {
        toast('操作失败: ' + (r.error || ''), 'error');
    }
    await new Promise(function(resolve) { setTimeout(resolve, 500); });
    await refreshXwebdServices();
    await refreshPlugins(); /* usb_lun 开关嵌在 usb 插件条目 */
    if (service === 'usb_lun') refreshUsbMode();
}

async function setCustomWsUrl(url) {
    if (!S.wl.connected) return;
    var r = await api('/api/services/toggle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ service: 'custom_ws_url', action: 'enable', value: url }),
    });
    if (r.ok) {
        toast('自定义WS URL已保存', 'success');
        await new Promise(function(resolve) { setTimeout(resolve, 500); });
        await refreshConfig();
    } else {
        toast('设置失败: ' + (r.error || ''), 'error');
    }
}

// ==================== Plugins (B0 插件框架) ====================

async function refreshPlugins() {
    if (!S.wl.connected) return;
    var r = await api('/api/plugins');
    if (r.error) return;
    var d = r.data || r;
    var plugs = (d.plugins || []);
    /* usb_lun 开关状态: 优先用 refreshXwebdServices 缓存, 无则拉一次 */
    var svc = S.svcData;
    if (!svc) {
        var sr = await api('/api/services');
        if (!sr.error) svc = sr.data || sr;
        S.svcData = svc;
    }
    var usbLunOn = svc && svc.usb_lun && svc.usb_lun.enabled;
    var html = '';
    if (!plugs.length) {
        html = '<div class="empty-state">暂无插件，点击右上角「安装插件」</div>';
    } else {
        plugs.forEach(function(p) {
            var meta = PLUGIN_META[p.name] || {};
            var status = p.disabled ? '<span class="svc-status svc-off">已停用</span>'
                : (p.online ? '<span class="svc-status svc-on">运行中</span>'
                            : '<span class="svc-status svc-off">离线</span>');
            html += '<div class="plugin-item">';
            html += '<div class="plugin-head">';
            html += '<span class="plugin-name">' + escapeHtml(meta.cn || p.name) + '</span>';
            html += '<span class="plugin-id">' + escapeHtml(p.name) + '</span>';
            html += status;
            html += '</div>';
            html += '<div class="plugin-desc">' + escapeHtml(meta.desc || '未知插件，无描述') + '</div>';
            html += '<div class="plugin-foot">';
            if (p.mem_kb) html += '<span class="svc-hint">内存 ' + p.mem_kb + 'KB</span>';
            if (p.restarts) html += '<span class="svc-hint">重启 ' + p.restarts + ' 次</span>';
            html += '<span class="svc-hint">pid ' + (p.pid || '--') + '</span>';
            if (p.name === 'usb') {
                /* USB 存储卡开关随 usb 插件条目展示(服务状态卡已拆解) */
                html += '<label class="svc-toggle" title="数据传输模式下暴露存储卡">';
                html += '<input type="checkbox"' + (usbLunOn ? ' checked' : '') + ' onchange="toggleService(\'usb_lun\', this.checked)">';
                html += '<span class="svc-toggle-track"><span class="svc-toggle-thumb"></span></span>';
                html += '</label>';
                html += '<span class="svc-hint">存储卡</span>';
            }
            if (!meta.must) {
                html += '<span class="plugin-btns">';
                html += '<button class="btn btn-ghost btn-sm" onclick="pluginRestart(\'' + p.name + '\')">重启</button>';
                html += '<button class="btn btn-danger btn-outline btn-sm" onclick="pluginRemove(\'' + p.name + '\')">卸载</button>';
                html += '</span>';
            } else {
                html += '<span class="svc-hint">核心组件</span>';
            }
            html += '</div>';
            html += '</div>';
        });
    }
    $('plugList').innerHTML = html;
}

async function pluginRestart(name) {
    if (!S.wl.connected) return;
    var r = await api('/api/plugins/restart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name }),
    });
    if (r.ok || !r.error) { toast('插件 ' + name + ' 已重启', 'success'); }
    else toast('重启失败: ' + (r.error || ''), 'error');
    await new Promise(function(resolve) { setTimeout(resolve, 800); });
    await refreshPlugins();
}

async function pluginRemove(name) {
    if (!S.wl.connected) return;
    showConfirm('确定卸载插件 ' + name + '？', {
        danger: true,
        onOk: async function() {
            var r = await api('/api/plugins/remove', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name: name }),
            });
            if (r.ok || !r.error) { toast('插件 ' + name + ' 已卸载', 'success'); }
            else toast('卸载失败: ' + (r.error || ''), 'error');
            await refreshPlugins();
        },
    });
}

async function showPluginInstall() {
    if (!S.wl.connected) { toast('请先连接设备', 'error'); return; }
    var input = document.createElement('input');
    input.type = 'file';
    input.onchange = async function() {
        if (!input.files.length) return;
        var file = input.files[0];
        var m = file.name.match(/^xwplug-([\w-]+)$/);
        if (!m) { toast('文件名须为 xwplug-<名称> 形式', 'error'); return; }
        var name = m[1];
        toast('上传插件 ' + name + ' 中...', 'info');
        try {
            var up = await fetch('/api/files/upload?path=' + encodeURIComponent('/var/upgrade/plugins'),
                                  { method: 'POST', body: file });
            var upData = await up.json();
            if (!upData.ok) { toast('上传失败: ' + (upData.error || ''), 'error'); return; }
            var r = await api('/api/plugins/install', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name: name, path: '/var/upgrade/plugins/' + file.name }),
            });
            if (r.ok || !r.error) { toast('插件 ' + name + ' 已安装', 'success'); await refreshPlugins(); }
            else toast('安装失败: ' + (r.error || ''), 'error');
        } catch (e) { toast('安装失败: ' + e.message, 'error'); }
    };
    input.click();
}

var _procFilterCategory = '';
var _procFilterAction = '';
var _procCache = [];

function _procSortKey(p) {
    var stateOrder;
    if (!p.running && p.controllable) stateOrder = 0;
    else if (p.running && p.controllable) stateOrder = 1;
    else stateOrder = 2;
    var catOrder;
    if (p.category === '可选') catOrder = 0;
    else if (p.category === '核心') catOrder = 1;
    else catOrder = 2;
    return stateOrder * 10 + catOrder;
}

function renderProcesses() {
    var procs = _procCache.slice();
    procs.sort(function(a, b) { return _procSortKey(a) - _procSortKey(b); });

    var filtered = procs;
    if (_procFilterCategory) {
        filtered = filtered.filter(function(p) { return p.category === _procFilterCategory; });
    }
    if (_procFilterAction) {
        if (_procFilterAction === 'start') {
            filtered = filtered.filter(function(p) { return !p.running && p.controllable; });
        } else if (_procFilterAction === 'stop') {
            filtered = filtered.filter(function(p) { return p.running && p.controllable; });
        } else if (_procFilterAction === 'protected') {
            filtered = filtered.filter(function(p) { return !p.controllable; });
        }
    }

    var runningCount = 0;
    var totalRss = 0;
    procs.forEach(function(p) {
        if (p.running) { runningCount++; totalRss += p.rss || 0; }
    });
    var summaryEl = $('processSummary');
    if (summaryEl) summaryEl.textContent = '共 ' + procs.length + ' 个进程 · 运行中 ' + runningCount + ' · 总内存 ' + (totalRss / 1024).toFixed(1) + ' MB';

    var colgroup = '<colgroup>'
        + '<col style="width:15%">' 
        + '<col style="width:8%">'
        + '<col style="width:10%">'
        + '<col style="width:14%">'
        + '<col style="width:33%">'
        + '<col style="width:20%">'
        + '</colgroup>';

    var headHtml = '<table class="process-table process-table-head">' + colgroup + '<thead><tr>'
        + '<th>进程</th><th>PID</th><th>内存</th>'
        + '<th class="proc-filter-th"><span>类别</span><select class="proc-filter-select" onchange="_procFilterCategory=this.value;renderProcesses()">'
        + '<option value="">全部</option><option value="核心"' + (_procFilterCategory === '核心' ? ' selected' : '') + '>核心</option><option value="系统"' + (_procFilterCategory === '系统' ? ' selected' : '') + '>系统</option><option value="可选"' + (_procFilterCategory === '可选' ? ' selected' : '') + '>可选</option></select></th>'
        + '<th>说明</th>'
        + '<th class="proc-filter-th"><span>操作</span><select class="proc-filter-select" onchange="_procFilterAction=this.value;renderProcesses()">'
        + '<option value="">全部</option><option value="start"' + (_procFilterAction === 'start' ? ' selected' : '') + '>待启动</option><option value="stop"' + (_procFilterAction === 'stop' ? ' selected' : '') + '>可停止</option><option value="protected"' + (_procFilterAction === 'protected' ? ' selected' : '') + '>受保护</option></select></th>'
        + '</tr></thead></table>';

    var bodyHtml = '<table class="process-table process-table-body">' + colgroup + '<tbody>';
    if (!filtered.length) {
        bodyHtml += '<tr><td colspan="6" style="text-align:center;color:var(--text-muted);padding:12px">无匹配进程</td></tr>';
    }
    filtered.forEach(function(p) {
        var rssStr = p.running ? (p.rss >= 1024 ? (p.rss / 1024).toFixed(1) + ' MB' : p.rss + ' KB') : '--';
        var catCls = 'process-cat-' + (p.category === '核心' ? 'core' : p.category === '系统' ? 'sys' : 'opt');
        var statusCls = p.running ? 'process-running' : 'process-stopped';
        bodyHtml += '<tr class="' + statusCls + '">';
        bodyHtml += '<td><span class="process-name">' + escapeHtml(p.name) + '</span></td>';
        bodyHtml += '<td>' + (p.running ? p.pid : '--') + '</td>';
        bodyHtml += '<td>' + rssStr + '</td>';
        bodyHtml += '<td><span class="process-cat ' + catCls + '">' + escapeHtml(p.category) + '</span></td>';
        bodyHtml += '<td class="process-desc">' + escapeHtml(p.desc) + '</td>';
        bodyHtml += '<td class="process-actions">';
        if (p.controllable) {
            if (p.running) {
                bodyHtml += '<button class="btn btn-danger btn-xs" onclick="processControl(\'' + escapeHtml(p.name) + '\',\'stop\')">停止</button>';
            } else {
                bodyHtml += '<button class="btn btn-accent btn-xs" onclick="processControl(\'' + escapeHtml(p.name) + '\',\'start\')">启动</button>';
            }
        } else {
            bodyHtml += '<span class="process-locked">受保护</span>';
        }
        bodyHtml += '</td></tr>';
    });
    bodyHtml += '</tbody></table>';

    $('processContainer').innerHTML = '<div class="process-table-wrap">'
        + '<div class="process-table-header">' + headHtml + '</div>'
        + '<div class="process-table-scroll">' + bodyHtml + '</div>'
        + '</div>';
}

async function refreshProcesses() {
    if (!S.wl.connected) return;
    var r = await api('/api/processes');
    if (r.error) {
        if (r.error.indexOf('not installed') >= 0) {
            $('processContainer').innerHTML = '<div class="empty-state">进程管理插件（procs）未安装<br>在「插件管理」中安装后可用</div>';
            return;
        }
        if (r._retried) {
            $('processContainer').innerHTML = '<div class="empty-state">获取进程列表失败</div>';
        } else {
            setTimeout(async function() {
                var r2 = await api('/api/processes');
                if (r2.error) {
                    $('processContainer').innerHTML = '<div class="empty-state">获取进程列表失败</div>';
                } else {
                    _procCache = r2.processes || [];
                    if (!_procCache.length) {
                        $('processContainer').innerHTML = '<div class="empty-state">暂无进程信息</div>';
                    } else {
                        renderProcesses();
                    }
                }
            }, 3000);
        }
        return;
    }
    _procCache = r.processes || [];
    if (!_procCache.length) {
        $('processContainer').innerHTML = '<div class="empty-state">暂无进程信息</div>';
        return;
    }
    renderProcesses();
}

async function processControl(name, action) {
    var actionLabel = action === 'stop' ? '停止' : '启动';
    if (action === 'stop') {
        if (!await showConfirm('确定停止进程 ' + name + '？\n\n停止后可能影响设备功能，可随时重新启动', {danger: true})) return;
    } else {
        toast('正在启动 ' + name + '...', 'info');
    }
    var r = await api('/api/processes/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name, action: action }),
    });
    if (r.ok) {
        toast(name + ' 已' + actionLabel, 'success');
        await new Promise(function(resolve) { setTimeout(resolve, 800); });
        await refreshProcesses();
    } else {
        toast(actionLabel + ' ' + name + ' 失败: ' + (r.error || ''), 'error');
        await refreshProcesses();
    }
}

async function reselectUsbMode() {
    if (!S.wl.connected) return;
    if (!await showConfirm('确定重新选择USB模式？\n\n设备将重新显示USB模式选择页面')) return;
    toast('正在触发USB重选...', 'info');
    try {
        var ctl = new AbortController();
        var tid = setTimeout(function() { ctl.abort(); }, 3000);
        await fetch('/api/usb/mode', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({}),
            signal: ctl.signal,
        });
        clearTimeout(tid);
    } catch (e) {}
    toast('设备将重新显示USB模式选择页面', 'success');
}

function setSvcStatus(elId, ok, text) {
    var el = $(elId);
    if (!el) return;
    el.textContent = text;
    el.className = 'svc-status ' + (ok ? 'svc-on' : 'svc-off');
}

async function refreshConfig() {
    if (!S.wl.connected) return;
    var r = await api('/api/assistant/config');
    if (!r.error) {
        if (r.mcp_endpoint !== undefined) $('cfgMcpEndpoint').value = r.mcp_endpoint;
        if (r.listening_mode) $('cfgListeningMode').value = r.listening_mode;
        if (r.aec_mode) $('cfgAecMode').value = r.aec_mode;
        if (r.log_level) $('cfgSairLogLevel').value = r.log_level;
        if (r.listen_timeout) $('cfgListenTimeout').value = Math.round(r.listen_timeout / 1000);
        if (r.session_timeout) $('cfgSessionTimeout').value = Math.round(r.session_timeout / 1000);
        if (r.wakeup_cooldown) $('cfgWakeupCooldown').value = Math.round(r.wakeup_cooldown / 1000);
        if (r.ws_ping_interval) $('cfgWsPingInterval').value = Math.round(r.ws_ping_interval / 1000);
        if (r.ws_url) $('curWsUrl').value = r.ws_url;
        if (r.boot_push_disable !== undefined) $('cfgBootPushDisable').checked = !!r.boot_push_disable;
        if (r.screen_off_idle_sec !== undefined) $('cfgScreenOffSec').value = r.screen_off_idle_sec;
        if (r.volume !== undefined && r.volume >= 0) {
            $('volSlider').value = r.volume;
            volOnInput();
        }
        updateAecVisibility();
        var ul = r.use_limit;
        if (ul) {
            $('cfgLimitEnable').checked = !!ul.enable;
            if (ul.minutes) $('cfgLimitMinutes').value = ul.minutes;
            $('cfgLimitDelayTool').checked = !!ul.delay_tool;
            var sc = ul.sched || {};
            $('cfgLimitSched').checked = !!sc.enable;
            $('cfgLimitSpan1').value = sc.span1 || '';
            $('cfgLimitSpan2').value = sc.span2 || '';
            renderSchedDays(sc.days || 0);
            var spent = Math.floor((ul.spent_sec || 0) / 60);
            var statText;
            if (!ul.enable) {
                statText = '今日已用 ' + spent + ' 分钟（限制未启用）';
            } else {
                var remain = Math.max(0, Math.floor((ul.remain_sec || 0) / 60));
                statText = '今日已用 ' + spent + ' 分钟 / 上限 ' + (ul.minutes || 0) + ' 分钟，剩余 ' + remain + ' 分钟';
                if (ul.locked) statText += '（已达限锁定）';
                if (ul.delay_until) statText += '（延迟豁免至 ' + new Date(ul.delay_until * 1000).toLocaleTimeString() + '）';
            }
            $('limitStat').textContent = statText;
        }
    }
    var r2 = await api('/api/xwebd/config');
    if (!r2.error) {
        if (r2.log_level) $('cfgLogLevel').value = r2.log_level;
        $('cfgUploadMax').value = r2.upload_max_mb != null ? r2.upload_max_mb : 10;
    }
    var r3 = await api('/api/services');
    if (!r3.error) {
        var sd = r3.data || r3;
        S.svcData = sd;
        if (sd.custom_ws_url !== undefined) $('cfgCustomWsUrl').value = sd.custom_ws_url;
        if (sd.audio_precache) $('cfgPrecache').checked = !!sd.audio_precache.enabled;
    }
}

/* 音频预缓存开关(sair 域, 经 xwebd services/toggle) */
async function togglePrecache(enable) {
    if (!S.wl.connected) { $('cfgPrecache').checked = !enable; return; }
    var action = enable ? 'enable' : 'disable';
    var r = await api('/api/services/toggle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ service: 'audio_precache', action: action }),
    });
    if (r.ok || !r.error) {
        toast('音频预缓存已' + (enable ? '开启' : '关闭') + '（下次唤醒生效）', 'success');
    } else {
        toast('设置失败: ' + (r.error || ''), 'error');
        $('cfgPrecache').checked = !enable;
    }
}

/* 星期位图 <-> 勾选框（checkbox value 即位掩码 bit0=周日 .. bit6=周六） */
function renderSchedDays(days) {
    document.querySelectorAll('#schedDays input[type=checkbox]').forEach(function(cb) {
        cb.checked = (days & parseInt(cb.value, 10)) !== 0;
    });
}

function readSchedDays() {
    var days = 0;
    document.querySelectorAll('#schedDays input[type=checkbox]').forEach(function(cb) {
        if (cb.checked) days |= parseInt(cb.value, 10);
    });
    return days;
}

/* AEC 选项仅 Realtime 模式有意义（AutoStop 一问一答、播放与收音不并行） */
function updateAecVisibility() {
    var group = $('aecModeGroup');
    if (!group) return;
    var realtime = $('cfgListeningMode').value === 'realtime';
    group.style.display = realtime ? '' : 'none';
}

async function limitDelayNow(minutes) {
    if (!S.wl.connected) { toast('请先连接设备', 'error'); return; }
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ use_limit_delay_min: minutes }),
    });
    if (r.ok || !r.error) {
        toast('已延长 ' + minutes + ' 分钟', 'success');
        await new Promise(function(resolve) { setTimeout(resolve, 1200); });
        await refreshConfig();
    } else {
        toast('延长失败: ' + (r.error || ''), 'error');
    }
}

async function doWakeup() {
    toast('发送唤醒指令...', 'info');
    var r = await api('/api/wakeup', { method: 'POST' });
    if (r.ok) toast('唤醒指令已发送', 'success');
    else toast('唤醒失败', 'error');
}

async function doAbort() {
    var r = await api('/api/abort', { method: 'POST' });
    if (r.ok) toast('已中止对话', 'success');
    else toast('中止失败', 'error');
}

async function doActivate() {
    toast('发送激活指令...', 'info');
    var r = await api('/api/assistant/activate', { method: 'POST' });
    if (!r.ok && r.error) {
        toast('激活失败: ' + r.error, 'error');
        return;
    }
    toast('激活指令已发送，等待响应...', 'info');
    var maxAttempts = 10;
    var attempt = 0;
    var success = false;
    while (attempt < maxAttempts) {
        await new Promise(function(resolve) { setTimeout(resolve, 2000); });
        attempt++;
        try {
            var sr = await api('/api/assistant/status');
            var sd = sr.data || sr;
            if (sr.error) continue;
            if (sd.activated) {
                renderActivationCode(sd.activation_code || '', true);
                toast('设备已激活，WebSocket配置已获取', 'success');
                success = true;
                refreshAssistantStatus();
                break;
            }
            var code = sd.activation_code || '';
            if (code) {
                renderActivationCode(code, sd.activated || false);
                toast('激活码: ' + code, 'success');
                success = true;
                refreshAssistantStatus();
                break;
            }
        } catch(e) {}
    }
    if (!success) {
        await refreshAssistantStatus();
        toast('未获取到激活信息，请确认设备WiFi已连接且OTA服务可达', 'error');
    }
}

async function doPoweroff() {
    if (!await showConfirm('确定要关机吗？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/poweroff', { method: 'POST' });
    stopPolling();
    S.wl.connected = false;
    S.wl.xwebd = false;
    S.wl.sair = false;
    updateConnUI(false);
    updateConnStatus('xwebdConnStatus', false);
    updateConnStatus('assistantConnStatus', false);
    $('btnConnect').textContent = '连接';
    $('btnConnect').className = 'btn btn-primary';
    $('btnConnect').disabled = false;
    resetWirelessUI();
    toast('设备已关机', 'info');
}

async function doCleanup() {
    if (!await showConfirm('确定清理垃圾文件？\n\n将清理：日志文件(截断清空)、旧版本备份(sair_old)、临时上传文件(.upload_pid)、临时文件(.tmp)\n\n不会删除：sair、xwebd、test.sh 等受保护文件', {icon: '🧹'})) return;
    toast('清理中...', 'info');
    var r = await api('/api/files/cleanup', { method: 'POST' });
    if (r.ok) {
        var freed = r.cleaned_bytes ? (r.cleaned_bytes / 1024).toFixed(1) + ' KB' : '';
        var count = r.cleaned_files || 0;
        toast('清理完成' + (count ? '，清理 ' + count + ' 个文件' : '') + (freed ? '，释放 ' + freed : ''), 'success');
        refreshFiles();
    } else toast('清理失败: ' + (r.error || ''), 'error');
}

async function saveAssistantConfig() {
    var config = {};
    var mcpEndpoint = $('cfgMcpEndpoint').value.trim();
    if (mcpEndpoint) config.mcp_endpoint = mcpEndpoint;
    var logLevel = $('cfgSairLogLevel').value;
    if (logLevel) config.log_level = logLevel;
    config.listening_mode = $('cfgListeningMode').value;
    if (config.listening_mode === 'realtime') {
        config.aec_mode = $('cfgAecMode').value || 'local';
    }
    var listenTimeout = parseInt($('cfgListenTimeout').value);
    var sessionTimeout = parseInt($('cfgSessionTimeout').value);
    var wakeupCooldown = parseInt($('cfgWakeupCooldown').value);
    var wsPingInterval = parseInt($('cfgWsPingInterval').value);
    if (listenTimeout > 0) config.listen_timeout = listenTimeout * 1000;
    if (sessionTimeout > 0) config.session_timeout = sessionTimeout * 1000;
    if (wakeupCooldown > 0) config.wakeup_cooldown = wakeupCooldown * 1000;
    if (wsPingInterval > 0) config.ws_ping_interval = wsPingInterval * 1000;
    var screenOffSec = parseInt($('cfgScreenOffSec').value);
    if (!isNaN(screenOffSec) && screenOffSec >= 0) config.screen_off_idle_sec = screenOffSec;
    /* 每日使用时长限制 */
    config.use_limit_enable = $('cfgLimitEnable').checked ? 1 : 0;
    var limitMinutes = parseInt($('cfgLimitMinutes').value);
    if (!isNaN(limitMinutes)) config.use_limit_minutes = limitMinutes;
    config.use_limit_delay_tool = $('cfgLimitDelayTool').checked ? 1 : 0;
    config.use_limit_sched = [
        $('cfgLimitSched').checked ? 1 : 0,
        readSchedDays(),
        $('cfgLimitSpan1').value.trim(),
        $('cfgLimitSpan2').value.trim()
    ].join(',');
    /* 进阶选项 */
    var customWsUrl = $('cfgCustomWsUrl').value.trim();
    config.custom_ws_url = customWsUrl;
    config.boot_push_disable = $('cfgBootPushDisable').checked ? 1 : 0;
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config),
    });
    if (r.ok || !r.error) {
        toast('助手配置已保存', 'success');
        await new Promise(function(resolve) { setTimeout(resolve, 1500); });
        await refreshConfig();
    } else {
        toast('保存失败: ' + (r.error || ''), 'error');
    }
}

async function saveXwebdConfig() {
    var r = await api('/api/xwebd/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            log_level: $('cfgLogLevel').value,
            upload_max_mb: parseInt($('cfgUploadMax').value) || 10,
        }),
    });
    if (r.ok || !r.error) toast('xwebd 配置已保存', 'success');
    else toast('保存失败', 'error');
}

async function restoreAssistantDefaults() {
    if (!await showConfirm('确定恢复助手配置为默认值？（使用时长限制一并重置）', {icon: '🔄'})) return;
    $('cfgMcpEndpoint').value = '';
    $('cfgSairLogLevel').value = 'INFO';
    $('cfgListeningMode').value = 'autostop';
    $('cfgAecMode').value = 'local';
    updateAecVisibility();
    $('cfgListenTimeout').value = '120';
    $('cfgSessionTimeout').value = '300';
    $('cfgWakeupCooldown').value = '3';
    $('cfgWsPingInterval').value = '25';
    $('cfgScreenOffSec').value = '0';
    $('cfgCustomWsUrl').value = '';
    $('cfgBootPushDisable').checked = false;
    $('cfgLimitEnable').checked = false;
    $('cfgLimitMinutes').value = '60';
    $('cfgLimitDelayTool').checked = false;
    $('cfgLimitSched').checked = false;
    $('cfgLimitSpan1').value = '';
    $('cfgLimitSpan2').value = '';
    renderSchedDays(127);
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            mcp_endpoint: '',
            log_level: 'INFO',
            listening_mode: 'autostop',
            aec_mode: 'local',
            listen_timeout: 120000,
            session_timeout: 300000,
            wakeup_cooldown: 3000,
            ws_ping_interval: 25000,
            screen_off_idle_sec: 0,
            custom_ws_url: '',
            boot_push_disable: 0,
            use_limit_enable: 0,
            use_limit_minutes: 60,
            use_limit_delay_tool: 0,
            use_limit_sched: '0,127,,'
        }),
    });
    if (r.ok || !r.error) {
        toast('助手配置已恢复默认值', 'success');
        await new Promise(function(resolve) { setTimeout(resolve, 1500); });
        await refreshConfig();
    } else {
        toast('恢复默认值失败', 'error');
    }
}

async function restoreXwebdDefaults() {
    if (!await showConfirm('确定恢复面板内核配置为默认值？', {icon: '🔄'})) return;
    $('cfgLogLevel').value = 'DEBUG';
    $('cfgUploadMax').value = '10';
    await saveXwebdConfig();
}

function showAssistantOverlay(totalSec) {
    var el = $('assistantOverlay');
    var txt = $('assistantOverlayText');
    if (el) el.style.display = 'flex';
    if (txt) txt.textContent = '热更新中... ' + totalSec + 's';
}
function updateAssistantOverlayCountdown(sec) {
    var txt = $('assistantOverlayText');
    if (txt) txt.textContent = '热更新中... ' + sec + 's';
}
function updateAssistantOverlayText(msg) {
    var txt = $('assistantOverlayText');
    if (txt) txt.textContent = msg;
}
function hideAssistantOverlay() {
    var el = $('assistantOverlay');
    if (el) el.style.display = 'none';
}

async function doHotUpdate() {
    if (S.busy) return;
    S.busy = true;
    try {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error) {
        toast('无法获取助手状态', 'error');
        return;
    }
    if (!sd.installed) {
        toast('语音助手未安装，请先点击「部署」按钮', 'error');
        return;
    }
    if (!sd.running) {
        toast('自定义语音助手未运行，请使用「冷更新」功能', 'error');
        return;
    }
    var oldPid = sd.pid;
    var oldVersion = sd.version || '';
    var lv = await api('/api/local-versions');
    var newVer = lv.assistant || '';
    var verInfo = '';
    if (oldVersion || newVer) verInfo = '\n\n当前 v' + oldVersion + ' → v' + newVer;
    if (!await showConfirm('确定进行热更新？' + verInfo + '\n\n热更新不会重启设备，助手进程将自动替换为新版本')) return;
    toast('正在执行热更新...', 'info');
    var r = await api('/api/assistant/upgrade', { method: 'POST', body: JSON.stringify({method: 'hot'}) });
    if (!r.ok && r.error) {
        toast('热更新失败: ' + r.error, 'error');
        return;
    }
    if (r.rebooting) {
        toast('冷更新成功，设备重启中...', 'success');
        if (S.mode === 'wired') {
            showWiredOverlays();
            waitForReconnect(_adbReconnectOpts());
        } else {
            showWirelessOverlays();
            wirelessWaitForReconnect();
        }
        return;
    }
    toast('热更新指令已发送，等待助手重启...', 'info');
    showAssistantOverlay(60);
    var maxAttempts = 15;
    var attempt = 0;
    var success = false;
    var sawOffline = false;
    var sawRestart = false;
    var consecutiveErrors = 0;
    while (attempt < maxAttempts) {
        await new Promise(function(resolve) { setTimeout(resolve, 2000); });
        attempt++;
        updateAssistantOverlayCountdown(Math.max(0, 60 - attempt * 2));
        try {
            var sr = await api('/api/assistant/status');
            var sd2 = sr.data || sr;
            if (sr.error) {
                consecutiveErrors++;
                if (consecutiveErrors >= 5) {
                    hideAssistantOverlay();
                    toast('设备似乎已重启，切换到重启等待模式...', 'info');
                    showWirelessOverlays();
                    wirelessWaitForReconnect();
                    return;
                }
                continue;
            }
            consecutiveErrors = 0;
            if (!sd2.running) {
                sawOffline = true;
                continue;
            }
            if (sawOffline) {
                success = true;
                var newVer = sd2.version || '';
                if (newVer && newVer !== oldVersion) {
                    toast('热更新成功！版本 ' + oldVersion + ' → ' + newVer, 'success');
                } else {
                    toast('热更新成功！助手已重启', 'success');
                }
                refreshAssistantStatus();
                refreshConfig();
                break;
            }
            if (sd2.running && !sd2.version && oldVersion) {
                sawRestart = true;
                continue;
            }
            if (sawRestart && sd2.version) {
                success = true;
                var newVer = sd2.version || '';
                if (newVer !== oldVersion) {
                    toast('热更新成功！版本 ' + oldVersion + ' → ' + newVer, 'success');
                } else {
                    toast('热更新成功！助手已重启', 'success');
                }
                refreshAssistantStatus();
                refreshConfig();
                break;
            }
            if (sd2.version && sd2.version !== oldVersion) {
                success = true;
                toast('热更新成功！版本 ' + oldVersion + ' → ' + sd2.version, 'success');
                refreshAssistantStatus();
                refreshConfig();
                break;
            }
        } catch(e) {
            consecutiveErrors++;
            if (consecutiveErrors >= 5) {
                hideAssistantOverlay();
                toast('设备似乎已重启，切换到重启等待模式...', 'info');
                showWirelessOverlays();
                wirelessWaitForReconnect();
                return;
            }
        }
    }
    hideAssistantOverlay();
    if (!success) {
        toast('热更新超时，请检查助手状态', 'error');
        refreshAssistantStatus();
    }
    } finally { S.busy = false; }
}

async function doDeploy() {
    if (S.busy) return;
    S.busy = true;
    try {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error) {
        toast('无法获取助手状态', 'error');
        return;
    }
    var confirmMsg = '确定部署语音助手？\n\n部署过程中设备将重启一次';
    if (sd && sd.native_running) {
        confirmMsg = '当前运行的是原生语音助手，部署将替换为自定义版本。\n\n部署过程中设备将重启一次';
    } else if (sd && !sd.installed) {
        confirmMsg = '语音助手未安装，部署将安装自定义版本。\n\n部署过程中设备将重启一次';
    }
    if (!await showConfirm(confirmMsg)) return;
    var progress = $('upgradeProgress');
    var bar = $('upgradeBar');
    var label = $('upgradeLabel');
    progress.style.display = 'block';
    bar.style.width = '0%';
    label.textContent = '上传部署中...';
    $('btnDeploy').disabled = true;
    bar.style.width = '10%';

    var r = await api('/api/assistant/smart-deploy', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({}),
    });

    bar.style.width = '100%';
    $('btnDeploy').disabled = false;
    if (r.ok || r.rebooting) {
        label.textContent = '部署成功！设备重启中...';
        toast('语音助手部署成功，设备重启中...', 'success');
        setTimeout(function() { progress.style.display = 'none'; }, 1500);
        if (S.mode === 'wired') {
            showWiredOverlays();
            waitForReconnect(_adbReconnectOpts());
        } else {
            showWirelessOverlays();
            wirelessWaitForReconnect();
        }
    } else {
        label.textContent = '部署失败: ' + (r.error || '');
        toast('部署失败: ' + (r.error || ''), 'error');
    }
    } finally { S.busy = false; }
}

async function doUpdate() {
    if (S.busy) return;
    S.busy = true;
    try {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error) {
        toast('无法获取助手状态', 'error');
        return;
    }
    if (!sd.installed) {
        if (sd.native_running) {
            toast('当前运行的是原生语音助手，请使用「部署」功能', 'error');
        } else {
            toast('语音助手未安装，请先点击「部署」按钮', 'error');
        }
        return;
    }
    var oldVersion = sd.version || '';
    var lv = await api('/api/local-versions');
    var newVer = lv.assistant || '';
    var verInfo = '';
    if (oldVersion || newVer) verInfo = '\n\n当前 v' + oldVersion + ' → v' + newVer;
    if (!await showConfirm('确定进行冷更新？' + verInfo + '\n\n冷更新将替换助手程序并重启设备')) return;
    toast('正在执行冷更新...', 'info');
    var r = await api('/api/assistant/upgrade', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({method: 'cold'}),
    });
    if (r.ok || r.rebooting) {
        toast('冷更新成功，设备重启中...', 'success');
        if (S.mode === 'wired') {
            showWiredOverlays();
            waitForReconnect(_adbReconnectOpts());
        } else {
            showWirelessOverlays();
            wirelessWaitForReconnect();
        }
    } else if (r.error) {
        toast('冷更新失败: ' + r.error, 'error');
    } else {
        toast('冷更新指令已发送', 'success');
        refreshAssistantStatus();
    }
    } finally { S.busy = false; }
}

async function doUninstall() {
    if (S.busy) return;
    S.busy = true;
    try {
    if (!await showConfirm('确定卸载语音助手？\n\n卸载后设备将回退到原生语音助手并重启', {danger: true})) return;
    toast('卸载中...', 'info');
    var r = await api('/api/assistant/uninstall', { method: 'POST' });
    if (r.ok || r.rebooting) {
        toast('语音助手已卸载，设备重启中...', 'success');
        if (S.mode === 'wired') {
            showWiredOverlays();
            waitForReconnect(_adbReconnectOpts());
        } else {
            showWirelessOverlays();
            wirelessWaitForReconnect();
        }
    } else {
        toast('卸载失败: ' + (r.error || ''), 'error');
    }
    } finally { S.busy = false; }
}

async function doXwebdUpdate() {
    if (S.busy) return;
    S.busy = true;
    try {
    var lv = await api('/api/local-versions');
    var newVer = lv.xwebd || '';
    var curVer = S.xwebdVersion || '';
    var verInfo = '';
    if (curVer || newVer) verInfo = '\n\n当前 v' + curVer + ' → v' + newVer;
    if (!await showConfirm('确定更新xwebd？' + verInfo + '\n\n更新将替换xwebd程序并重启设备')) return;
    toast('正在更新xwebd...', 'info');
    if (S.wl.connected) {
        var r = await api('/api/xwebd/wireless-update', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({}),
        });
        if (r.ok || r.rebooting) {
            toast('xwebd 更新成功，设备重启中...', 'success');
            showWirelessOverlays();
            wirelessWaitForReconnect();
        } else {
            toast('更新失败: ' + (r.error || ''), 'error');
        }
    } else {
        var r = await api('/api/xwebd/upload-update', { method: 'POST' });
        if (r.ok || r.rebooting) {
            toast('xwebd 更新成功，设备重启中...', 'success');
            showWiredOverlays();
            waitForReconnect(_adbReconnectOpts());
        } else {
            toast('更新失败: ' + (r.error || ''), 'error');
        }
    }
    } finally { S.busy = false; }
}

async function doXwebdRestart() {
    toast('重启 xwebd...', 'info');
    var r = await api('/api/xwebd/restart', { method: 'POST' });
    if (r.ok) toast('xwebd 已重启', 'success');
    else toast('重启失败', 'error');
}

async function doXwebdRemove() {
    if (!await showConfirm('确定卸载 xwebd？', {danger: true})) return;
    var r = await api('/api/xwebd/remove', { method: 'POST' });
    if (r.ok) toast('xwebd 已卸载', 'success');
    else toast('卸载失败', 'error');
}

// ==================== Reconnect ====================

function showWiredOverlays() {
    showOverlay('adbDeviceOverlay');
    showOverlay('adbAssistantOverlay');
    showOverlay('adbControlOverlay');
}
function hideWiredOverlays() {
    hideOverlay('adbDeviceOverlay');
    hideOverlay('adbAssistantOverlay');
    hideOverlay('adbControlOverlay');
}
function showWirelessOverlays() {
    showOverlay('deviceOverlay');
    showOverlay('xwebdOverlay');
    showOverlay('processOverlay');
    showOverlay('serviceOverlay');
    showOverlay('assistantOverlay');
}
function hideWirelessOverlays() {
    hideOverlay('deviceOverlay');
    hideOverlay('xwebdOverlay');
    hideOverlay('processOverlay');
    hideOverlay('serviceOverlay');
    hideOverlay('assistantOverlay');
}

function waitForReconnect(opts) {
    S.rebootStart = Date.now();
    if (S.timers.reboot) clearInterval(S.timers.reboot);
    S.timers.reboot = setInterval(async function() {
        if (Date.now() - S.rebootStart > (opts.timeout || 60000)) {
            clearInterval(S.timers.reboot);
            S.timers.reboot = null;
            if (opts.onTimeout) opts.onTimeout();
            return;
        }
        try {
            var done = await opts.onCheck();
            if (done) {
                clearInterval(S.timers.reboot);
                S.timers.reboot = null;
                if (opts.onReconnect) opts.onReconnect();
            }
        } catch(e) {}
    }, opts.interval || 3000);
}

function _adbReconnectOpts() {
    return {
        onCheck: async function() {
            var r = await api('/api/adb/devices');
            if (r.error) return false;
            var devices = r.devices || [];
            for (var i = 0; i < devices.length; i++) {
                if (devices[i].serial === S.adb.serial && devices[i].state === 'device') return true;
            }
            return false;
        },
        onReconnect: async function() {
            hideWiredOverlays();
            toast('设备已重新连接', 'success');
            await selectAdbDevice(S.adb.serial);
        },
        onTimeout: function() {
            hideWiredOverlays();
            toast('重连超时，请手动扫描', 'error');
        }
    };
}

function adbWaitForReconnect() {
    toast('设备重启中，等待重新连接...', 'info');
    showWiredOverlays();
    waitForReconnect(_adbReconnectOpts());
}

async function adbRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    showWiredOverlays();
    await api('/api/adb/reboot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    waitForReconnect(_adbReconnectOpts());
}

async function wirelessRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    showWirelessOverlays();
    await api('/api/reboot', { method: 'POST' });
    wirelessWaitForReconnect();
}

function wirelessWaitForReconnect() {
    if (S.timers.wirelessReboot) { clearTimeout(S.timers.wirelessReboot); S.timers.wirelessReboot = null; }
    S.wl.connected = false;
    S.wl.xwebd = false;
    S.wl.sair = false;
    stopPolling();
    updateConnUI(false);
    updateConnStatus('xwebdConnStatus', false);
    updateConnStatus('assistantConnStatus', false);
    $('btnConnect').textContent = '连接';
    $('btnConnect').className = 'btn btn-primary';
    $('btnConnect').disabled = true;
    toast('设备重启中，等待重新连接...', 'info');
    S.timers.wirelessReboot = setTimeout(function() {
        S.timers.wirelessReboot = null;
        waitForReconnect({
            timeout: 90000,
            interval: 3000,
            onCheck: async function() {
                try {
                    var r = await api('/api/connect', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ host: S.wl.host }),
                    });
                    return r.ok && r.xwebd_connected;
                } catch(e) { return false; }
            },
            onReconnect: function() {
                S.wl.connected = true;
                S.wl.xwebd = true;
                S.wl.sair = false;
                updateConnUI(true);
                $('btnConnect').textContent = '断开';
                $('btnConnect').className = 'btn btn-danger';
                $('btnConnect').disabled = false;
                startPolling();
                updateConnStatus('xwebdConnStatus', true);
                updateConnStatus('assistantConnStatus', false);
                hideWirelessOverlays();
                toast('设备已重新连接', 'success');
                setTimeout(function() { refreshAll(); }, 5000);
            },
            onTimeout: function() {
                hideWirelessOverlays();
                $('btnConnect').disabled = false;
                toast('重连超时，请手动连接', 'error');
            }
        });
    }, 15000);
}

// ==================== Logs ====================

function getLogContainerId(source) {
    if (source === 'xwebd') return 'logXwebd';
    if (source === 'assistant') return 'logAssistant';
    return S.mode === 'wired' ? 'logPanelWired' : 'logPanel';
}

async function refreshLogPanel(source, flash) {
    var containerId = getLogContainerId(source);
    if (source === 'panel') {
        var levelEl = S.mode === 'wired' ? $('logLevelPanelWired') : $('logLevelPanel');
        var level = levelEl ? levelEl.value : '';
        var url = '/api/panel/logs?lines=80';
        if (level) url += '&level=' + level;
        var r = await api(url);
        renderLogPanel(containerId, r, source);
    } else {
        if (!S.wl.connected) return;
        var levelId = source === 'xwebd' ? 'logLevelXwebd' : 'logLevelAssistant';
        var level = $(levelId) ? $(levelId).value : '';
        var sourceNum = source === 'xwebd' ? '2' : '1';
        var url = '/api/logs?lines=80&source=' + sourceNum;
        if (level) url += '&level=' + level;
        var r = await api(url);
        renderLogPanel(containerId, r, source);
    }
    if (flash) flashEl($(containerId));
}

function renderLogPanel(containerId, r, source) {
    var container = $(containerId);
    if (!container) return;
    if (r.error) {
        container.innerHTML = '<div class="empty-state">获取失败: ' + escapeHtml(r.error) + '</div>';
        return;
    }
    var d = r.data || r;
    var allLines = d.lines || d.logs || [];
    var ls = LOG[source];
    var skipCount = 0;
    if (ls.clearLine >= 0) {
        skipCount = Math.min(ls.clearLine, allLines.length);
        if (allLines.length > ls.clearLine) ls.clearLine = -1;
    }
    ls.lastCount = allLines.length;
    var lines = allLines.slice(skipCount);
    if (!lines.length) {
        container.innerHTML = '<div class="empty-state">暂无日志</div>';
        return;
    }
    var html = '';
    lines.forEach(function(l) {
        if (typeof l === 'string') {
            html += '<div class="log-line">' + renderLogLine(l) + '</div>';
        } else {
            var cls = 'log-info';
            if (l.level === 'ERROR' || l.level === 'E' || l.level === 'CRITICAL') cls = 'log-error';
            else if (l.level === 'WARN' || l.level === 'W' || l.level === 'WARNING') cls = 'log-warn';
            else if (l.level === 'DEBUG' || l.level === 'D') cls = 'log-debug';
            var src = l.source ? '<span class="log-source">[' + l.source + ']</span> ' : '';
            html += '<div class="log-line ' + cls + '">' + src + escapeHtml(stripAnsi(l.text || l.message || '')) + '</div>';
        }
    });
    container.innerHTML = html;
    requestAnimationFrame(function() { container.scrollTop = container.scrollHeight; });
}

function clearLogPanel(source) {
    var containerId = getLogContainerId(source);
    var container = $(containerId);
    if (container) {
        container.innerHTML = '<div class="empty-state">日志已清除</div>';
        flashEl(container);
    }
    LOG[source].clearLine = LOG[source].lastCount;
    if (source === 'panel') {
        disconnectPanelSSE();
        setTimeout(connectPanelSSE, 500);
    }
    toast('已清屏，仅显示新日志', 'success');
}

async function cleanLogPanel(source) {
    if (source === 'panel') {
        var r = await api('/api/panel/logs/clean', { method: 'POST' });
        if (r.ok) {
            LOG.panel.clearLine = -1;
            LOG.panel.lastCount = 0;
            var container = $('logPanel');
            if (container) container.innerHTML = '<div class="empty-state">日志已清理</div>';
            toast('面板日志已清理', 'success');
        } else toast('清理失败', 'error');
    } else {
        if (!S.wl.connected) { toast('请先连接设备', 'error'); return; }
        var sourceNum = source === 'xwebd' ? '2' : '1';
        var r = await api('/api/logs/clean?source=' + sourceNum, { method: 'POST' });
        if (r.ok) {
            LOG[source].clearLine = -1;
            LOG[source].lastCount = 0;
            var container = $(getLogContainerId(source));
            if (container) container.innerHTML = '<div class="empty-state">日志已清理</div>';
            toast((source === 'xwebd' ? '面板内核' : '语音助手') + '日志已清理', 'success');
        } else toast('清理失败', 'error');
    }
}

function connectPanelSSE() {
    if (S.panelSSE) return;
    try {
        S.panelSSE = new EventSource('/api/panel/logs/stream');
        S.panelSSE.onmessage = function(e) {
            var autoEl = S.mode === 'wired' ? $('autoRefreshPanelWired') : $('autoRefreshPanel');
            if (!autoEl || !autoEl.checked) return;
            var data = JSON.parse(e.data);
            var containerId = S.mode === 'wired' ? 'logPanelWired' : 'logPanel';
            var container = $(containerId);
            if (!container) return;
            var emptyState = container.querySelector('.empty-state');
            if (emptyState) emptyState.remove();
            var div = document.createElement('div');
            div.className = 'log-line';
            div.innerHTML = renderLogLine(data.text || '');
            container.appendChild(div);
            if (container.children.length > 500) container.removeChild(container.firstChild);
            container.scrollTop = container.scrollHeight;
        };
        S.panelSSE.onerror = function() {
            S.panelSSE.close();
            S.panelSSE = null;
            setTimeout(connectPanelSSE, 5000);
        };
    } catch(e) {}
    startPanelLogPoll();
}

function startPanelLogPoll() {
    if (S.timers.panelLogPoll) return;
    S.timers.panelLogPoll = setInterval(function() {
        var autoEl = S.mode === 'wired' ? $('autoRefreshPanelWired') : $('autoRefreshPanel');
        if (autoEl && autoEl.checked) refreshLogPanel('panel', false);
    }, 3000);
}

function stopPanelLogPoll() {
    if (S.timers.panelLogPoll) {
        clearInterval(S.timers.panelLogPoll);
        S.timers.panelLogPoll = null;
    }
}

function disconnectPanelSSE() {
    if (S.panelSSE) { S.panelSSE.close(); S.panelSSE = null; }
    stopPanelLogPoll();
}

function connectDeviceSSE(source) {
    if (S.deviceSSE[source]) return;
    var sourceNum = source === 'xwebd' ? '2' : '1';
    try {
        var es = new EventSource('/api/device/logs/stream?source=' + sourceNum);
        S.deviceSSE[source] = es;
        es.onmessage = function(e) {
            var autoEl = source === 'xwebd' ? $('autoRefreshXwebd') : $('autoRefreshAssistant');
            if (!autoEl || !autoEl.checked) return;
            try {
                var data = JSON.parse(e.data);
                var containerId = getLogContainerId(source);
                var container = $(containerId);
                if (!container) return;
                var emptyState = container.querySelector('.empty-state');
                if (emptyState) emptyState.remove();
                var cls = 'log-info';
                if (data.level === 'ERROR' || data.level === 'E') cls = 'log-error';
                else if (data.level === 'WARN' || data.level === 'W') cls = 'log-warn';
                else if (data.level === 'DEBUG' || data.level === 'D') cls = 'log-debug';
                var src = data.source ? '<span class="log-source">[' + data.source + ']</span> ' : '';
                var div = document.createElement('div');
                div.className = 'log-line ' + cls;
                div.innerHTML = src + escapeHtml(stripAnsi(data.text || ''));
                container.appendChild(div);
                if (container.children.length > 500) container.removeChild(container.firstChild);
                container.scrollTop = container.scrollHeight;
            } catch(ex) {}
        };
        es.onerror = function() {
            es.close();
            S.deviceSSE[source] = null;
            setTimeout(function() {
                if (S.wl.connected) connectDeviceSSE(source);
            }, 5000);
        };
    } catch(e) {}
}

function disconnectDeviceSSE(source) {
    if (S.deviceSSE[source]) { S.deviceSSE[source].close(); S.deviceSSE[source] = null; }
}

// ==================== File Management ====================

async function refreshFiles() {
    if (!S.wl.connected) return;
    var r = await api('/api/files?path=' + encodeURIComponent(S.currentPath));
    if (r.error) {
        if (r.error.indexOf('not installed') >= 0) {
            $('fileContainer').innerHTML = '<div class="empty-state">文件管理插件（files）未安装<br>在「插件管理」中安装后可用</div>';
        }
        return;
    }
    var d = r.data || r;
    var files = d.files || [];
    var container = $('fileContainer');

    var parts = S.currentPath.split('/').filter(Boolean);
    var navHtml = '<span class="file-nav-dim">/</span>';
    var cumPath = '';
    parts.forEach(function(p, i) {
        cumPath += '/' + p;
        navHtml += '<span class="file-nav-sep">/</span>';
        if (i === 0) navHtml += '<span class="file-nav-dim">' + escapeHtml(p) + '</span>';
        else if (i < parts.length - 1) navHtml += '<span class="file-nav-link" data-nav-path="' + escapeHtml(cumPath) + '">' + escapeHtml(p) + '</span>';
        else navHtml += '<span>' + escapeHtml(p) + '</span>';
    });

    var html = '<div class="file-nav">' + navHtml + '</div>';

    if (!files.length) {
        container.innerHTML = html + '<div class="empty-state">空目录</div>';
        container.querySelectorAll('[data-nav-path]').forEach(function(el) {
            el.addEventListener('click', function() { navigateTo(el.getAttribute('data-nav-path')); });
        });
        return;
    }
    html += '<table class="file-table"><thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead><tbody>';
    files.forEach(function(f) {
        var nameClass = f.is_dir ? 'file-dir-link' : (f.protected ? 'file-name-cell file-protected' : 'file-name-cell');
        var filePath = S.currentPath.endsWith('/') ? S.currentPath + f.name : S.currentPath + '/' + f.name;
        var nameAttr = f.is_dir ? ' data-nav-path="' + escapeHtml(filePath) + '"' : '';
        html += '<tr>';
        html += '<td><span class="' + nameClass + '"' + nameAttr + ' style="cursor:pointer">' + escapeHtml(f.name) + (f.is_dir ? '/' : '') + '</span></td>';
        html += '<td>' + (f.is_dir ? '--' : formatSize(f.size)) + '</td>';
        html += '<td>' + formatFileTime(f.mtime) + '</td>';
        html += '<td class="file-actions">';
        if (!f.is_dir) html += '<button class="btn btn-ghost btn-xs" data-download="' + escapeHtml(filePath) + '">下载</button>';
        if (!f.protected) html += '<button class="btn btn-danger btn-xs" data-delete="' + escapeHtml(filePath) + '">删除</button>';
        html += '</td></tr>';
    });
    html += '</tbody></table>';
    container.innerHTML = html;

    container.querySelectorAll('[data-nav-path]').forEach(function(el) {
        el.addEventListener('click', function() { navigateTo(el.getAttribute('data-nav-path')); });
    });
    container.querySelectorAll('[data-download]').forEach(function(el) {
        el.addEventListener('click', function() { downloadFile(el.getAttribute('data-download')); });
    });
    container.querySelectorAll('[data-delete]').forEach(function(el) {
        el.addEventListener('click', function() { deleteFile(el.getAttribute('data-delete')); });
    });
}

async function refreshFilesWithFlash() {
    await refreshFiles();
    var container = $('fileContainer');
    if (container) flashEl(container);
}

function navigateTo(path) {
    S.currentPath = path;
    refreshFiles();
}

async function downloadFile(path) {
    window.open('/api/files/download?path=' + encodeURIComponent(path), '_blank');
}

async function deleteFile(path) {
    if (!await showConfirm('确定删除 ' + path + '？', {danger: true})) return;
    var r = await api('/api/files/download?path=' + encodeURIComponent(path), { method: 'DELETE' });
    if (r.ok) { toast('已删除', 'success'); refreshFiles(); }
    else toast('删除失败', 'error');
}

function triggerUpload() { $('uploadFile').click(); }

async function doUpload() {
    var fileInput = $('uploadFile');
    if (!fileInput.files.length) return;
    toast('上传中...', 'info');
    var fd = new FormData();
    for (var i = 0; i < fileInput.files.length; i++) fd.append('file', fileInput.files[i]);
    try {
        var r = await fetch('/api/files/upload?path=' + encodeURIComponent(S.currentPath), { method: 'POST', body: fd });
        var data = await r.json();
        if (data.ok) { toast('上传成功', 'success'); refreshFiles(); }
        else toast('上传失败', 'error');
    } catch (e) { toast('上传失败', 'error'); }
    fileInput.value = '';
}

// ==================== Diagnostics ====================

async function runDiag() {
    var container = $('diagContainer');
    var btn = $('btnDiag');
    if (!S.wl.connected) {
        container.innerHTML = '<div class="empty-state">请先无线连接设备</div>';
        return;
    }
    btn.disabled = true;
    btn.textContent = '检测中...';

    var assistantEnvResult = null, assistantResult = null;
    try { var aer = await api('/api/assistant/env'); assistantEnvResult = aer.items ? aer : (aer.data ? aer.data : null); } catch(e) {}
    try { var ar = await api('/api/assistant/diag'); assistantResult = ar.items ? ar : (ar.data ? ar.data : null); } catch(e) {}

    var allItems = [];
    if (assistantEnvResult) {
        var envItems = assistantEnvResult.items || [];
        for (var i = 0; i < envItems.length; i++) allItems.push(envItems[i]);
    }
    if (assistantResult) {
        var diagItems = assistantResult.items || [];
        for (var j = 0; j < diagItems.length; j++) allItems.push(diagItems[j]);
    }

    if (!allItems.length) {
        container.innerHTML = '<div class="empty-state">请先无线连接设备</div>';
        btn.disabled = false;
        btn.textContent = '运行自检';
        return;
    }

    var html = '<div class="diag-items">';
    for (var k = 0; k < allItems.length; k++) {
        html += '<div class="diag-item diag-item-pending" data-diag-idx="' + k + '">';
        html += '<span class="diag-item-icon diag-icon-spinner"></span>';
        html += '<span class="diag-item-name">' + allItems[k].name + '</span>';
        html += '<span class="diag-item-msg">检测中...</span>';
        html += '</div>';
    }
    html += '</div>';
    container.innerHTML = html;

    for (var m = 0; m < allItems.length; m++) {
        await new Promise(function(resolve) { setTimeout(resolve, 80 + Math.random() * 120); });
        var item = allItems[m];
        var el = container.querySelector('[data-diag-idx="' + m + '"]');
        if (!el) continue;
        var cls = item.ok ? 'diag-item-ok' : 'diag-item-fail';
        var icon = item.ok ? '&#10003;' : '&#10007;';
        el.className = 'diag-item ' + cls;
        el.querySelector('.diag-item-icon').className = 'diag-item-icon';
        el.querySelector('.diag-item-icon').innerHTML = icon;
        el.querySelector('.diag-item-msg').textContent = item.message;
    }

    btn.disabled = false;
    btn.textContent = '运行自检';
}

async function runWiredDiag() {
    var container = $('wiredDiagContainer');
    var btn = $('btnWiredDiag');
    if (!S.adb.serial) {
        container.innerHTML = '<div class="empty-state">请先连接ADB设备</div>';
        return;
    }
    btn.disabled = true;
    btn.textContent = '检测中...';

    var envResult = null;
    try {
        var er = await api('/api/adb/xwebd-env?serial=' + encodeURIComponent(S.adb.serial || ''));
        envResult = er.items ? er : (er.data ? er.data : null);
    } catch(e) {}

    if (!envResult) {
        container.innerHTML = '<div class="empty-state">环境自检失败，请检查ADB连接</div>';
        btn.disabled = false;
        btn.textContent = '运行自检';
        return;
    }

    var items = envResult.items || [];
    var html = '<div class="diag-items">';
    for (var k = 0; k < items.length; k++) {
        html += '<div class="diag-item diag-item-pending" data-diag-idx="' + k + '">';
        html += '<span class="diag-item-icon diag-icon-spinner"></span>';
        html += '<span class="diag-item-name">' + items[k].name + '</span>';
        html += '<span class="diag-item-msg">检测中...</span>';
        html += '</div>';
    }
    html += '</div>';
    container.innerHTML = html;

    for (var m = 0; m < items.length; m++) {
        await new Promise(function(resolve) { setTimeout(resolve, 80 + Math.random() * 120); });
        var item = items[m];
        var el = container.querySelector('[data-diag-idx="' + m + '"]');
        if (!el) continue;
        var cls = item.ok ? 'diag-item-ok' : 'diag-item-fail';
        var icon = item.ok ? '&#10003;' : '&#10007;';
        el.className = 'diag-item ' + cls;
        el.querySelector('.diag-item-icon').className = 'diag-item-icon';
        el.querySelector('.diag-item-icon').innerHTML = icon;
        el.querySelector('.diag-item-msg').textContent = item.message;
    }

    btn.disabled = false;
    btn.textContent = '运行自检';
}

function renderDiagSection(title, result) {
    var items = result.items || [];
    var okCount = result.ok_count || items.filter(function(i) { return i.ok; }).length;
    var failCount = result.fail_count || items.filter(function(i) { return !i.ok; }).length;
    var total = result.total || items.length;
    var allOk = failCount === 0;
    var html = '<div class="diag-section">';
    html += '<div class="diag-section-header">';
    html += '<span class="diag-section-title">' + title + '</span>';
    html += '<span class="diag-badge ' + (allOk ? 'diag-badge-ok' : 'diag-badge-fail') + '">';
    html += allOk ? '全部通过' : (okCount + '/' + total + ' 通过');
    html += '</span></div>';
    html += '<div class="diag-items">';
    items.forEach(function(item) {
        var cls = item.ok ? 'diag-item-ok' : 'diag-item-fail';
        var icon = item.ok ? '&#10003;' : '&#10007;';
        html += '<div class="diag-item ' + cls + '">';
        html += '<span class="diag-item-icon">' + icon + '</span>';
        html += '<span class="diag-item-name">' + item.name + '</span>';
        html += '<span class="diag-item-msg">' + item.message + '</span>';
        html += '</div>';
    });
    html += '</div></div>';
    return html;
}

// ==================== Help ====================

function showHelp() { $('helpModal').style.display = 'flex'; }
function closeHelp() { $('helpModal').style.display = 'none'; }

// ==================== Emergency Nuke ====================

var _nukeEventSource = null;

async function emergencyNuke(mode) {
    var modeLabel = mode === 'adb' ? '有线(ADB)' : '无线(HTTP)';
    var confirmMsg = '🚨 紧急修复 🚨\n\n'
        + '此功能用于设备频繁重启时紧急恢复。\n'
        + '将高频轮询设备连接状态，一旦检测到设备上线，\n'
        + '立即执行恢复出厂设置（删除所有自定义程序）。\n\n'
        + '模式：' + modeLabel + '\n\n'
        + '⚠️ 确定要执行吗？此操作不可撤销！';
    
    if (!await showConfirm(confirmMsg, {danger: true, icon: '🚨', okText: '执行紧急修复'})) return;
    
    if (!await showConfirm('⚠️ 最后确认 ⚠️\n\n即将开始高频轮询，检测到设备后立即清理。\n\n确定继续？', {danger: true, icon: '⚠️', okText: '确认执行'})) return;
    
    var body = { mode: mode };
    if (mode === 'adb') {
        if (!S.adb.serial) {
            toast('请先扫描并选择ADB设备', 'error');
            return;
        }
        body.serial = S.adb.serial;
    } else {
        if (!S.wl.host) {
            toast('请先输入设备IP地址', 'error');
            return;
        }
        body.host = S.wl.host;
    }
    
    toast('紧急修复已启动，正在轮询设备...', 'info');
    
    if (_nukeEventSource) {
        _nukeEventSource.close();
        _nukeEventSource = null;
    }
    
    try {
        var resp = await fetch('/api/emergency-nuke', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
        });
        
        var reader = resp.body.getReader();
        var decoder = new TextDecoder();
        var buffer = '';
        
        while (true) {
            var result = await reader.read();
            if (result.done) break;
            buffer += decoder.decode(result.value, { stream: true });
            
            var lines = buffer.split('\n');
            buffer = lines.pop();
            
            for (var i = 0; i < lines.length; i++) {
                var line = lines[i].trim();
                if (line.startsWith('data: ')) {
                    try {
                        var event = JSON.parse(line.substring(6));
                        if (event.status === 'polling') {
                            toast('轮询中... 第 ' + event.attempt + ' 次 (' + event.elapsed + 's)', 'info');
                        } else if (event.status === 'detected') {
                            toast('🎯 检测到设备！正在执行清理...', 'success');
                        } else if (event.status === 'success') {
                            toast('✅ 紧急修复成功！设备已恢复出厂设置', 'success');
                            if (mode === 'adb') {
                                adbDisconnect();
                            } else {
                                stopPolling();
                                S.wl.connected = false;
                                S.wl.xwebd = false;
                                S.wl.sair = false;
                                updateConnUI(false);
                                updateConnStatus('xwebdConnStatus', false);
                                updateConnStatus('assistantConnStatus', false);
                                $('btnConnect').textContent = '连接';
                                $('btnConnect').className = 'btn btn-primary';
                                $('btnConnect').disabled = false;
                                resetWirelessUI();
                            }
                        } else if (event.status === 'partial') {
                            toast('⚠️ 清理命令已执行，但可能不完整', 'info');
                        } else if (event.status === 'timeout') {
                            toast('❌ 等待超时，设备未上线', 'error');
                        }
                    } catch(e) {}
                }
            }
        }
    } catch(e) {
        toast('紧急修复请求失败: ' + e.message, 'error');
    }
}

// ==================== Init ====================

document.addEventListener('DOMContentLoaded', function() {
    var flashStyle = document.createElement('style');
    flashStyle.textContent = '.flash{animation:flash-anim .35s ease}@keyframes flash-anim{0%{opacity:1}30%{opacity:.4}100%{opacity:1}}';
    document.head.appendChild(flashStyle);

    document.addEventListener('mousemove', function(e) {
        var card = e.target.closest('.card');
        if (!card) return;
        var rect = card.getBoundingClientRect();
        var y = ((e.clientY - rect.top) / rect.height) * 100;
        card.style.setProperty('--glow-y', y + '%');
        card.style.setProperty('--glow-top', Math.max(0, y - 25) + '%');
        card.style.setProperty('--glow-bottom', Math.min(100, y + 25) + '%');
    });

    applyMode();
    updateSairLocks();
    renderMcpTools();

    document.querySelectorAll('.svc-list,.file-container,.log-container,.diag-container,.mcp-tools-list,.adb-device-list,.modal').forEach(function(el) {
        el.addEventListener('wheel', function(e) {
            var st = el.scrollTop;
            var atTop = st <= 0;
            var atBottom = st + el.clientHeight >= el.scrollHeight;
            if ((atTop && e.deltaY < 0) || (atBottom && e.deltaY > 0)) {
                e.preventDefault();
            }
        }, { passive: false });
    });

    document.addEventListener('wheel', function(e) {
        var el = e.target.closest('.process-table-scroll');
        if (!el) return;
        var st = el.scrollTop;
        var atTop = st <= 0;
        var atBottom = st + el.clientHeight >= el.scrollHeight;
        if ((atTop && e.deltaY < 0) || (atBottom && e.deltaY > 0)) {
            e.preventDefault();
        }
    }, { passive: false });

    document.addEventListener('click', function(e) {
        if (e.target.classList.contains('btn-copy')) {
            copyActivationCode(e.target);
        }
    });

    $('cfgListeningMode').addEventListener('change', function() {
        updateAecVisibility();
        var mode = this.value;
        var label = mode === 'realtime' ? 'Realtime（实时模式）' : 'AutoStop（自动停止）';
        toast('监听模式已切换为 ' + label + '，点击「保存配置」生效', 'info');
    });

    connectPanelSSE();
    refreshLogPanel('panel', false);
    window.addEventListener('resize', updateViewportHeight);

    // 滚动 bug 修复: viewport 高度此前只在模式切换/个别数据回调时重算,
    // 无线页内容异步加载晚于量高时底部被 overflow:hidden 裁掉(滚不动),
    // 切换模式再切回才恢复。ResizeObserver 盯住两页, 内容变高自动跟随。
    // (page 宽度恒为 50% 与 viewport 高度无关, 不会造成回调循环)
    if (window.ResizeObserver) {
        var ro = new ResizeObserver(function () { updateViewportHeight(); });
        ro.observe($('wiredPage'));
        ro.observe($('wirelessPage'));
    }
});
