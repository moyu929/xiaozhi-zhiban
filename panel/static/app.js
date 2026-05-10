var g_mode = 'wired';
var g_adbSerial = null;
var g_adbConnected = false;

var g_deviceHost = '';
var g_connected = false;
var g_xwebdConnected = false;
var g_sairConnected = false;
var g_volume = 20;
var g_brightness = 80;
var g_muted = false;
var g_currentPath = '/var/upgrade';
var g_logTimer = null;
var g_statusTimer = null;
var g_confirmResolve = null;
var g_volumeTimer = null;
var g_brightnessTimer = null;
var g_clearLineXwebd = -1;
var g_clearLineAssistant = -1;
var g_clearLinePanel = -1;
var g_lastLogCountXwebd = 0;
var g_lastLogCountAssistant = 0;
var g_lastLogCountPanel = 0;
var g_panelSSE = null;
var g_rebootTimer = null;
var g_rebootStartTime = 0;

var STATE_MAP = {
    'Idle': '空闲',
    'Connecting': '连接中',
    'Listening': '监听中',
    'Speaking': '说话中',
    'Cleaning': '清理中',
    'Starting': '启动中',
    'Activating': '激活中'
};

function $(id) { return document.getElementById(id); }

function showConfirm(msg, opts) {
    opts = opts || {};
    return new Promise(function(resolve) {
        g_confirmResolve = resolve;
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
    if (g_confirmResolve) {
        g_confirmResolve(result);
        g_confirmResolve = null;
    }
}

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

function showHelp() { $('helpModal').style.display = 'flex'; }
function closeHelp() { $('helpModal').style.display = 'none'; }

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

function toggleMode() {
    if (g_mode === 'wired') {
        g_mode = 'wireless';
    } else {
        g_mode = 'wired';
    }
    applyMode();
}

function applyMode() {
    var track = $('modeTrack');
    var slider = $('panelsSlider');

    if (g_mode === 'wireless') {
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
    var wiredInner = document.querySelector('#wiredPage .panel-page-inner');
    var wirelessInner = document.querySelector('#wirelessPage .panel-page-inner');
    var target = g_mode === 'wired' ? wiredInner : wirelessInner;
    if (target) {
        viewport.style.height = target.scrollHeight + 'px';
    }
}

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
        var selected = (g_adbSerial === d.serial) ? ' selected' : '';
        var stateLabel = (d.state === 'device' || !d.state || d.state === 'unknown') ? '' : (d.state === 'unauthorized' ? '（未授权，请在设备上确认）' : '（' + d.state + '）');
        var stateCls = (d.state === 'device' || !d.state || d.state === 'unknown') ? '' : ' svc-off';
        var xwebdLabel = d.xwebd_installed ? (d.xwebd_status && d.xwebd_status.running ? '内核运行中' : '内核已安装') : '未安装内核';
        var xwebdCls = d.xwebd_installed ? (d.xwebd_status && d.xwebd_status.running ? 'svc-on' : 'svc-off') : 'svc-unknown';
        var initLabel = '';
        if (d.state === 'device' || d.state === 'unknown' || !d.state) {
            var initInfo = d.initialized || {};
            if (!initInfo.initialized) {
                initLabel = ' · 新设备';
            }
        }
        var model = (d.model && d.model !== 'unknown') ? d.model : ((d.device && d.device !== 'unknown') ? d.device : null);
        var deviceName = model || d.serial;
        var deviceSub = model ? d.serial : '';
        var clickable = (d.state === 'device' || d.state === 'unknown' || !d.state) ? '" onclick="selectAdbDevice(\'' + d.serial + '\')' : ' device-unauthorized"';
        html += '<div class="adb-device-item' + selected + clickable + '" style="display:grid;grid-template-columns:1fr auto;align-items:center">';
        html += '<div class="adb-device-info" style="display:flex;flex-direction:column;gap:1px">';
        html += '<div class="adb-device-serial">' + escapeHtml(deviceName) + '</div>';
        if (deviceSub) html += '<div class="adb-device-model">' + escapeHtml(deviceSub) + stateLabel + '</div>';
        else html += '<div class="adb-device-model">' + stateLabel + '</div>';
        html += '</div>';
        html += '<div style="display:flex;flex-direction:column;align-items:flex-end;gap:4px">';
        if (d.state === 'device' || d.state === 'unknown' || !d.state) {
            html += '<div class="adb-device-status svc-status ' + xwebdCls + '">' + xwebdLabel + initLabel + '</div>';
        } else {
            html += '<div class="adb-device-status svc-status' + stateCls + '">' + d.state + '</div>';
        }
        html += '</div>';
        html += '</div>';
    }
    $('adbDeviceList').innerHTML = html;

    if (!g_adbSerial && devices.length === 1) {
        selectAdbDevice(devices[0].serial);
    }
    toast('检测到 ' + devices.length + ' 台设备', 'success');
    updateViewportHeight();
}

function adbDisconnect() {
    g_adbSerial = null;
    g_adbConnected = false;
    $('btnAdbDisconnect').style.display = 'none';
    $('adbDeviceInfo').style.display = 'none';
    adbDetect();
    toast('已断开设备连接', 'info');
}

async function selectAdbDevice(serial) {
    g_adbSerial = serial;
    g_adbConnected = true;
    $('btnAdbDisconnect').style.display = '';

    var items = document.querySelectorAll('.adb-device-item');
    for (var i = 0; i < items.length; i++) {
        items[i].classList.toggle('selected', items[i].querySelector('.adb-device-serial').textContent === serial);
    }

    await adbRefreshStatus();
    await adbRefreshDeviceInfo();
    await adbRefreshLogs();
}

async function adbRefreshStatus() {
    if (!g_adbSerial) return;
    var r = await api('/api/adb/sair-status?serial=' + encodeURIComponent(g_adbSerial));
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
        var installed = await api('/api/adb/check?serial=' + encodeURIComponent(g_adbSerial));
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
        $('btnAdbDeploySairHot').style.display = '';
        $('btnAdbDeploySairCold').style.display = 'none';
        $('btnAdbRemoveSair').style.display = '';
    } else if (sair.native_running) {
        sairEl.textContent = '原生运行中';
        sairEl.className = 'svc-status svc-off';
        $('btnAdbDeploySairHot').style.display = '';
        $('btnAdbDeploySairCold').style.display = '';
        $('btnAdbRemoveSair').style.display = 'none';
    } else if (sair.custom_installed) {
        sairEl.textContent = sairVer ? '已安装 ' + sairVer : '已安装';
        sairEl.className = 'svc-status svc-off';
        $('btnAdbDeploySairHot').style.display = '';
        $('btnAdbDeploySairCold').style.display = '';
        $('btnAdbRemoveSair').style.display = '';
    } else {
        sairEl.textContent = '未安装';
        sairEl.className = 'svc-status svc-unknown';
        $('btnAdbDeploySairHot').style.display = '';
        $('btnAdbDeploySairCold').style.display = '';
        $('btnAdbRemoveSair').style.display = 'none';
    }
}

