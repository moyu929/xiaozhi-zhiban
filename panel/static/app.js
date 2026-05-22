var S = {
    mode: 'wired',
    adb: { serial: null, connected: false },
    wl: { host: '', connected: false, xwebd: false, sair: false },
    currentPath: '/var/upgrade',
    timers: { status: null, adbInfo: null, reboot: null, panelLogPoll: null },
    rebootStart: 0,
    confirmResolve: null,
    panelSSE: null,
    deviceSSE: {},
    mcpTools: [
        { name: 'self.get_device_status', desc: '获取设备实时状态', params: '{"type":"object","properties":{}}' },
        { name: 'self.audio_speaker.set_volume', desc: '设置音量 (0-100)', params: '{"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100}},"required":["volume"]}' },
        { name: 'self.get_system_info', desc: '获取系统信息', params: '{"type":"object","properties":{}}' },
        { name: 'self.clean_junk', desc: '清理临时文件', params: '{"type":"object","properties":{}}' },
        { name: 'self.reboot', desc: '重启设备', params: '{"type":"object","properties":{}}' },
        { name: 'self.poweroff', desc: '关机', params: '{"type":"object","properties":{}}' },
        { name: 'self.get_mcp_tools', desc: '列出所有MCP工具', params: '{"type":"object","properties":{}}' }
    ],
    mcpEditingIdx: -1
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
    if (!S.mcpTools.length) {
        container.innerHTML = '<div class="empty-state" style="padding:12px">暂无MCP工具，点击上方按钮新建</div>';
        return;
    }
    var html = '';
    for (var i = 0; i < S.mcpTools.length; i++) {
        var t = S.mcpTools[i];
        if (S.mcpEditingIdx === i) {
            html += '<div class="mcp-edit-form">';
            html += '<div class="mcp-edit-row">';
            html += '<input class="mcp-edit-name" id="mcpEditName" value="' + escapeHtml(t.name) + '" placeholder="工具名称">';
            html += '<input class="mcp-edit-desc" id="mcpEditDesc" value="' + escapeHtml(t.desc) + '" placeholder="工具描述">';
            html += '</div>';
            html += '<textarea class="mcp-edit-params" id="mcpEditParams" placeholder="接口参数 (JSON Schema)">' + escapeHtml(t.params || '') + '</textarea>';
            html += '<div class="mcp-edit-btns">';
            html += '<button class="btn btn-accent btn-xs" onclick="mcpToolSaveEdit(' + i + ')">保存</button>';
            html += '<button class="btn btn-ghost btn-xs" onclick="mcpToolCancelEdit()">取消</button>';
            html += '</div>';
            html += '</div>';
        } else {
            html += '<div class="mcp-tool-item">';
            html += '<span class="mcp-tool-name">' + escapeHtml(t.name) + '</span>';
            html += '<span class="mcp-tool-desc">' + escapeHtml(t.desc) + '</span>';
            html += '<span class="mcp-tool-actions">';
            html += '<button class="mcp-tool-btn-edit" onclick="mcpToolEdit(' + i + ')">编辑</button>';
            html += '<button class="mcp-tool-btn-del" onclick="mcpToolDel(' + i + ')">删除</button>';
            html += '</span>';
            html += '</div>';
        }
    }
    container.innerHTML = html;
}

function mcpToolAdd() {
    S.mcpEditingIdx = S.mcpTools.length;
    S.mcpTools.push({ name: '', desc: '', params: '' });
    renderMcpTools();
    var nameInput = $('mcpEditName');
    if (nameInput) nameInput.focus();
}

function mcpToolEdit(idx) {
    S.mcpEditingIdx = idx;
    renderMcpTools();
    var nameInput = $('mcpEditName');
    if (nameInput) nameInput.focus();
}

function mcpToolSaveEdit(idx) {
    var name = $('mcpEditName').value.trim();
    var desc = $('mcpEditDesc').value.trim();
    var params = $('mcpEditParams').value.trim();
    if (!name) { toast('工具名称不能为空', 'error'); return; }
    if (params) {
        try { JSON.parse(params); } catch(e) { toast('接口参数必须是有效的JSON', 'error'); return; }
    }
    S.mcpTools[idx] = { name: name, desc: desc, params: params };
    S.mcpEditingIdx = -1;
    renderMcpTools();
    toast('工具已保存', 'success');
}

