let g_deviceHost = '';
let g_connected = false;
let g_mock = false;
let g_volume = 20;
let g_brightness = 80;
let g_muted = false;
let g_currentPath = '/var/upgrade';
let g_logTimer = null;
let g_statusTimer = null;

function $(id) { return document.getElementById(id); }

(function initParticles() {
    const canvas = document.getElementById('particles');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    const particles = [];
    for (let i = 0; i < 120; i++) {
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
        const t = Date.now() * 0.001;
        particles.forEach(p => {
            const a = p.alpha * (0.3 + 0.7 * Math.sin(t * p.speed * 5 + p.phase));
            ctx.beginPath();
            ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
            ctx.fillStyle = `rgba(50, 240, 140, ${a})`;
            ctx.fill();
        });
        requestAnimationFrame(animate);
    }
    animate();
    window.addEventListener('resize', () => {
        canvas.width = window.innerWidth;
        canvas.height = window.innerHeight;
    });
})();

function showHelp() { $('helpModal').style.display = 'flex'; }
function closeHelp() { $('helpModal').style.display = 'none'; }

function toast(msg, type) {
    type = type || 'info';
    const el = $('toast');
    el.textContent = msg;
    el.className = 'toast ' + type;
    el.style.display = 'block';
    clearTimeout(el._t);
    el._t = setTimeout(() => { el.style.display = 'none'; }, 3000);
}

async function api(path, opts) {
    opts = opts || {};
    try {
        const r = await fetch(path, {
            method: opts.method || 'GET',
            headers: opts.headers || {},
            body: opts.body || undefined,
        });
        const data = await r.json();
        return data;
    } catch (e) {
        return { ok: false, error: e.message };
    }
}

async function connectDevice() {
    const host = $('deviceHost').value.trim();
    if (!host) { toast('请输入设备地址', 'error'); return; }
    g_deviceHost = host;
    const btn = $('btnConnect');
    btn.disabled = true;
    btn.textContent = '连接中...';
    const r = await api('/api/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ host: host }),
    });
    btn.disabled = false;
    btn.textContent = '连接';
    if (r.ok) {
        g_connected = true;
        g_mock = r.mock || false;
        updateConnUI(true);
        toast('已连接 ' + host, 'success');
        startPolling();
        refreshAll();
    } else {
        toast('连接失败: ' + (r.error || '未知错误'), 'error');
    }
}

function updateConnUI(online) {
    const pill = $('connStatus');
    if (online) {
        pill.className = 'status-pill online';
        pill.innerHTML = '<span class="status-dot online"></span>在线';
    } else {
        pill.className = 'status-pill offline';
        pill.innerHTML = '<span class="status-dot offline"></span>离线';
    }
    $('mockBadge').style.display = g_mock ? 'inline' : 'none';
}

function startPolling() {
    stopPolling();
    g_statusTimer = setInterval(refreshStatus, 3000);
    if ($('autoRefresh') && $('autoRefresh').checked) {
        g_logTimer = setInterval(refreshLogs, 2000);
    }
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
    refreshLogs();
    refreshConfig();
}

function formatUptime(seconds) {
    if (!seconds && seconds !== 0) return '--';
    const d = Math.floor(seconds / 86400);
    const h = Math.floor((seconds % 86400) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return d + '天' + h + '时';
    if (h > 0) return h + '时' + m + '分';
    return m + '分钟';
}

function formatMem(freeKb, totalKb, cachedKb) {
    if (!totalKb) return '--';
    const usedMb = Math.round((totalKb - freeKb - (cachedKb || 0)) / 1024);
    const totalMb = Math.round(totalKb / 1024);
    return usedMb + '/' + totalMb + ' MB';
}

function formatDisk(usedKb, totalKb) {
    if (!totalKb) return '--';
    const usedMb = Math.round(usedKb / 1024);
    const totalMb = Math.round(totalKb / 1024);
    return usedMb + '/' + totalMb + ' MB';
}

async function refreshStatus() {
    if (!g_connected) return;
    const r = await api('/api/status');
    if (!r.ok) return;
    const d = r.data || r;
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
        $('volumeSlider').value = d.volume;
        $('volumeVal').textContent = d.volume;
    }
    if (d.brightness != null) {
        g_brightness = d.brightness;
        $('brightnessSlider').value = d.brightness;
        $('brightnessVal').textContent = d.brightness;
    }
    if (d.muted != null) {
        g_muted = d.muted;
        updateMuteUI();
    }
    if (d.state) {
        const stateEl = $('devState');
        stateEl.textContent = d.state;
        stateEl.className = 'status-pill state-' + d.state.toLowerCase();
    }
    $('footerInfo').textContent = (d.model || '?') + ' · ' + (d.wifi_ip || '?');
}