async function adbRefreshDeviceInfo() {
    if (!g_adbSerial) return;
    var r = await api('/api/adb/device-info?serial=' + encodeURIComponent(g_adbSerial));
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
    var initR = await api('/api/adb/init-status?serial=' + encodeURIComponent(g_adbSerial || ''));
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
        body: JSON.stringify({ serial: g_adbSerial }),
    });

    bar.style.width = '100%';
    $('btnAdbDeployXwebd').disabled = false;

    if (r.ok) {
        label.textContent = '完成';
        toast('面板内核部署成功', 'success');
        if (r.ip) {
            $('deviceHost').value = r.ip;
        }
        setTimeout(function() {
            progress.style.display = 'none';
            adbRefreshStatus();
        }, 1500);
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
        body: JSON.stringify({ serial: g_adbSerial }),
    });
    if (r.ok) {
        toast('面板内核已启动', 'success');
        adbRefreshStatus();
    } else {
        toast('启动失败', 'error');
    }
}

async function adbRestartXwebd() {
    if (!await showConfirm('确定重启面板内核？')) return;
    toast('正在重启面板内核...', 'info');
    var r = await api('/api/xwebd/restart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: g_adbSerial }),
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
        body: JSON.stringify({ serial: g_adbSerial }),
    });
    if (r.ok) {
        toast('面板内核已卸载', 'success');
        adbRefreshStatus();
    } else {
        toast('卸载失败', 'error');
    }
}

async function adbDeploySair(mode) {
    mode = mode || 'cold';
    var confirmMsg = mode === 'hot' ? '确定热部署语音助手？（SIGUSR2热更新，进程不中断）' : '确定冷部署语音助手？（上传sair后设备将重启）';
    if (!await showConfirm(confirmMsg)) return;
    var progress = $('adbSairProgress');
    var bar = $('adbSairBar');
    var label = $('adbSairProgressLabel');
    progress.style.display = 'inline-flex';
    bar.style.width = '10%';
    label.textContent = mode === 'hot' ? '热部署中...' : '冷部署中...';
    $('btnAdbDeploySairHot').disabled = true;
    $('btnAdbDeploySairCold').disabled = true;

    var r = await api('/api/adb/deploy-sair', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: g_adbSerial, mode: mode }),
    });

    bar.style.width = '100%';
    $('btnAdbDeploySairHot').disabled = false;
    $('btnAdbDeploySairCold').disabled = false;

    if (r.ok) {
        label.textContent = '完成';
        if (r.rebooting) {
            toast('语音助手冷部署成功，设备重启中...', 'success');
            setTimeout(function() {
                progress.style.display = 'none';
                adbWaitForReconnect();
            }, 1500);
        } else {
            toast(mode === 'hot' ? '语音助手热部署成功' : '语音助手冷部署成功', 'success');
            setTimeout(function() {
                progress.style.display = 'none';
                adbRefreshStatus();
            }, 1500);
        }
    } else {
        label.textContent = '失败';
        toast('语音助手部署失败: ' + (r.error || ''), 'error');
    }
}