function mcpToolCancelEdit() {
    if (S.mcpEditingIdx >= 0 && S.mcpTools[S.mcpEditingIdx].name === '') {
        S.mcpTools.splice(S.mcpEditingIdx, 1);
    }
    S.mcpEditingIdx = -1;
    renderMcpTools();
}

async function mcpToolDel(idx) {
    var name = S.mcpTools[idx].name || '该工具';
    if (!await showConfirm('确定删除工具 ' + name + '？', {danger: true})) return;
    S.mcpTools.splice(idx, 1);
    S.mcpEditingIdx = -1;
    renderMcpTools();
    toast('工具已删除', 'success');
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
    $('svcList').innerHTML = '<div class="empty-state">等待连接设备...</div>';
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
    S.mcpTools = [
        { name: 'self.get_device_status', desc: '获取设备实时状态', params: '{"type":"object","properties":{}}' },
        { name: 'self.audio_speaker.set_volume', desc: '设置音量 (0-100)', params: '{"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100}},"required":["volume"]}' },
        { name: 'self.get_system_info', desc: '获取系统信息', params: '{"type":"object","properties":{}}' },
        { name: 'self.clean_junk', desc: '清理临时文件', params: '{"type":"object","properties":{}}' },
        { name: 'self.reboot', desc: '重启设备', params: '{"type":"object","properties":{}}' },
        { name: 'self.poweroff', desc: '关机', params: '{"type":"object","properties":{}}' },
        { name: 'self.get_mcp_tools', desc: '列出所有MCP工具', params: '{"type":"object","properties":{}}' }
    ];
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
    refreshServices();
    refreshProcesses();
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
        $('devCpuUsage').textContent = (d.cpu_usage >= 0) ? d.cpu_usage + '%' : '--';
        var wifiOk = d.wifi_connected;
        if (S.wl.connected && !wifiOk && d.wifi_ip) wifiOk = true;
        $('devWifi').textContent = wifiOk ? '已连接' : '未连接';
        $('devBattery').textContent = d.battery_cap != null ? (d.battery_cap >= 0 && d.battery_cap <= 100 ? d.battery_cap + '%' : 'USB供电') : '--';
        $('devUptime').textContent = formatUptime(d.uptime_s);
        $('devMem').textContent = formatMem(d.mem_free_kb, d.mem_total_kb, d.mem_cached_kb);
        $('devDisk').textContent = formatDisk(d.disk_used_kb, d.disk_total_kb);
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

    var items = [
        { name: '面板内核', key: 'xwebd_running', status: d.xwebd && d.xwebd.running, toggle: false,
          statusText: d.xwebd && d.xwebd.running ? '运行中' : '已停止' },
        { name: '面板内核自启动', key: 'autostart', status: d.xwebd && d.xwebd.autostart, toggle: true, service: 'autostart',
          statusText: d.xwebd && d.xwebd.autostart ? '已启用' : '未启用', hint: '重启保留' },
        { name: '语音助手', key: 'sair', status: d.sair && d.sair.running, toggle: false,
          statusText: d.sair ? (d.sair.running ? '运行中' : (d.sair.installed ? '已停止' : '未安装')) : '未知' },
        { name: '看门狗脚本', key: 'watchdog', status: d.boot_watchdog && (d.boot_watchdog.deployed || d.boot_watchdog.running), toggle: true, service: 'boot_watchdog',
          statusText: d.boot_watchdog ? (d.boot_watchdog.running ? '运行中' : (d.boot_watchdog.deployed ? '已部署' : '未部署')) : '未知', hint: '重启保留' },
        { name: 'Telnet 终端', key: 'telnet', status: d.telnet && d.telnet.running, toggle: true, service: 'telnet',
          statusText: d.telnet && d.telnet.running ? '运行中' : '已停止', hint: '重启保留' },
        { name: 'USB存储卡', key: 'usb_lun', status: d.usb_lun && d.usb_lun.enabled, toggle: true, service: 'usb_lun',
          statusText: d.usb_lun && d.usb_lun.enabled ? '已开启' : '已关闭', hint: '重启保留' },
        { name: '呼吸灯', key: 'led', status: d.led && d.led.enabled, toggle: true, service: 'led',
          statusText: d.led && d.led.enabled ? '已开启' : '已关闭', hint: '重启保留' },
        { name: '按键背光', key: 'key_backlight', status: d.key_backlight && d.key_backlight.enabled === true, toggle: true, service: 'key_backlight',
          statusText: d.key_backlight ? (d.key_backlight.enabled === true ? '已开启' : (d.key_backlight.enabled === false ? '已关闭' : '不支持')) : '未知', hint: '重启保留' },
        { name: '音频预缓存', key: 'audio_precache', status: d.audio_precache && d.audio_precache.enabled, toggle: true, service: 'audio_precache',
          statusText: d.audio_precache && d.audio_precache.enabled ? '已开启' : '已关闭', hint: '重启保留' }
    ];

    var html = '';
    items.forEach(function(item) {
        var statusCls = item.status ? 'svc-on' : 'svc-off';
        html += '<div class="svc-item">';
        html += '<span class="svc-name">' + item.name + '</span>';
        if (item.toggle) {
            html += '<label class="svc-toggle">';
            html += '<input type="checkbox"' + (item.status ? ' checked' : '') + ' onchange="toggleService(\'' + item.service + '\', this.checked)">';
            html += '<span class="svc-toggle-track"><span class="svc-toggle-thumb"></span></span>';
            html += '</label>';
            if (item.hint) html += '<span class="svc-hint">' + item.hint + '</span>';
        } else {
            html += '<span class="svc-status ' + statusCls + '">' + item.statusText + '</span>';
        }
        html += '</div>';
    });
    $('svcList').innerHTML = html;
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
        await new Promise(function(resolve) { setTimeout(resolve, 500); });
        await refreshServices();
    } else {
        toast('操作失败: ' + (r.error || ''), 'error');
        await refreshServices();
    }
}

