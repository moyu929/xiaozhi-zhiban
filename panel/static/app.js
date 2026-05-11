var S = {
    mode: 'wired',
    adb: { serial: null, connected: false },
    wl: { host: '', connected: false, xwebd: false, sair: false },
    volume: 20, brightness: 80, muted: false,
    currentPath: '/var/upgrade',
    timers: { status: null, adbInfo: null, volume: null, brightness: null, reboot: null, panelLogPoll: null },
    rebootStart: 0,
    confirmResolve: null,
    panelSSE: null,
    deviceSSE: {}
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
    try {
        var r = await fetch(path, {
            method: opts.method || 'GET',
            headers: opts.headers || {},
            body: opts.body || undefined,
        });
        var data = await r.json();
        return data;
    } catch (e) {
        return { ok: false, error: e.message };
    }
}

// ==================== Confirm Dialog ====================

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
    if (!viewport) return;
    var target = S.mode === 'wired'
        ? document.querySelector('#wiredPage .panel-page-inner')
        : document.querySelector('#wirelessPage .panel-page-inner');
    if (target) viewport.style.height = target.scrollHeight + 'px';
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
        $('btnConnect').disabled = false;
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
    var r = await api('/api/assistant/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    if (r.ok) { toast('语音助手已卸载', 'success'); adbRefreshStatus(); }
    else toast('卸载失败', 'error');
}