function adbWaitForReconnect() {
    toast('设备重启中，等待重新连接...', 'info');
    $('adbDeviceOverlay').style.display = 'flex';
    g_rebootStartTime = Date.now();
    if (g_rebootTimer) clearInterval(g_rebootTimer);
    g_rebootTimer = setInterval(async function() {
        if (Date.now() - g_rebootStartTime > 60000) {
            clearInterval(g_rebootTimer);
            g_rebootTimer = null;
            $('adbDeviceOverlay').style.display = 'none';
            toast('重连超时，请手动扫描', 'error');
            return;
        }
        var r = await api('/api/adb/devices');
        if (r.error) return;
        var devices = r.devices || [];
        for (var i = 0; i < devices.length; i++) {
            if (devices[i].serial === g_adbSerial && devices[i].state === 'device') {
                clearInterval(g_rebootTimer);
                g_rebootTimer = null;
                $('adbDeviceOverlay').style.display = 'none';
                toast('设备已重新连接', 'success');
                await selectAdbDevice(g_adbSerial);
                return;
            }
        }
    }, 3000);
}

async function adbRemoveSair() {
    if (!await showConfirm('确定卸载语音助手？', {danger: true})) return;
    toast('正在卸载语音助手...', 'info');
    var r = await api('/api/assistant/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: g_adbSerial }),
    });
    if (r.ok) {
        toast('语音助手已卸载', 'success');
        adbRefreshStatus();
    } else {
        toast('卸载失败', 'error');
    }
}

async function adbReboot() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    toast('正在重启设备...', 'info');
    await api('/api/adb/reboot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: g_adbSerial }),
    });
}

async function adbPoweroff() {
    if (!await showConfirm('确定关机？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/adb/poweroff', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ serial: g_adbSerial }),
    });
}

async function rebootAndReconnect(rebootFn, checkFn) {
    await rebootFn();
    toast('设备重启中，等待重新连接...', 'info');
    g_rebootStartTime = Date.now();
    if (g_rebootTimer) clearInterval(g_rebootTimer);
    g_rebootTimer = setInterval(async function() {
        if (Date.now() - g_rebootStartTime > 60000) {
            clearInterval(g_rebootTimer);
            g_rebootTimer = null;
            toast('重连超时，请手动连接', 'error');
            return;
        }
        await checkFn();
    }, 3000);
}

async function adbRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    $('adbDeviceOverlay').style.display = 'flex';
    await rebootAndReconnect(
        async function() {
            await api('/api/adb/reboot', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ serial: g_adbSerial }),
            });
        },
        async function() {
            var r = await api('/api/adb/devices');
            if (r.error) return;
            var devices = r.devices || [];
            for (var i = 0; i < devices.length; i++) {
                if (devices[i].serial === g_adbSerial && devices[i].state === 'device') {
                    clearInterval(g_rebootTimer);
                    g_rebootTimer = null;
                    $('adbDeviceOverlay').style.display = 'none';
                    toast('设备已重新连接', 'success');
                    await selectAdbDevice(g_adbSerial);
                    return;
                }
            }
        }
    );
    if (g_rebootTimer === null && $('adbDeviceOverlay').style.display !== 'none') {
        $('adbDeviceOverlay').style.display = 'none';
    }
}

async function wirelessRebootAndReconnect() {
    if (!await showConfirm('确定重启设备？', {danger: true})) return;
    showOverlay('deviceOverlay');
    showOverlay('assistantOverlay');
    showOverlay('xwebdOverlay');
    await rebootAndReconnect(
        async function() {
            await api('/api/reboot', { method: 'POST' });
            g_connected = false;
            g_xwebdConnected = false;
            g_sairConnected = false;
            stopPolling();
            updateConnUI(false);
            updateConnStatus('xwebdConnStatus', false);
            updateConnStatus('assistantConnStatus', false);
            $('btnConnect').textContent = '连接';
            $('btnConnect').disabled = false;
        },
        async function() {
            var resp = await fetch('/api/connect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ host: g_deviceHost }),
            });
            var r = await resp.json();
            if (r.ok && r.xwebd_connected) {
                clearInterval(g_rebootTimer);
                g_rebootTimer = null;
                g_connected = true;
                g_xwebdConnected = true;
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
            }
        }
    );
    if (g_rebootTimer === null) {
        hideOverlay('deviceOverlay');
        hideOverlay('assistantOverlay');
        hideOverlay('xwebdOverlay');
    }
}

async function adbRefreshLogs() {
    await adbRefreshLogType('xwebd');
    await adbRefreshLogType('sair');
}

async function adbRefreshLogType(type) {
    if (!g_adbSerial) return;
    var r = await api('/api/adb/logs?serial=' + encodeURIComponent(g_adbSerial) + '&type=' + type + '&lines=80');
    if (!r || r.error) return;
    var logs = r.logs || {};
    var lines = logs[type] || [];
    var containerId = type === 'xwebd' ? 'adbLogXwebd' : 'adbLogSair';
    var container = $(containerId);
    if (!container) return;
    if (!lines.length) {
        container.innerHTML = '<div class="empty-state">暂无日志</div>';
        return;
    }
    var html = '';
    for (var i = 0; i < lines.length; i++) {
        var l = lines[i];
        if (!l) continue;
        html += '<div class="log-line">' + renderLogLine(l) + '</div>';
    }
    container.innerHTML = html;
    container.scrollTop = container.scrollHeight;
}

// ==================== 无线模式 ====================