async function refreshAssistantStatus() {
    if (!g_connected) return;
    const r = await api('/api/assistant/status');
    if (!r.ok) return;
    const d = r.data || r;
    const installed = d.installed;
    const running = d.running;
    let label = '未安装';
    if (installed && running) label = '运行中';
    else if (installed) label = '已安装 (未运行)';
    $('assistantInstalled').textContent = label;
    $('assistantVersion').textContent = d.version || '--';
    $('btnDeploy').disabled = installed;
    $('btnUpdate').disabled = !installed;
}

async function refreshXwebdStatus() {
    if (!g_connected) return;
    try {
        const vr = await api('/api/xwebd/version');
        if (vr.ok || vr.version) {
            $('xwebdVersion').textContent = vr.version ? 'v' + vr.version : '--';
        }
    } catch(e) {}
    try {
        const sr = await api('/api/services');
        if (sr.ok || sr.telnet) {
            $('xwebdStatus').textContent = '运行中';
        }
    } catch(e) {}
}

async function refreshServices() {
    if (!g_connected) return;
    const r = await api('/api/services');
    if (!r.ok) return;
    const d = r.data || r;
    setSvcStatus('svcTelnet', d.telnet && d.telnet.running, d.telnet ? (d.telnet.running ? '运行中' : '已停止') : '未知');
    setSvcStatus('svcWatchdog', d.boot_watchdog && d.boot_watchdog.deployed, d.boot_watchdog ? (d.boot_watchdog.deployed ? '已部署' : '未部署') : '未知');
    setSvcStatus('svcAutostart', d.xwebd_autostart && d.xwebd_autostart.enabled, d.xwebd_autostart ? (d.xwebd_autostart.enabled ? '已启用' : '已禁用') : '未知');

    const ar = await api('/api/assistant/status');
    if (ar.ok) {
        const ad = ar.data || ar;
        setSvcStatus('svcAssistant', ad.running, ad.running ? '运行中 (PID ' + (ad.pid || '?') + ')' : (ad.installed ? '已停止' : '未安装'));
        setSvcStatus('svcBackup', ad.native_backup_exists, ad.native_backup_exists ? '存在' : '无');
    }
}

function setSvcStatus(elId, ok, text) {
    const el = $(elId);
    if (!el) return;
    el.textContent = text;
    el.className = 'svc-status ' + (ok ? 'svc-on' : 'svc-off');
}

async function refreshConfig() {
    if (!g_connected) return;
    const r = await api('/api/config');
    if (!r.ok) return;
    const d = r.data || r;
    if (d.ws_url) $('cfgWsUrl').value = d.ws_url;
    if (d.realtime_mode != null) $('cfgRealtime').checked = d.realtime_mode;
    if (d.aec_enabled != null) $('cfgAec').checked = d.aec_enabled;
    if (d.wake_word) $('cfgWakeWord').value = d.wake_word;
    if (d.wake_threshold != null) {
        $('cfgWakeThreshold').value = d.wake_threshold;
        $('wakeThresholdVal').textContent = d.wake_threshold;
    }
    if (d.log_level) $('cfgLogLevel').value = d.log_level;
    if (d.upload_max_mb) $('cfgUploadMax').value = d.upload_max_mb;
}