async function adbPoweroff() {
    if (!await showConfirm('确定关机？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/adb/poweroff', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
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
        $('btnConnect').disabled = false;
        resetWirelessUI();
        toast('恢复出厂设置完成，设备将重启', 'success');
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
    container.scrollTop = container.scrollHeight;
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
        btn.textContent = '连接';
        btn.disabled = false;
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
    showOverlay('deviceOverlay');
    showOverlay('assistantOverlay');
    showOverlay('xwebdOverlay');

    if (S.adb.serial) {
        if (S.timers.adbInfo) { clearInterval(S.timers.adbInfo); S.timers.adbInfo = null; }
        S.adb.serial = null;
        S.adb.connected = false;
        $('btnAdbDisconnect').style.display = 'none';
        $('adbDeviceInfo').style.display = 'none';
        $('adbXwebdStatus').textContent = '未检测';
        $('adbXwebdStatus').className = 'svc-status svc-unknown';
        $('adbSairStatus').textContent = '未检测';
        $('adbSairStatus').className = 'svc-status svc-unknown';
        $('adbDeviceList').innerHTML = '<div class="empty-state">点击「扫描设备」检测通过USB连接的设备</div>';
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

    hideOverlay('deviceOverlay');
    hideOverlay('assistantOverlay');
    hideOverlay('xwebdOverlay');
    if (!S.wl.connected) { btn.disabled = false; btn.textContent = '连接'; }
}

function resetWirelessUI() {
    $('devModel').textContent = '--';
    $('devKernel').textContent = '--';
    $('devCpu').textContent = '--';
    $('devIp').textContent = '--';
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
    $('cfgWsUrl').value = '';
    $('cfgSairLogLevel').value = '';
    $('cfgLogLevel').value = '';
    $('cfgUploadMax').value = '10';
    $('diagContainer').innerHTML = '<div class="empty-state">点击「运行自检」检测设备是否满足语音助手运行环境</div>';
    $('fileContainer').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logXwebd').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logAssistant').innerHTML = '<div class="empty-state">等待连接设备...</div>';
    $('logPanel').innerHTML = '<div class="empty-state">暂无日志</div>';
    var svcIds = ['svcTelnet','svcWatchdog','svcAutostart','svcXwebd','svcAssistant'];
    for (var i = 0; i < svcIds.length; i++) {
        var el = $(svcIds[i]);
        if (el) { el.textContent = '未知'; el.className = 'svc-status svc-unknown'; }
    }
    $('btnXwebdRestart').style.display = '';
    $('btnXwebdRemove').style.display = '';
    $('btnXwebdUpdate').style.display = '';
    $('btnDeploy').disabled = false;
    $('footerInfo').textContent = '--';
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

function updateConnStatus(id, connected) {
    var el = document.getElementById(id);
    var dot = el.querySelector('.conn-dot');
    if (connected) {
        dot.className = 'conn-dot connected';
        el.lastChild.textContent = '已连接';
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
    refreshServices();
    refreshFiles();
    refreshLogPanel('panel', false);
    refreshLogPanel('xwebd', false);
    refreshLogPanel('assistant', false);
    refreshConfig();
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
        $('devIp').textContent = d.wifi_ip || d.ip || d.device_ip || S.wl.host || '--';
        var wifiOk = d.wifi_connected;
        if (S.wl.connected && !wifiOk && d.wifi_ip) wifiOk = true;
        $('devWifi').textContent = wifiOk ? '已连接' : '未连接';
        $('devBattery').textContent = d.battery_cap != null && d.battery_cap >= 0 ? d.battery_cap + '%' : '--';
        $('devUptime').textContent = formatUptime(d.uptime_s);
        $('devMem').textContent = formatMem(d.mem_free_kb, d.mem_total_kb, d.mem_cached_kb);
        $('devDisk').textContent = formatDisk(d.disk_used_kb, d.disk_total_kb);
        if (d.volume != null) {
            S.volume = d.volume;
            $('volumeSlider').value = Math.round(d.volume * 100 / 80);
            $('volumeVal').textContent = Math.round(d.volume * 100 / 80);
        }
        if (d.brightness != null) {
            S.brightness = d.brightness;
            $('brightnessSlider').value = Math.round(d.brightness * 100 / 900);
            $('brightnessVal').textContent = Math.round(d.brightness * 100 / 900);
        }
        if (d.muted != null) { S.muted = d.muted; updateMuteUI(); }
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
    else if (d.installed) label = '已安装 (未运行)';
    $('assistantInstalled').textContent = label;
    $('assistantVersion').textContent = d.version || '--';
    $('assistantPid').textContent = d.pid || '--';
    $('btnDeploy').disabled = d.installed;
    $('btnUpdate').disabled = false;
    if (d.activation_code) $('assistantActivation').textContent = d.activation_code;
    else $('assistantActivation').textContent = '--';
    if (d.ws_url) $('cfgWsUrl').value = d.ws_url;
    if (d.log_level) $('cfgSairLogLevel').value = d.log_level;
    var wasConnected = S.wl.sair;
    S.wl.sair = d.running;
    updateConnStatus('assistantConnStatus', d.running);
    if (wasConnected !== S.wl.sair) updateSairLocks();
}

async function refreshXwebdStatus() {
    if (!S.wl.connected) return;
    try {
        var vr = await api('/api/xwebd/version');
        if (!vr.error && vr.version) $('xwebdVersion').textContent = 'v' + vr.version;
    } catch(e) {}
    try {
        var sr = await api('/api/services');
        if (!sr.error && sr.telnet) {
            $('xwebdStatus').textContent = '运行中';
            updateConnStatus('xwebdConnStatus', true);
        }
    } catch(e) {}
}

async function refreshServices() {
    if (!S.wl.connected) return;
    var r = await api('/api/services');
    if (r.error) return;
    var d = r.data || r;
    setSvcStatus('svcTelnet', d.telnet && d.telnet.running, d.telnet ? (d.telnet.running ? '运行中' : '已停止') : '未知');
    var wd = d.boot_watchdog || {};
    setSvcStatus('svcWatchdog', wd.deployed || wd.running, (wd.running ? '运行中' : (wd.deployed ? '已部署' : '未部署')));
    var xw = d.xwebd || {};
    setSvcStatus('svcAutostart', xw.autostart, xw.autostart ? '自启动已启用' : '自启动未启用');
    setSvcStatus('svcXwebd', xw.running, xw.running ? '运行中' : '已停止');
    var sr = d.sair || {};
    setSvcStatus('svcAssistant', sr.running, sr.running ? '运行中' : (sr.installed ? '已停止' : '未安装'));
}

async function refreshServicesWithFlash() {
    await refreshServices();
    var svcList = document.querySelector('.svc-list');
    if (svcList) flashEl(svcList);
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
        if (r.ws_url) $('cfgWsUrl').value = r.ws_url;
    }
    var r2 = await api('/api/xwebd/config');
    if (!r2.error) {
        if (r2.log_level) $('cfgLogLevel').value = r2.log_level;
        $('cfgUploadMax').value = r2.upload_max_mb != null ? r2.upload_max_mb : 10;
    }
}

async function onVolumeChange(val) {
    var realVol = Math.round(parseInt(val) * 80 / 100);
    S.volume = realVol;
    $('volumeVal').textContent = val;
    if (S.timers.volume) clearTimeout(S.timers.volume);
    S.timers.volume = setTimeout(async function() {
        await api('/api/volume', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ volume: realVol }),
        });
    }, 300);
}