async function connectDevice() {
    var btn = $('btnConnect');
    if (g_connected) {
        stopPolling();
        g_connected = false;
        g_xwebdConnected = false;
        g_sairConnected = false;
        updateConnUI(false);
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
        btn.textContent = '连接';
        btn.disabled = false;
        toast('已断开连接', 'info');
        return;
    }
    var host = $('deviceHost').value.trim();
    if (!host) {
        toast('请输入设备IP地址', 'error');
        return;
    }
    g_deviceHost = host;
    btn.disabled = true;
    btn.textContent = '连接中...';
    flashEl(btn);

    showOverlay('deviceOverlay');
    showOverlay('assistantOverlay');
    showOverlay('xwebdOverlay');

    try {
        var resp = await fetch('/api/connect', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ host: host }),
        });
        var r = await resp.json();

        if (r.ok && r.xwebd_connected) {
            g_connected = true;
            g_xwebdConnected = true;
            updateConnUI(true);
            btn.textContent = '断开';
            btn.disabled = false;
            toast('连接设备成功', 'success');
            if (g_adbSerial) adbDisconnect();
            startPolling();
            refreshAll();
            updateConnStatus('xwebdConnStatus', true);

            try {
                var ar = await api('/api/assistant/status');
                var ad = ar.data || ar;
                if (!ar.error && ad.running) {
                    g_sairConnected = true;
                    updateConnStatus('assistantConnStatus', true);
                    toast('语音助手已连接', 'success');
                } else {
                    g_sairConnected = false;
                    updateConnStatus('assistantConnStatus', false);
                    toast('语音助手未运行', 'info');
                }
            } catch (e) {
                g_sairConnected = false;
                updateConnStatus('assistantConnStatus', false);
            }
            updateSairLocks();
        } else {
            g_xwebdConnected = false;
            g_sairConnected = false;
            updateConnStatus('xwebdConnStatus', false);
            updateConnStatus('assistantConnStatus', false);
            updateSairLocks();
            toast('连接失败', 'error');
        }
    } catch (e) {
        g_xwebdConnected = false;
        g_sairConnected = false;
        updateSairLocks();
        toast('连接错误：面板服务器未启动', 'error');
        updateConnStatus('xwebdConnStatus', false);
        updateConnStatus('assistantConnStatus', false);
    }

    hideOverlay('deviceOverlay');
    hideOverlay('assistantOverlay');
    hideOverlay('xwebdOverlay');

    if (!g_connected) {
        btn.disabled = false;
        btn.textContent = '连接';
    }
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

function updateSairLocks() {
    document.querySelectorAll('.sair-required').forEach(function(el) {
        if (g_sairConnected) {
            el.classList.remove('sair-locked');
        } else {
            el.classList.add('sair-locked');
        }
    });
}

function showOverlay(id) { document.getElementById(id).style.display = 'flex'; }
function hideOverlay(id) { document.getElementById(id).style.display = 'none'; }

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

function startPolling() {
    stopPolling();
    g_statusTimer = setInterval(refreshStatus, 3000);
    g_logTimer = setInterval(refreshDeviceLogs, 2000);
}

function stopPolling() {
    if (g_statusTimer) { clearInterval(g_statusTimer); g_statusTimer = null; }
    if (g_logTimer) { clearInterval(g_logTimer); g_logTimer = null; }
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
    var usedMb = Math.round(usedKb / 1024);
    var totalMb = Math.round(totalKb / 1024);
    return usedMb + '/' + totalMb + ' MB';
}

function formatFileTime(t) {
    if (!t) return '--';
    var d;
    if (typeof t === 'number') {
        d = new Date(t * 1000);
    } else {
        d = new Date(t);
    }
    if (isNaN(d.getTime())) return t;
    var pad = function(n) { return n < 10 ? '0' + n : n; };
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}

async function refreshStatus() {
    if (!g_connected) return;
    var r = await api('/api/status');
    if (r.error) return;
    var d = r.data || r;
    $('devModel').textContent = d.model || '--';
    $('devKernel').textContent = d.kernel || '--';
    $('devCpu').textContent = d.cpu || '--';
    $('devIp').textContent = d.wifi_ip || d.ip || d.device_ip || '--';
    $('devWifi').textContent = d.wifi_connected ? '已连接' : '未连接';
    $('devBattery').textContent = d.battery_cap != null ? d.battery_cap + '%' : '--';
    $('devUptime').textContent = formatUptime(d.uptime_s);
    $('devMem').textContent = formatMem(d.mem_free_kb, d.mem_total_kb, d.mem_cached_kb);
    $('devDisk').textContent = formatDisk(d.disk_used_kb, d.disk_total_kb);
    if (d.volume != null) {
        g_volume = d.volume;
        $('volumeSlider').value = Math.round(d.volume * 100 / 80);
        $('volumeVal').textContent = Math.round(d.volume * 100 / 80);
    }
    if (d.brightness != null) {
        g_brightness = d.brightness;
        $('brightnessSlider').value = Math.round(d.brightness * 100 / 900);
        $('brightnessVal').textContent = Math.round(d.brightness * 100 / 900);
    }
    if (d.muted != null) {
        g_muted = d.muted;
        updateMuteUI();
    }
    if (d.state) {
        var stateEl = $('assistantState');
        if (stateEl) {
            stateEl.textContent = STATE_MAP[d.state] || d.state;
            stateEl.className = 'status-pill state-' + d.state.toLowerCase();
        }
    }
    $('footerInfo').textContent = (d.model || '?') + ' · ' + (d.wifi_ip || '?');
}

