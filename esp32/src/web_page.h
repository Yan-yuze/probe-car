#pragma once

#include <Arduino.h>

static const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <meta name="theme-color" content="#0f172a">
  <title>电子围栏小车控制台</title>
  <style>
    :root { color-scheme: dark; font-family: Inter,"Microsoft YaHei",system-ui,sans-serif; }
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
    body { margin:0; min-height:100vh; background:radial-gradient(circle at top,#1e3a5f,#0f172a 48%); color:#e2e8f0; }
    main { width:min(94vw,620px); margin:auto; padding:28px 0 40px; }
    h1 { margin:0 0 6px; font-size:clamp(24px,6vw,38px); letter-spacing:.02em; }
    .sub { margin:0 0 22px; color:#94a3b8; line-height:1.6; }
    .panel { background:rgba(15,23,42,.82); border:1px solid rgba(148,163,184,.18); border-radius:22px; padding:18px; box-shadow:0 24px 70px rgba(0,0,0,.28); backdrop-filter:blur(16px); }
    .status-grid { display:grid; grid-template-columns:repeat(2,1fr); gap:10px; margin-bottom:18px; }
    .status-card { background:#172033; border:1px solid #26344d; border-radius:14px; padding:12px; min-height:76px; }
    .label { display:block; color:#94a3b8; font-size:12px; margin-bottom:7px; }
    .value { display:flex; align-items:center; gap:8px; font-weight:700; font-size:15px; overflow-wrap:anywhere; }
    .dot { width:10px; height:10px; border-radius:50%; background:#ef4444; box-shadow:0 0 14px currentColor; flex:0 0 auto; }
    .dot.ok { background:#22c55e; }
    .command { color:#67e8f9; font-size:19px; }
    .dpad { display:grid; grid-template-columns:repeat(3,minmax(78px,1fr)); grid-template-rows:repeat(3,88px); gap:10px; user-select:none; touch-action:none; }
    button { border:0; border-radius:18px; color:#e2e8f0; background:linear-gradient(145deg,#263650,#18243a); box-shadow:0 7px 0 #0b1220,0 12px 24px rgba(0,0,0,.25); font:700 17px inherit; cursor:pointer; touch-action:none; transition:transform .06s,filter .06s,box-shadow .06s; }
    button:active,button.active { transform:translateY(6px); box-shadow:0 1px 0 #0b1220; filter:brightness(1.25); }
    button.stop { background:linear-gradient(145deg,#ef4444,#991b1b); box-shadow:0 7px 0 #601010,0 12px 24px rgba(0,0,0,.25); }
    [data-command="FORWARD"] { grid-column:2; grid-row:1; }
    [data-command="LEFT"] { grid-column:1; grid-row:2; }
    [data-command="STOP"] { grid-column:2; grid-row:2; }
    [data-command="RIGHT"] { grid-column:3; grid-row:2; }
    [data-command="BACKWARD"] { grid-column:2; grid-row:3; }
    .hint { margin:18px 2px 0; color:#94a3b8; font-size:13px; line-height:1.65; }
    .warning { margin-top:14px; padding:11px 13px; border-radius:12px; background:#3b1d24; color:#fecaca; display:none; }
    .warning.show { display:block; }
    .servo { margin-top:24px; padding-top:18px; border-top:1px solid #26344d; }
    .servo h2 { margin:0 0 14px; font-size:18px; }
    .servo-buttons { display:flex; gap:8px; }
    .servo-buttons button { flex:1; padding:12px 5px; border-radius:12px; font-size:13px; }
    .servo button:disabled,.servo input:disabled { opacity:.4; cursor:not-allowed; }
    .servo label { display:flex; justify-content:space-between; margin-top:20px; }
    .servo input { width:100%; margin-top:14px; accent-color:#67e8f9; }
    @media (max-width:420px) { .panel{padding:13px}.dpad{grid-template-rows:repeat(3,76px);gap:8px}button{font-size:15px}.status-grid{grid-template-columns:1fr 1fr} }
  </style>
</head>
<body>
<main>
  <h1>小车控制台</h1>
  <p class="sub">按住方向键移动，松开立即停车。支持键盘 W / A / S / D 和方向键。</p>
  <section class="panel">
    <div class="status-grid">
      <div class="status-card"><span class="label">网页链路</span><span class="value"><i id="webDot" class="dot"></i><b id="webState">连接中</b></span></div>
      <div class="status-card"><span class="label">STM32 状态 / 版本</span><span class="value"><i id="stmDot" class="dot"></i><b id="stmState">等待</b></span></div>
      <div class="status-card"><span class="label">当前命令</span><span id="commandState" class="value command">停止</span></div>
      <div class="status-card"><span class="label">ESP32 地址</span><span id="ipState" class="value">192.168.4.1</span></div>
    </div>

    <div class="dpad" aria-label="小车方向控制">
      <button data-command="FORWARD" aria-label="前进">▲<br>前进</button>
      <button data-command="LEFT" aria-label="左转">◀ 左转</button>
      <button class="stop" data-command="STOP" aria-label="停止">■<br>停止</button>
      <button data-command="RIGHT" aria-label="右转">右转 ▶</button>
      <button data-command="BACKWARD" aria-label="后退">▼<br>后退</button>
    </div>
    <div id="warning" class="warning">未收到 STM32 回执。请检查 GPIO17/18、PA2/PA3 和共地接线。</div>
    <div class="servo">
      <h2>SG90 舵机</h2>
      <div class="servo-buttons">
        <button data-servo-step="-10" disabled>← 舵机左转</button>
        <button id="servoCenter" disabled>回中</button>
        <button data-servo-step="10" disabled>舵机右转 →</button>
      </div>
      <label for="servoRange">目标角度 <output id="servoAngle">90°</output></label>
      <input id="servoRange" type="range" min="30" max="150" value="90" disabled>
      <p id="servoHint" class="hint">等待 STM32 舵机固件…</p>
    </div>
    <p class="hint">安全逻辑：控制消息中断 0.5 秒后 ESP32 自动停车；STM32 0.7 秒收不到串口命令也会停车。首次联调请架空车轮。</p>
  </section>
</main>
<script>
(() => {
  const names = {FORWARD:'前进',BACKWARD:'后退',LEFT:'左转',RIGHT:'右转',STOP:'停止'};
  const keyMap = {ArrowUp:'FORWARD',w:'FORWARD',W:'FORWARD',ArrowDown:'BACKWARD',s:'BACKWARD',S:'BACKWARD',ArrowLeft:'LEFT',a:'LEFT',A:'LEFT',ArrowRight:'RIGHT',d:'RIGHT',D:'RIGHT',' ':'STOP'};
  const buttons = [...document.querySelectorAll('button[data-command]')];
  const webDot = document.getElementById('webDot');
  const webState = document.getElementById('webState');
  const stmDot = document.getElementById('stmDot');
  const stmState = document.getElementById('stmState');
  const commandState = document.getElementById('commandState');
  const ipState = document.getElementById('ipState');
  const warning = document.getElementById('warning');
  let socket;
  let reconnectTimer;
  let repeatTimer;
  let activeCommand = 'STOP';
  let activeKey = null;
  let activePointerId = null;
  let servoReady = false;
  let servoBusy = false;
  let servoEditing = false;
  let servoTarget = 90;
  const servoRange = document.getElementById('servoRange');
  const servoAngle = document.getElementById('servoAngle');
  const servoHint = document.getElementById('servoHint');
  const servoButtons = [...document.querySelectorAll('[data-servo-step],#servoCenter')];

  function enableServo() {
    servoButtons.forEach(button => { button.disabled = !servoReady || servoBusy; });
    servoRange.disabled = !servoReady || servoBusy;
  }

  function displayServo(angle) {
    servoTarget = Math.max(30,Math.min(150,angle));
    servoRange.value = String(servoTarget);
    servoAngle.textContent = `${servoTarget}°`;
  }

  async function sendServo(angle) {
    if (!servoReady || servoBusy) return;
    displayServo(angle);
    servoBusy = true;
    enableServo();
    try {
      const response = await fetch('/api/servo', {method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'angle='+servoTarget,cache:'no-store'});
      if (!response.ok) throw new Error('servo command failed');
      servoHint.textContent = '目标已发送，等待 STM32 确认。';
    } catch (_) { servoHint.textContent = '舵机指令发送失败，请检查连接。'; }
    finally { servoBusy = false; enableServo(); }
  }

  function markButton(command) {
    buttons.forEach(button => button.classList.toggle('active', button.dataset.command === command && command !== 'STOP'));
  }

  async function sendHttp(command) {
    try {
      await fetch('/api/command', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd='+encodeURIComponent(command), cache:'no-store'});
    } catch (_) {}
  }

  function send(command) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(command);
    else sendHttp(command);
  }

  function stopMovement(forceSend = false) {
    clearInterval(repeatTimer);
    repeatTimer = null;
    const wasMoving = activeCommand !== 'STOP';
    activeCommand = 'STOP';
    activeKey = null;
    activePointerId = null;
    markButton('STOP');
    if (wasMoving || forceSend) send('STOP');
  }

  function startMovement(command, key = null) {
    if (command === 'STOP') { stopMovement(true); return; }
    if (activeCommand === command && repeatTimer) return;
    stopMovement(activeCommand !== 'STOP');
    activeCommand = command;
    activeKey = key;
    markButton(command);
    send(command);
    repeatTimer = setInterval(() => send(command), 180);
  }

  function updateStatus(status) {
    commandState.textContent = names[status.command] || status.command || '未知';
    ipState.textContent = status.station_ip || status.ap_ip || location.host;
    const stmOnline = Boolean(status.stm32_online);
    buttons.forEach(button => { if (button.dataset.command !== 'STOP') button.disabled = !stmOnline || status.motor_ready === false; });
    const stmVersion = typeof status.stm32_version === 'string' ? status.stm32_version : '';
    stmDot.classList.toggle('ok', stmOnline);
    stmState.textContent = stmOnline ? (stmVersion ? `在线 · ${stmVersion}` : '在线 · 读取中') : '未收到';
    warning.classList.toggle('show', (!stmOnline || status.motor_ready === false) && Number(status.uptime_ms) > 2500);
    warning.textContent = stmOnline && status.motor_ready === false ? '电机驱动板未就绪；请检查电源和 PB6/PB7，舵机仍可单独控制。' : '未收到 STM32 回执。请检查 GPIO17/18、PA2/PA3 和共地接线。';
    servoReady = Boolean(stmOnline && status.servo_supported);
    enableServo();
    if (servoReady && !servoBusy && !servoEditing && Number.isInteger(status.servo_target_angle)) displayServo(status.servo_target_angle);
    servoHint.textContent = !servoReady ? '请确认 STM32 / ESP32 已烧录舵机新固件。'
      : status.servo_angle === servoTarget ? '目标已确认。每次点击转动 10°，松开滑条后发送。' : '等待 STM32 确认目标。';
  }

  function connect() {
    clearTimeout(reconnectTimer);
    socket = new WebSocket(`ws://${location.hostname}:81/`);
    socket.onopen = () => {
      webDot.classList.add('ok');
      webState.textContent = '已连接';
      socket.send('PING');
    };
    socket.onmessage = event => {
      try { updateStatus(JSON.parse(event.data)); } catch (_) {}
    };
    socket.onclose = () => {
      webDot.classList.remove('ok');
      webState.textContent = '重连中';
      servoReady = false;
      enableServo();
      stopMovement(false);
      reconnectTimer = setTimeout(connect, 1000);
    };
    socket.onerror = () => socket.close();
  }

  buttons.forEach(button => {
    button.addEventListener('pointerdown', event => {
      event.preventDefault();
      button.setPointerCapture?.(event.pointerId);
      startMovement(button.dataset.command);
      if (button.dataset.command !== 'STOP') activePointerId = event.pointerId;
    });
  });
  window.addEventListener('pointerup', event => { if (event.pointerId === activePointerId) stopMovement(false); });
  window.addEventListener('pointercancel', event => { if (event.pointerId === activePointerId) stopMovement(false); });
  window.addEventListener('blur', () => stopMovement(true));
  document.addEventListener('visibilitychange', () => { if (document.hidden) stopMovement(true); });
  window.addEventListener('beforeunload', () => send('STOP'));
  window.addEventListener('keydown', event => {
    if (event.target.closest('input,textarea,select')) return;
    const command = keyMap[event.key];
    if (!command) return;
    event.preventDefault();
    if (!event.repeat) startMovement(command, event.key);
  });

  servoButtons.forEach(button => button.addEventListener('click', () => sendServo(button.id === 'servoCenter' ? 90 : servoTarget+Number(button.dataset.servoStep))));
  servoRange.addEventListener('input', () => { servoEditing = true; displayServo(Number(servoRange.value)); });
  servoRange.addEventListener('change', () => { servoEditing = false; sendServo(Number(servoRange.value)); });
  servoRange.addEventListener('blur', () => { servoEditing = false; });
  window.addEventListener('keyup', event => {
    if (event.key === activeKey || keyMap[event.key] === activeCommand) {
      event.preventDefault();
      stopMovement(false);
    }
  });

  setInterval(async () => {
    if (socket && socket.readyState === WebSocket.OPEN) return;
    try { const response = await fetch('/api/status', {cache:'no-store'}); updateStatus(await response.json()); } catch (_) {}
  }, 1000);
  connect();
})();
</script>
</body>
</html>
)HTML";