async function onBrightnessChange(val) {
    var realBri = Math.round(parseInt(val) * 900 / 100);
    S.brightness = realBri;
    $('brightnessVal').textContent = val;
    if (S.timers.brightness) clearTimeout(S.timers.brightness);
    S.timers.brightness = setTimeout(async function() {
        await api('/api/brightness', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ brightness: realBri }),
        });
    }, 300);
}

async function toggleMute() {
    S.muted = !S.muted;
    updateMuteUI();
    await api('/api/mute', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ muted: S.muted }),
    });
}

function updateMuteUI() {
    var btn = $('btnMute');
    if (S.muted) {
        btn.classList.add('muted');
        $('iconUnmuted').style.display = 'none';
        $('iconMuted').style.display = 'block';
    } else {
        btn.classList.remove('muted');
        $('iconUnmuted').style.display = 'block';
        $('iconMuted').style.display = 'none';
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

async function doPoweroff() {
    if (!await showConfirm('确定要关机吗？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/poweroff', { method: 'POST' });
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
    var config = { ws_url: $('cfgWsUrl').value };
    var logLevel = $('cfgSairLogLevel').value;
    if (logLevel) config.log_level = logLevel;
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config),
    });
    if (r.ok || !r.error) toast('助手配置已保存', 'success');
    else toast('保存失败', 'error');
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
    if (!await showConfirm('确定恢复助手配置为默认值？', {icon: '🔄'})) return;
    $('cfgWsUrl').value = 'wss://api.tenclass.net/xiaozhi/v1/';
    $('cfgSairLogLevel').value = '';
    await saveAssistantConfig();
}

async function restoreXwebdDefaults() {
    if (!await showConfirm('确定恢复面板内核配置为默认值？', {icon: '🔄'})) return;
    $('cfgLogLevel').value = 'DEBUG';
    $('cfgUploadMax').value = '10';
    await saveXwebdConfig();
}

async function doHotUpdate() {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error || !sd.installed) {
        toast('语音助手未安装，请先点击「部署」按钮', 'error');
        return;
    }
    if (!await showConfirm('确定进行热更新？')) return;
    toast('正在检查更新...', 'info');
    var r = await api('/api/assistant/upgrade', { method: 'POST' });
    if (r.ok) toast('热更新指令已发送', 'success');
    else toast('热更新失败: ' + (r.error || ''), 'error');
}

async function doDeploy() {
    if (!await showConfirm('确定部署语音助手？\n\n部署过程中设备将重启一次')) return;
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
    if (r.ok) {
        label.textContent = '部署成功！设备重启中...';
        toast('语音助手部署成功，设备重启中...', 'success');
        stopPolling();
        S.wl.connected = false;
        S.wl.xwebd = false;
        S.wl.sair = false;
        updateConnUI(false);
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        $('btnConnect').textContent = '连接';
        $('btnConnect').disabled = false;
        resetWirelessUI();
        setTimeout(function() {
            toast('设备重启中，约30秒后可重新连接', 'info');
        }, 2000);
    } else {
        label.textContent = '部署失败: ' + (r.error || '');
        toast('部署失败: ' + (r.error || ''), 'error');
    }
}