async function refreshAssistantStatus() {
    if (!g_connected) return;
    var r = await api('/api/assistant/status');
    if (r.error) return;
    var d = r.data || r;
    var installed = d.installed;
    var running = d.running;
    var label = '未安装';
    if (installed && running) label = '运行中';
    else if (installed) label = '已安装 (未运行)';
    $('assistantInstalled').textContent = label;
    $('assistantVersion').textContent = d.version || '--';
    $('assistantPid').textContent = d.pid || '--';
    $('btnDeploy').disabled = installed;
    $('btnUpdate').disabled = false;
    if (d.activation_code) {
        $('assistantActivation').textContent = d.activation_code;
    }
    var wasSairConnected = g_sairConnected;
    g_sairConnected = running;
    updateConnStatus('assistantConnStatus', running);
    if (wasSairConnected !== g_sairConnected) updateSairLocks();
}

async function refreshXwebdStatus() {
    if (!g_connected) return;
    try {
        var vr = await api('/api/xwebd/version');
        if (!vr.error && vr.version) {
            $('xwebdVersion').textContent = 'v' + vr.version;
        }
    } catch(e) {}
    try {
        var sr = await api('/api/services');
        if (!sr.error && sr.telnet) {
            $('xwebdStatus').textContent = '运行中';
        }
    } catch(e) {}
}

async function refreshServices() {
    if (!g_connected) return;
    var r = await api('/api/services');
    if (r.error) return;
    var d = r.data || r;
    setSvcStatus('svcTelnet', d.telnet && d.telnet.running, d.telnet ? (d.telnet.running ? '运行中' : '已停止') : '未知');
    setSvcStatus('svcWatchdog', d.boot_watchdog && d.boot_watchdog.deployed, d.boot_watchdog ? (d.boot_watchdog.deployed ? '已部署' : '未部署') : '未知');
    setSvcStatus('svcAutostart', d.xwebd_autostart && d.xwebd_autostart.enabled, d.xwebd_autostart ? (d.xwebd_autostart.enabled ? '已启用' : '已禁用') : '未知');

    var ar = await api('/api/assistant/status');
    if (!ar.error) {
        var ad = ar.data || ar;
        setSvcStatus('svcAssistant', ad.running, ad.running ? '运行中 (PID ' + (ad.pid || '?') + ')' : (ad.native_running ? '原生运行中' : (ad.installed ? '已停止' : '未安装')));
        setSvcStatus('svcBackup', ad.native_backup_exists, ad.native_backup_exists ? '存在' : '无');
    }
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
    if (!g_connected) return;
    var r = await api('/api/assistant/config');
    if (!r.error) {
        if (r.ws_url) $('cfgWsUrl').value = r.ws_url;
        if (r.realtime_mode != null) $('cfgRealtime').checked = r.realtime_mode;
        if (r.aec_enabled != null) $('cfgAec').checked = r.aec_enabled;
    }
    var r2 = await api('/api/xwebd/config');
    if (!r2.error) {
        if (r2.log_level) $('cfgLogLevel').value = r2.log_level;
        if (r2.upload_max_mb != null) $('cfgUploadMax').value = r2.upload_max_mb;
        else $('cfgUploadMax').value = 10;
    }
}

async function onVolumeChange(val) {
    var realVol = Math.round(parseInt(val) * 80 / 100);
    g_volume = realVol;
    $('volumeVal').textContent = val;
    if (g_volumeTimer) clearTimeout(g_volumeTimer);
    g_volumeTimer = setTimeout(async function() {
        await api('/api/volume', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ volume: realVol }),
        });
    }, 300);
}

async function onBrightnessChange(val) {
    var realBri = Math.round(parseInt(val) * 900 / 100);
    g_brightness = realBri;
    $('brightnessVal').textContent = val;
    if (g_brightnessTimer) clearTimeout(g_brightnessTimer);
    g_brightnessTimer = setTimeout(async function() {
        await api('/api/brightness', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ brightness: realBri }),
        });
    }, 300);
}

async function toggleMute() {
    g_muted = !g_muted;
    updateMuteUI();
    await api('/api/mute', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ muted: g_muted }),
    });
}

function updateMuteUI() {
    var btn = $('btnMute');
    var iconOn = $('iconUnmuted');
    var iconOff = $('iconMuted');
    if (g_muted) {
        btn.classList.add('muted');
        iconOn.style.display = 'none';
        iconOff.style.display = 'block';
    } else {
        btn.classList.remove('muted');
        iconOn.style.display = 'block';
        iconOff.style.display = 'none';
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

async function doReboot() {
    if (!await showConfirm('确定要重启设备吗？', {danger: true})) return;
    toast('正在重启...', 'info');
    await api('/api/reboot', { method: 'POST' });
}

async function doPoweroff() {
    if (!await showConfirm('确定要关机吗？设备将完全断电', {danger: true})) return;
    toast('正在关机...', 'info');
    await api('/api/poweroff', { method: 'POST' });
}

async function doCleanup() {
    if (!await showConfirm('确定清理垃圾文件？', {icon: '🧹'})) return;
    toast('清理中...', 'info');
    var r = await api('/api/files/cleanup', { method: 'POST' });
    if (r.ok) {
        var freed = r.freed_bytes ? (r.freed_bytes / 1024).toFixed(1) + ' KB' : '';
        toast('清理完成' + (freed ? '，释放 ' + freed : ''), 'success');
        refreshFiles();
    } else {
        toast('清理失败', 'error');
    }
}

async function saveAssistantConfig() {
    var cfg = {
        ws_url: $('cfgWsUrl').value,
        realtime_mode: $('cfgRealtime').checked,
        aec_enabled: $('cfgAec').checked,
    };
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg),
    });
    if (r.ok || !r.error) toast('助手配置已保存', 'success');
    else toast('保存失败', 'error');
}