async function onVolumeChange(val) {
    g_volume = parseInt(val);
    $('volumeVal').textContent = val;
    await api('/api/volume', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ volume: g_volume }),
    });
}

async function onBrightnessChange(val) {
    g_brightness = parseInt(val);
    $('brightnessVal').textContent = val;
    await api('/api/brightness', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ brightness: g_brightness }),
    });
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
    const btn = $('btnMute');
    const iconOn = $('iconUnmuted');
    const iconOff = $('iconMuted');
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
    const r = await api('/api/wakeup', { method: 'POST' });
    if (r.ok) toast('唤醒指令已发送', 'success');
    else toast('唤醒失败: ' + (r.error || ''), 'error');
}

async function doAbort() {
    const r = await api('/api/abort', { method: 'POST' });
    if (r.ok) toast('已中止对话', 'success');
    else toast('中止失败: ' + (r.error || ''), 'error');
}

async function doReboot() {
    if (!confirm('确定要重启设备吗？')) return;
    toast('正在重启...', 'info');
    await api('/api/reboot', { method: 'POST' });
}

async function doPoweroff() {
    if (!confirm('确定要关机吗？设备将完全断电')) return;
    toast('正在关机...', 'info');
    await api('/api/poweroff', { method: 'POST' });
}

async function saveAssistantConfig() {
    const cfg = {
        ws_url: $('cfgWsUrl').value,
        realtime_mode: $('cfgRealtime').checked,
        aec_enabled: $('cfgAec').checked,
        wake_word: $('cfgWakeWord').value,
        wake_threshold: parseInt($('cfgWakeThreshold').value) || 50,
    };
    const r = await api('/api/assistant/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg),
    });
    if (r.ok) toast('Assistant 配置已保存', 'success');
    else toast('保存失败: ' + (r.error || ''), 'error');
}

async function saveXwebdConfig() {
    const cfg = {
        log_level: $('cfgLogLevel').value,
        upload_max_mb: parseInt($('cfgUploadMax').value) || 20,
    };
    const r = await api('/api/xwebd/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg),
    });
    if (r.ok) toast('xwebd 配置已保存', 'success');
    else toast('保存失败: ' + (r.error || ''), 'error');
}

async function doOtaUpgrade() {
    if (!confirm('确定进行OTA在线升级？')) return;
    toast('正在检查更新...', 'info');
    const r = await api('/api/upgrade', { method: 'POST' });
    if (r.ok) toast('OTA升级指令已发送', 'success');
    else toast('OTA升级失败: ' + (r.error || ''), 'error');
}

async function refreshFiles() {
    if (!g_connected) return;
    const r = await api('/api/files?path=' + encodeURIComponent(g_currentPath));
    if (!r.ok) return;
    const d = r.data || r;
    const files = d.files || [];
    const container = $('fileContainer');
    if (!files.length) {
        container.innerHTML = '<div class="empty-state">空目录</div>';
        return;
    }
    const parts = g_currentPath.split('/').filter(Boolean);
    let navHtml = '<span class="file-nav-link" onclick="navigateTo(\'/\')">/</span>';
    let cumPath = '';
    parts.forEach((p, i) => {
        cumPath += '/' + p;
        navHtml += '<span class="file-nav-sep">/</span>';
        if (i < parts.length - 1) {
            navHtml += '<span class="file-nav-link" onclick="navigateTo(\'' + cumPath + '\')">' + p + '</span>';
        } else {
            navHtml += '<span>' + p + '</span>';
        }
    });

    let html = '<div class="file-nav">' + navHtml + '</div>';
    html += '<table class="file-table"><thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead><tbody>';
    files.forEach(f => {
        const nameClass = f.is_dir ? 'file-dir-link' : (f.protected ? 'file-name-cell file-protected' : 'file-name-cell');
        const nameClick = f.is_dir ? ' onclick="navigateTo(\'' + (g_currentPath === '/' ? '' : g_currentPath) + '/' + f.name + '\')"' : '';
        const size = f.is_dir ? '--' : formatSize(f.size);
        html += '<tr>';
        html += '<td><span class="' + nameClass + '"' + nameClick + ' style="cursor:pointer">' + f.name + (f.is_dir ? '/' : '') + '</span></td>';
        html += '<td>' + size + '</td>';
        html += '<td>' + (f.mtime || '--') + '</td>';
        html += '<td class="file-actions">';
        if (!f.is_dir) {
            html += '<button class="btn btn-ghost btn-xs" onclick="downloadFile(\'' + f.path + '\')">下载</button>';
            if (!f.protected) {
                html += '<button class="btn btn-danger btn-xs" onclick="deleteFile(\'' + f.path + '\')">删除</button>';
            }
        }
        html += '</td></tr>';
    });
    html += '</tbody></table>';
    container.innerHTML = html;
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
    if (!confirm('确定删除 ' + path + '？')) return;
    const r = await api('/api/files?path=' + encodeURIComponent(path), { method: 'DELETE' });
    if (r.ok) { toast('已删除', 'success'); refreshFiles(); }
    else toast('删除失败: ' + (r.error || ''), 'error');
}