async function doUpdate() {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error || !sd.installed) {
        toast('语音助手未安装，请先点击「部署」按钮', 'error');
        return;
    }
    if (!await showConfirm('确定进行冷更新？设备将重启')) return;
    toast('冷更新中...', 'info');
    var r = await api('/api/assistant/update', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({}),
    });
    if (r.ok) {
        toast('冷更新指令已发送，设备将重启', 'success');
        wirelessRebootAndReconnect();
    } else {
        toast('冷更新失败: ' + (r.error || ''), 'error');
    }
}

async function doUninstall() {
    if (!await showConfirm('确定卸载语音助手？设备将停止语音助手功能', {danger: true})) return;
    toast('卸载中...', 'info');
    var r = await api('/api/assistant/uninstall', { method: 'POST' });
    if (r.ok) { toast('语音助手已卸载', 'success'); refreshAssistantStatus(); }
    else toast('卸载失败', 'error');
}

async function doXwebdUpdate() {
    toast('更新xwebd中...', 'info');
    if (S.wl.connected) {
        var r = await api('/api/xwebd/wireless-update', { method: 'POST' });
        if (r.ok) {
            toast('xwebd 更新成功，正在重启...', 'success');
            wirelessRebootAndReconnect();
        } else {
            toast('更新失败: ' + (r.error || ''), 'error');
        }
    } else {
        var r = await api('/api/xwebd/upload-update', { method: 'POST' });
        if (r.ok) toast('xwebd 更新成功', 'success');
        else toast('更新失败: ' + (r.error || ''), 'error');
    }
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
            $('adbDeviceOverlay').style.display = 'none';
            toast('设备已重新连接', 'success');
            await selectAdbDevice(S.adb.serial);
        },
        onTimeout: function() {
            $('adbDeviceOverlay').style.display = 'none';
            toast('重连超时，请手动扫描', 'error');
        }
    };
}

function adbWaitForReconnect() {
    toast('设备重启中，等待重新连接...', 'info');
    $('adbDeviceOverlay').style.display = 'flex';
    waitForReconnect(_adbReconnectOpts());
}

async function adbRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    $('adbDeviceOverlay').style.display = 'flex';
    await api('/api/adb/reboot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: S.adb.serial }),
    });
    waitForReconnect(_adbReconnectOpts());
}

async function wirelessRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    showOverlay('deviceOverlay');
    showOverlay('assistantOverlay');
    showOverlay('xwebdOverlay');
    await api('/api/reboot', { method: 'POST' });
    S.wl.connected = false;
    S.wl.xwebd = false;
    S.wl.sair = false;
    stopPolling();
    updateConnUI(false);
    updateConnStatus('xwebdConnStatus', false);
    updateConnStatus('assistantConnStatus', false);
    $('btnConnect').textContent = '连接';
    $('btnConnect').disabled = false;
    waitForReconnect({
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
            updateConnUI(true);
            $('btnConnect').textContent = '断开';
            $('btnConnect').disabled = false;
            startPolling();
            refreshAll();
            updateConnStatus('xwebdConnStatus', true);
            hideOverlay('deviceOverlay');
            hideOverlay('assistantOverlay');
            hideOverlay('xwebdOverlay');
            toast('设备已重新连接', 'success');
        },
        onTimeout: function() {
            hideOverlay('deviceOverlay');
            hideOverlay('assistantOverlay');
            hideOverlay('xwebdOverlay');
            toast('重连超时，请手动连接', 'error');
        }
    });
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
    container.scrollTop = container.scrollHeight;
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
    if (r.error) return;
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

    if (!files.length) { container.innerHTML = html + '<div class="empty-state">空目录</div>'; return; }
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
    connectPanelSSE();
    refreshLogPanel('panel', false);
    window.addEventListener('resize', updateViewportHeight);
});