async function saveXwebdConfig() {
    var cfg = {
        log_level: $('cfgLogLevel').value,
        upload_max_mb: parseInt($('cfgUploadMax').value) || 10,
    };
    var r = await api('/api/xwebd/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg),
    });
    if (r.ok || !r.error) toast('xwebd 配置已保存', 'success');
    else toast('保存失败', 'error');
}

function restoreAssistantDefaults() {
    $('cfgWsUrl').value = 'wss://api.tenclass.net/xiaozhi/v1/';
    $('cfgRealtime').checked = false;
    $('cfgAec').checked = false;
    toast('已恢复默认值', 'info');
}

function restoreXwebdDefaults() {
    $('cfgLogLevel').value = 'DEBUG';
    $('cfgUploadMax').value = '10';
    toast('已恢复默认值', 'info');
}

async function doHotUpdate() {
    if (!await showConfirm('确定进行热更新？')) return;
    toast('正在检查更新...', 'info');
    var r = await api('/api/upgrade', { method: 'POST' });
    if (r.ok) toast('热更新指令已发送', 'success');
    else toast('热更新失败', 'error');
}

async function refreshFiles() {
    if (!g_connected) return;
    var r = await api('/api/files?path=' + encodeURIComponent(g_currentPath));
    if (r.error) return;
    var d = r.data || r;
    var files = d.files || [];
    var container = $('fileContainer');
    if (!files.length) {
        container.innerHTML = '<div class="empty-state">空目录</div>';
        return;
    }
    var parts = g_currentPath.split('/').filter(Boolean);
    var navHtml = '<span style="cursor:default;opacity:0.5">/</span>';
    var cumPath = '';
    parts.forEach(function(p, i) {
        cumPath += '/' + p;
        navHtml += '<span class="file-nav-sep">/</span>';
        if (i === 0) {
            navHtml += '<span style="cursor:default;opacity:0.5">' + p + '</span>';
        } else if (i < parts.length - 1) {
            navHtml += '<span class="file-nav-link" onclick="navigateTo(\'' + cumPath + '\')">' + p + '</span>';
        } else {
            navHtml += '<span>' + p + '</span>';
        }
    });

    var html = '<div class="file-nav">' + navHtml + '</div>';
    html += '<table class="file-table"><thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead><tbody>';
    files.forEach(function(f) {
        var nameClass = f.is_dir ? 'file-dir-link' : (f.protected ? 'file-name-cell file-protected' : 'file-name-cell');
        var filePath = g_currentPath.endsWith('/') ? g_currentPath + f.name : g_currentPath + '/' + f.name;
        var nameClick = f.is_dir ? ' onclick="navigateTo(\'' + filePath + '\')"' : '';
        var size = f.is_dir ? '--' : formatSize(f.size);
        html += '<tr>';
        html += '<td><span class="' + nameClass + '"' + nameClick + ' style="cursor:pointer">' + f.name + (f.is_dir ? '/' : '') + '</span></td>';
        html += '<td>' + size + '</td>';
        html += '<td>' + formatFileTime(f.mtime) + '</td>';
        html += '<td class="file-actions">';
        if (!f.is_dir) {
            html += '<button class="btn btn-ghost btn-xs" onclick="downloadFile(\'' + filePath + '\')">下载</button>';
        }
        if (!f.protected) {
            html += '<button class="btn btn-danger btn-xs" onclick="deleteFile(\'' + filePath + '\')">删除</button>';
        }
        html += '</td></tr>';
    });
    html += '</tbody></table>';
    container.innerHTML = html;
}

async function refreshFilesWithFlash() {
    await refreshFiles();
    var container = $('fileContainer');
    if (container) flashEl(container);
}

function navigateTo(path) {
    g_currentPath = path;
    refreshFiles();
}

function formatSize(bytes) {
    if (!bytes && bytes !== 0) return '--';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / 1024 / 1024).toFixed(1) + ' MB';
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

function triggerUpload() {
    $('uploadFile').click();
}

async function doUpload() {
    var fileInput = $('uploadFile');
    if (!fileInput.files.length) return;
    toast('上传中...', 'info');
    var fd = new FormData();
    for (var i = 0; i < fileInput.files.length; i++) {
        fd.append('file', fileInput.files[i]);
    }
    try {
        var r = await fetch('/api/files/upload?path=' + encodeURIComponent(g_currentPath), {
            method: 'POST',
            body: fd,
        });
        var data = await r.json();
        if (data.ok) {
            toast('上传成功', 'success');
            refreshFiles();
        } else {
            toast('上传失败', 'error');
        }
    } catch (e) {
        toast('上传失败', 'error');
    }
    fileInput.value = '';
}