async function doCleanup() {
    if (!confirm('确定清理垃圾文件？')) return;
    toast('清理中...', 'info');
    const r = await api('/api/files/cleanup', { method: 'POST' });
    if (r.ok) { toast('清理完成', 'success'); refreshFiles(); }
    else toast('清理失败: ' + (r.error || ''), 'error');
}

function triggerUpload() {
    $('uploadFile').click();
}

async function doUpload() {
    const fileInput = $('uploadFile');
    if (!fileInput.files.length) return;
    toast('上传中...', 'info');
    const fd = new FormData();
    for (let i = 0; i < fileInput.files.length; i++) {
        fd.append('file', fileInput.files[i]);
    }
    try {
        const r = await fetch('/api/files/upload?path=' + encodeURIComponent(g_currentPath), {
            method: 'POST',
            body: fd,
        });
        const data = await r.json();
        if (data.ok) {
            toast('上传成功', 'success');
            refreshFiles();
        } else {
            toast('上传失败: ' + (data.error || ''), 'error');
        }
    } catch (e) {
        toast('上传失败: ' + e.message, 'error');
    }
    fileInput.value = '';
}

async function refreshLogs() {
    if (!g_connected) return;
    const level = $('logLevelFilter') ? $('logLevelFilter').value : '';
    const source = $('logSourceFilter') ? $('logSourceFilter').value : '';
    let url = '/api/logs?lines=80';
    if (level) url += '&level=' + level;
    if (source) url += '&source=' + source;
    const r = await api(url);
    if (!r.ok) return;
    const d = r.data || r;
    const lines = d.lines || d.logs || [];
    const container = $('logContainer');
    if (!lines.length) {
        container.innerHTML = '<div class="empty-state">暂无日志</div>';
        return;
    }
    let html = '';
    lines.forEach(l => {
        let cls = 'log-info';
        if (typeof l === 'string') {
            if (l.indexOf('[E]') >= 0 || l.indexOf('ERROR') >= 0) cls = 'log-error';
            else if (l.indexOf('[W]') >= 0 || l.indexOf('WARN') >= 0) cls = 'log-warn';
            else if (l.indexOf('[D]') >= 0 || l.indexOf('DEBUG') >= 0) cls = 'log-debug';
            html += '<div class="log-line ' + cls + '">' + escapeHtml(l) + '</div>';
        } else {
            if (l.level === 'ERROR' || l.level === 'E') cls = 'log-error';
            else if (l.level === 'WARN' || l.level === 'W') cls = 'log-warn';
            else if (l.level === 'DEBUG' || l.level === 'D') cls = 'log-debug';
            const src = l.source ? '<span class="log-source">[' + l.source + ']</span> ' : '';
            html += '<div class="log-line ' + cls + '">' + src + escapeHtml(l.text || l.message || '') + '</div>';
        }
    });
    container.innerHTML = html;
    container.scrollTop = container.scrollHeight;
}