async function refreshServicesWithFlash() {
    await refreshServices();
    var svcList = document.querySelector('.svc-list');
    if (svcList) flashEl(svcList);
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
        $('processContainer').innerHTML = '<div class="empty-state">获取进程列表失败</div>';
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
        if (r.mcp_endpoint) $('cfgMcpEndpoint').value = r.mcp_endpoint;
        if (r.listening_mode) $('cfgListeningMode').value = r.listening_mode;
    }
    var r2 = await api('/api/xwebd/config');
    if (!r2.error) {
        if (r2.log_level) $('cfgLogLevel').value = r2.log_level;
        $('cfgUploadMax').value = r2.upload_max_mb != null ? r2.upload_max_mb : 10;
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
    var listeningMode = $('cfgListeningMode').value;
    if (listeningMode) config.listening_mode = listeningMode;
    var listenTimeout = parseInt($('cfgListenTimeout').value);
    var sessionTimeout = parseInt($('cfgSessionTimeout').value);
    var wakeupCooldown = parseInt($('cfgWakeupCooldown').value);
    var wsPingInterval = parseInt($('cfgWsPingInterval').value);
    if (listenTimeout > 0) config.listen_timeout = listenTimeout * 1000;
    if (sessionTimeout > 0) config.session_timeout = sessionTimeout * 1000;
    if (wakeupCooldown > 0) config.wakeup_cooldown = wakeupCooldown * 1000;
    if (wsPingInterval > 0) config.ws_ping_interval = wsPingInterval * 1000;
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
    $('cfgMcpEndpoint').value = '';
    $('cfgSairLogLevel').value = 'INFO';
    $('cfgListeningMode').value = 'autostop';
    $('cfgListenTimeout').value = '120';
    $('cfgSessionTimeout').value = '300';
    $('cfgWakeupCooldown').value = '3';
    $('cfgWsPingInterval').value = '25';
    var r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            mcp_endpoint: '',
            log_level: 'INFO',
            listening_mode: 'autostop',
            listen_timeout: 120000,
            session_timeout: 300000,
            wakeup_cooldown: 3000,
            ws_ping_interval: 25000
        }),
    });
    if (r.ok || !r.error) toast('助手配置已恢复默认值', 'success');
    else toast('恢复默认值失败', 'error');
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
function hideAssistantOverlay() {
    var el = $('assistantOverlay');
    if (el) el.style.display = 'none';
}