function connectPanelSSE() {
    if (g_panelSSE) return;
    try {
        g_panelSSE = new EventSource('/api/panel/logs/stream');
        g_panelSSE.onmessage = function(e) {
            var autoEl = g_mode === 'wired' ? $('autoRefreshPanelWired') : $('autoRefreshPanel');
            if (!autoEl || !autoEl.checked) return;
            var data = JSON.parse(e.data);
            var containerId = g_mode === 'wired' ? 'logPanelWired' : 'logPanel';
            var container = $(containerId);
            if (!container) return;
            var l = stripAnsi(data.text || '');
            var emptyState = container.querySelector('.empty-state');
            if (emptyState) emptyState.remove();
            var div = document.createElement('div');
            div.className = 'log-line';
            div.innerHTML = renderLogLine(data.text || '');
            container.appendChild(div);
            if (container.children.length > 500) {
                container.removeChild(container.firstChild);
            }
            container.scrollTop = container.scrollHeight;
        };
        g_panelSSE.onerror = function() {
            g_panelSSE.close();
            g_panelSSE = null;
            setTimeout(connectPanelSSE, 5000);
        };
    } catch(e) {}
}

function disconnectPanelSSE() {
    if (g_panelSSE) {
        g_panelSSE.close();
        g_panelSSE = null;
    }
}

async function refreshAllLogs() {
    refreshDeviceLogs();
}

function refreshDeviceLogs() {
    if (g_mode === 'wired') return;
    if (!g_connected) return;
    if ($('autoRefreshXwebd') && $('autoRefreshXwebd').checked) {
        refreshLogPanel('xwebd', false);
    }
    if ($('autoRefreshAssistant') && $('autoRefreshAssistant').checked) {
        refreshLogPanel('assistant', false);
    }
}

function getLogContainerId(source) {
    if (source === 'xwebd') return 'logXwebd';
    if (source === 'assistant') return 'logAssistant';
    if (g_mode === 'wired') return 'logPanelWired';
    return 'logPanel';
}

async function refreshLogPanel(source, flash) {
    var containerId = getLogContainerId(source);
    if (source === 'panel') {
        var levelEl = g_mode === 'wired' ? $('logLevelPanelWired') : $('logLevelPanel');
        var level = levelEl ? levelEl.value : '';
        var url = '/api/panel/logs?lines=80';
        if (level) url += '&level=' + level;
        var r = await api(url);
        renderLogPanel(containerId, r, source);
    } else {
        if (!g_connected) return;
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
    var allLineCount = allLines.length;
    var skipCount = 0;
    if (source === 'xwebd') {
        if (g_clearLineXwebd >= 0) {
            skipCount = Math.min(g_clearLineXwebd, allLineCount);
            if (allLineCount > g_clearLineXwebd) {
                g_clearLineXwebd = -1;
            }
        }
        g_lastLogCountXwebd = allLineCount;
    } else if (source === 'assistant') {
        if (g_clearLineAssistant >= 0) {
            skipCount = Math.min(g_clearLineAssistant, allLineCount);
            if (allLineCount > g_clearLineAssistant) {
                g_clearLineAssistant = -1;
            }
        }
        g_lastLogCountAssistant = allLineCount;
    } else if (source === 'panel') {
        if (g_clearLinePanel >= 0) {
            skipCount = Math.min(g_clearLinePanel, allLineCount);
            if (allLineCount > g_clearLinePanel) {
                g_clearLinePanel = -1;
            }
        }
        g_lastLogCountPanel = allLineCount;
    }
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
    if (source === 'xwebd') g_clearLineXwebd = g_lastLogCountXwebd;
    else if (source === 'assistant') g_clearLineAssistant = g_lastLogCountAssistant;
    else if (source === 'panel') {
        g_clearLinePanel = g_lastLogCountPanel;
        disconnectPanelSSE();
        setTimeout(connectPanelSSE, 500);
    }
    toast('已清屏，仅显示新日志', 'success');
}

async function cleanLogPanel(source) {
    if (source === 'panel') {
        var r = await api('/api/panel/logs/clean', { method: 'POST' });
        if (r.ok) {
            g_clearLinePanel = -1;
            g_lastLogCountPanel = 0;
            var container = $('logPanel');
            if (container) container.innerHTML = '<div class="empty-state">日志已清理</div>';
            toast('面板日志已清理', 'success');
        } else {
            toast('清理失败', 'error');
        }
    } else {
        if (!g_connected) { toast('请先连接设备', 'error'); return; }
        var sourceNum = source === 'xwebd' ? '2' : '1';
        var r = await api('/api/logs/clean?source=' + sourceNum, { method: 'POST' });
        if (r.ok) {
            if (source === 'xwebd') { g_clearLineXwebd = -1; g_lastLogCountXwebd = 0; }
            else { g_clearLineAssistant = -1; g_lastLogCountAssistant = 0; }
            var containerId = getLogContainerId(source);
            var container = $(containerId);
            if (container) container.innerHTML = '<div class="empty-state">日志已清理</div>';
            toast((source === 'xwebd' ? '面板内核' : '语音助手') + '日志已清理', 'success');
        } else {
            toast('清理失败', 'error');
        }
    }
}

function renderLogLine(l) {
    var clean = stripAnsi(l);
    var m = clean.match(/^(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})\s+(DEBUG|INFO|WARNING|WARN|ERROR|CRITICAL)\s+(\S+):\s+(.*)$/);
    if (m) {
        var dateHtml = '<span style="color:#6a9955">' + escapeHtml(m[1]) + '</span>';
        var levelColors = {DEBUG:'#569cd6', INFO:'#d4d4d4', WARNING:'#e5c07b', WARN:'#e5c07b', ERROR:'#f44747', CRITICAL:'#ff4cff'};
        var levelColor = levelColors[m[2]] || '#d4d4d4';
        var levelHtml = '<span style="color:' + levelColor + ';font-weight:600">' + escapeHtml(m[2]) + '</span>';
        var sourceHtml = '<span style="color:#dcdcaa">' + escapeHtml(m[3]) + '</span>';
        var msgHtml = '<span style="color:' + levelColor + '">' + escapeHtml(m[4]) + '</span>';
        return dateHtml + ' ' + levelHtml + ' ' + sourceHtml + ': ' + msgHtml;
    }
    var cls = getLogClass(clean);
    var colors = {logInfo:'#d4d4d4', logError:'#f44747', logWarn:'#e5c07b', logDebug:'#569cd6', logCritical:'#ff4cff'};
    return '<span style="color:' + (colors[cls] || '#d4d4d4') + '">' + escapeHtml(clean) + '</span>';
}