function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

async function deployXwebd() {
    const banner = $('deployBanner');
    const progress = $('deployProgress');
    const bar = $('deployBar');
    const label = $('deployLabel');
    banner.querySelector('button').disabled = true;
    progress.style.display = 'block';
    bar.style.width = '30%';
    label.textContent = '正在部署 xwebd...';
    const r = await api('/api/deploy/xwebd', { method: 'POST' });
    bar.style.width = '100%';
    if (r.ok) {
        label.textContent = '部署完成！';
        toast('xwebd 部署成功', 'success');
        setTimeout(() => { banner.style.display = 'none'; }, 2000);
        refreshAll();
    } else {
        label.textContent = '部署失败: ' + (r.error || '');
        toast('部署失败: ' + (r.error || ''), 'error');
    }
    banner.querySelector('button').disabled = false;
}

async function doXwebdUpdate() {
    const fileInput = $('xwebdFile');
    if (!fileInput.files.length) { toast('请选择xwebd文件', 'error'); return; }
    toast('上传中...', 'info');
    const fd = new FormData();
    fd.append('file', fileInput.files[0]);
    try {
        const r = await fetch('/api/xwebd/update', { method: 'POST', body: fd });
        const data = await r.json();
        if (data.ok) toast('xwebd 更新成功', 'success');
        else toast('更新失败: ' + (data.error || ''), 'error');
    } catch (e) {
        toast('上传失败: ' + e.message, 'error');
    }
}

async function doXwebdRestart() {
    toast('重启 xwebd...', 'info');
    const r = await api('/api/xwebd/restart', { method: 'POST' });
    if (r.ok) toast('xwebd 已重启', 'success');
    else toast('重启失败: ' + (r.error || ''), 'error');
}

async function doXwebdRemove() {
    if (!confirm('确定卸载 xwebd？')) return;
    const r = await api('/api/xwebd/remove', { method: 'POST' });
    if (r.ok) toast('xwebd 已卸载', 'success');
    else toast('卸载失败: ' + (r.error || ''), 'error');
}

async function doDeploy() {
    const fileInput = $('firmwareFile');
    if (!fileInput.files.length) { toast('请选择sair固件文件', 'error'); return; }
    const progress = $('upgradeProgress');
    const bar = $('upgradeBar');
    const label = $('upgradeLabel');
    progress.style.display = 'block';
    bar.style.width = '0%';
    label.textContent = '上传中... 0%';
    $('btnDeploy').disabled = true;

    const fd = new FormData();
    fd.append('file', fileInput.files[0]);
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/assistant/deploy');
        xhr.upload.onprogress = function(e) {
            if (e.lengthComputable) {
                const pct = Math.round(e.loaded / e.total * 100);
                bar.style.width = pct + '%';
                label.textContent = '上传中... ' + pct + '%';
            }
        };
        xhr.onload = function() {
            let data;
            try { data = JSON.parse(xhr.responseText); } catch(e) { data = { ok: false, error: 'Invalid response' }; }
            if (data.ok) {
                bar.style.width = '100%';
                label.textContent = '部署成功！';
                toast('Assistant 部署成功', 'success');
                refreshAssistantStatus();
            } else {
                label.textContent = '部署失败: ' + (data.error || '');
                toast('部署失败: ' + (data.error || ''), 'error');
            }
            $('btnDeploy').disabled = false;
        };
        xhr.onerror = function() {
            label.textContent = '上传失败';
            toast('上传失败', 'error');
            $('btnDeploy').disabled = false;
        };
        xhr.send(fd);
    } catch (e) {
        toast('部署失败: ' + e.message, 'error');
        $('btnDeploy').disabled = false;
    }
}