async function doHotUpdate() {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
    if (statusR.error) {
        toast('无法获取助手状态', 'error');
        return;
    }
    if (!sd.installed) {
        if (sd.native_running) {
            toast('当前运行的是原生语音助手，不支持热更新，请使用「部署」功能', 'error');
        } else {
            toast('语音助手未安装，请先点击「部署」按钮', 'error');
        }
        return;
    }
    if (!sd.running) {
        toast('语音助手未运行，请使用「冷更新」功能', 'error');
        return;
    }
    var oldPid = sd.pid;
    var oldVersion = sd.version || '';
    if (!await showConfirm('确定进行热更新？\n\n热更新不会重启设备，助手进程将自动替换为新版本')) return;
    toast('正在执行热更新...', 'info');
    var r = await api('/api/assistant/upgrade', { method: 'POST' });
    if (!r.ok && r.error) {
        toast('热更新失败: ' + r.error, 'error');
        return;
    }
    toast('热更新指令已发送，等待助手重启...', 'info');
    showAssistantOverlay(60);
    var maxAttempts = 15;
    var attempt = 0;
    var success = false;
    while (attempt < maxAttempts) {
        await new Promise(function(resolve) { setTimeout(resolve, 2000); });
        attempt++;
        updateAssistantOverlayCountdown(Math.max(0, 60 - attempt * 2));
        try {
            var sr = await api('/api/assistant/status');
            var sd2 = sr.data || sr;
            if (sr.error) continue;
            if (sd2.running && (sd2.pid !== oldPid || (sd2.version && sd2.version !== oldVersion))) {
                success = true;
                var newVer = sd2.version || '';
                if (newVer && newVer !== oldVersion) {
                    toast('热更新成功！版本 ' + oldVersion + ' → ' + newVer, 'success');
                } else {
                    toast('热更新成功！助手已重启', 'success');
                }
                refreshAssistantStatus();
                break;
            }
        } catch(e) {}
    }
    hideAssistantOverlay();
    if (!success) {
        toast('热更新超时，请检查助手状态', 'error');
        refreshAssistantStatus();
    }
}

async function doDeploy() {
    var statusR = await api('/api/assistant/status');
    var sd = statusR.data || statusR;
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
    if (r.ok) {
        if (r.rebooting) {
            label.textContent = '部署成功！设备重启中...';
            toast('语音助手部署成功，设备重启中...', 'success');
            setTimeout(function() { progress.style.display = 'none'; }, 1500);
            wirelessWaitForReconnect();
        } else {
            label.textContent = '部署成功！';
            toast('语音助手部署成功', 'success');
            setTimeout(function() { progress.style.display = 'none'; refreshAssistantStatus(); }, 1500);
        }
    } else {
        label.textContent = '部署失败: ' + (r.error || '');
        toast('部署失败: ' + (r.error || ''), 'error');
    }
}

async function doUpdate() {
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
    if (!await showConfirm('确定进行冷更新？\n\n冷更新将替换助手程序并重启设备')) return;
    toast('冷更新中...', 'info');
    var r = await api('/api/assistant/update', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({}),
    });
    if (r.ok) {
        if (r.rebooting) {
            toast('冷更新成功，设备重启中...', 'success');
            wirelessRebootAndReconnect();
        } else {
            toast('冷更新指令已发送', 'success');
            refreshAssistantStatus();
        }
    } else {
        toast('冷更新失败: ' + (r.error || ''), 'error');
    }
}

async function doUninstall() {
    if (!await showConfirm('确定卸载语音助手？设备将停止语音助手功能', {danger: true})) return;
    toast('卸载中...', 'info');
    var r = await api('/api/assistant/uninstall', { method: 'POST' });
    if (r.ok) {
        if (r.rebooting) {
            toast('语音助手已卸载，设备重启中...', 'success');
            wirelessRebootAndReconnect();
        } else {
            toast('语音助手已卸载', 'success');
            refreshAssistantStatus();
        }
    }
    else toast('卸载失败: ' + (r.error || ''), 'error');
}

async function doXwebdUpdate() {
    toast('更新xwebd中...', 'info');
    if (S.wl.connected) {
        var r = await api('/api/xwebd/wireless-update', { method: 'POST' });
        if (r.ok) {
            if (r.rebooting) {
                toast('xwebd 更新成功，设备重启中...', 'success');
                wirelessRebootAndReconnect();
            } else {
                toast('xwebd 更新成功', 'success');
                refreshXwebdStatus();
            }
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
    setTimeout(function() {
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
                updateConnUI(true);
                $('btnConnect').textContent = '断开';
                $('btnConnect').className = 'btn btn-danger';
                $('btnConnect').disabled = false;
                startPolling();
                refreshAll();
                updateConnStatus('xwebdConnStatus', true);
                hideWirelessOverlays();
                toast('设备已重新连接', 'success');
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
        var mode = this.value;
        var label = mode === 'realtime' ? 'Realtime（实时模式）' : 'AutoStop（自动停止）';
        toast('监听模式已切换为 ' + label + '，点击「保存配置」生效', 'info');
    });

    connectPanelSSE();
    refreshLogPanel('panel', false);
    window.addEventListener('resize', updateViewportHeight);
});