function getLogClass(l) {
    if (l.indexOf('[E]') >= 0 || l.indexOf('ERROR') >= 0 || l.indexOf('CRITICAL') >= 0) return 'log-error';
    if (l.indexOf('[W]') >= 0 || l.indexOf('WARNING') >= 0 || l.indexOf('WARN') >= 0) return 'log-warn';
    if (l.indexOf('[D]') >= 0 || l.indexOf('DEBUG') >= 0) return 'log-debug';
    if (l.indexOf('[I]') >= 0 || l.indexOf('INFO') >= 0) return 'log-info';
    return 'log-info';
}

function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function stripAnsi(s) {
    return s.replace(/\x1b\[[0-9;]*m/g, '').replace(/\[[\d;]*m/g, function(m) {
        if (/^\[\d+m$/.test(m)) return '';
        return m;
    });
}

async function doXwebdUpdate() {
    toast('更新xwebd中...', 'info');
    var r = await api('/api/xwebd/upload-update', { method: 'POST' });
    if (r.ok) toast('xwebd 更新成功', 'success');
    else toast('更新失败: ' + (r.error || ''), 'error');
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

async function doDeploy() {
    var progress = $('upgradeProgress');
    var bar = $('upgradeBar');
    var label = $('upgradeLabel');
    progress.style.display = 'block';
    bar.style.width = '0%';
    label.textContent = '部署中...';
    $('btnDeploy').disabled = true;

    bar.style.width = '30%';
    api('/api/assistant/deploy', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: '/var/upgrade/sair_new' }),
    }).then(function(r) {
        if (r.ok) {
            bar.style.width = '100%';
            label.textContent = '部署成功！';
            toast('语音助手部署成功', 'success');
            refreshAssistantStatus();
        } else {
            label.textContent = '部署失败: ' + (r.error || '');
            toast('部署失败', 'error');
        }
        $('btnDeploy').disabled = false;
    });
}

async function doUpdate() {
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

async function runDiag() {
    if (!g_connected) { toast('请先连接设备', 'error'); return; }
    var container = $('diagContainer');
    var btn = $('btnDiag');
    btn.disabled = true;
    btn.textContent = '检测中...';
    container.innerHTML = '<div class="empty-state">正在检测设备环境...</div>';

    var xwebdResult = null;
    var assistantResult = null;

    try {
        var xr = await api('/api/xwebd/diag');
        xwebdResult = xr.items ? xr : (xr.data ? xr.data : null);
    } catch(e) {}

    try {
        var ar = await api('/api/assistant/diag');
        assistantResult = ar.items ? ar : (ar.data ? ar.data : null);
    } catch(e) {}

    var html = '';

    if (xwebdResult) {
        html += renderDiagSection('xwebd 自检', xwebdResult);
    }
    if (assistantResult) {
        html += renderDiagSection('语音助手自检', assistantResult);
    }
    if (!xwebdResult && !assistantResult) {
        html = '<div class="empty-state">自检请求失败，请检查设备连接</div>';
    }

    container.innerHTML = html;
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
    html += '</span>';
    html += '</div>';

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
    html += '</div>';
    html += '</div>';
    return html;
}

function flashEl(el) {
    if (!el) return;
    el.classList.add('flash');
    setTimeout(function() { el.classList.remove('flash'); }, 400);
}

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

    g_logTimer = setInterval(refreshDeviceLogs, 2000);

    window.addEventListener('resize', function() {
        updateViewportHeight();
    });

    $('cfgRealtime').addEventListener('change', function() {
        if (this.checked) {
            $('cfgWsUrl').value = 'wss://api.tenclass.net/xiaozhi/v1/realtime';
        } else {
            $('cfgWsUrl').value = 'wss://api.tenclass.net/xiaozhi/v1/';
        }
    });
});