async function doUpdate() {
    const fileInput = $('firmwareFile');
    if (!fileInput.files.length) { toast('请选择sair固件文件', 'error'); return; }
    toast('上传更新中...', 'info');
    const fd = new FormData();
    fd.append('file', fileInput.files[0]);
    try {
        const r = await fetch('/api/assistant/update', { method: 'POST', body: fd });
        const data = await r.json();
        if (data.ok) { toast('更新成功', 'success'); refreshAssistantStatus(); }
        else toast('更新失败: ' + (data.error || ''), 'error');
    } catch (e) {
        toast('更新失败: ' + e.message, 'error');
    }
}

async function doUninstall() {
    if (!confirm('确定卸载 Assistant？设备将停止语音助手功能')) return;
    toast('卸载中...', 'info');
    const r = await api('/api/assistant/uninstall', { method: 'POST' });
    if (r.ok) { toast('Assistant 已卸载', 'success'); refreshAssistantStatus(); }
    else toast('卸载失败: ' + (r.error || ''), 'error');
}

document.addEventListener('DOMContentLoaded', function() {
    $('xwebdFile').addEventListener('change', function() {
        $('xwebdFileName').textContent = this.files.length ? this.files[0].name : '未选择文件';
        $('btnXwebdUpdate').disabled = !this.files.length;
    });
    $('firmwareFile').addEventListener('change', function() {
        $('firmwareName').textContent = this.files.length ? this.files[0].name : '未选择文件';
        $('btnDeploy').disabled = false;
        $('btnUpdate').disabled = false;
    });
    $('autoRefresh').addEventListener('change', function() {
        if (this.checked && g_connected) {
            g_logTimer = setInterval(refreshLogs, 2000);
        } else {
            if (g_logTimer) { clearInterval(g_logTimer); g_logTimer = null; }
        }
    });
});

async function doClearLogs() {
    if (!confirm('确定清空设备日志？此操作不可恢复')) return;
    const r = await api('/api/assistant/logs/clear', { method: 'POST' });
    if (r.ok) {
        $('logContainer').innerHTML = '<div class="empty-state">日志已清空</div>';
        toast('日志已清空', 'success');
    } else {
        toast('清空失败: ' + (r.error || ''), 'error');
    }
}

async function runDiag() {
    if (!g_connected) { toast('请先连接设备', 'error'); return; }
    const container = $('diagContainer');
    const btn = $('btnDiag');
    btn.disabled = true;
    btn.textContent = '检测中...';
    container.innerHTML = '<div class="empty-state">正在检测设备环境...</div>';

    let xwebdResult = null;
    let assistantResult = null;

    try {
        const xr = await api('/api/xwebd/diag');
        xwebdResult = xr.items ? xr : (xr.data ? xr.data : null);
    } catch(e) {}

    try {
        const ar = await api('/api/assistant/diag');
        assistantResult = ar.items ? ar : (ar.data ? ar.data : null);
    } catch(e) {}

    let html = '';

    if (xwebdResult) {
        html += renderDiagSection('xwebd 自检', xwebdResult);
    }
    if (assistantResult) {
        html += renderDiagSection('Assistant 自检', assistantResult);
    }
    if (!xwebdResult && !assistantResult) {
        html = '<div class="empty-state">自检请求失败，请检查设备连接</div>';
    }

    container.innerHTML = html;
    btn.disabled = false;
    btn.textContent = '运行自检';
}

function renderDiagSection(title, result) {
    const items = result.items || [];
    const okCount = result.ok_count || items.filter(i => i.ok).length;
    const failCount = result.fail_count || items.filter(i => !i.ok).length;
    const total = result.total || items.length;
    const allOk = failCount === 0;

    let html = '<div class="diag-section">';
    html += '<div class="diag-section-header">';
    html += '<span class="diag-section-title">' + title + '</span>';
    html += '<span class="diag-badge ' + (allOk ? 'diag-badge-ok' : 'diag-badge-fail') + '">';
    html += allOk ? '全部通过' : (okCount + '/' + total + ' 通过');
    html += '</span>';
    html += '</div>';

    html += '<div class="diag-items">';
    items.forEach(function(item) {
        const cls = item.ok ? 'diag-item-ok' : 'diag-item-fail';
        const icon = item.ok ? '&#10003;' : '&#10007;';
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
