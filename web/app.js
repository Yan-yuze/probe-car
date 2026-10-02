(() => {
  const names = { FORWARD: '前进', BACKWARD: '后退', LEFT: '左转', RIGHT: '右转', STOP: '停止' };
  const keys = { ArrowUp: 'FORWARD', ArrowDown: 'BACKWARD', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT', w: 'FORWARD', s: 'BACKWARD', a: 'LEFT', d: 'RIGHT', ' ': 'STOP' };
  const controls = [...document.querySelectorAll('[data-command]')];
  const driveButtons = controls.filter(button => button.dataset.command !== 'STOP');
  const video = document.getElementById('video');
  const overlay = document.getElementById('video-overlay');
  const videoMessage = document.getElementById('video-message');
  const videoBadge = document.getElementById('video-badge');
  const videoDetail = document.getElementById('video-detail');
  const carDot = document.getElementById('car-dot');
  const carStatus = document.getElementById('car-status');
  const stmBadge = document.getElementById('stm-badge');
  const commandText = document.getElementById('command');
  const message = document.getElementById('control-message');
  const servoLeft = document.getElementById('servo-left');
  const servoStop = document.getElementById('servo-stop');
  const servoRight = document.getElementById('servo-right');
  const servoBadge = document.getElementById('servo-badge');
  const servoMessage = document.getElementById('servo-message');
  const servoState = document.getElementById('servo-state');
  const climateBadge = document.getElementById('climate-badge');
  const climateTemp = document.getElementById('climate-temp');
  const climateHum = document.getElementById('climate-hum');
  const climateMessage = document.getElementById('climate-message');
  const servoButtons = [servoLeft, servoStop, servoRight];
  let servoReady = false;
  let servoRotating = null;        // 'LEFT' | 'RIGHT' | null
  let servoPending = false;
  let servoQueued = false;
  let servoTimer = null;
  let activeServoPointerId = null;
  let nextServoSequence = 0;
  let activeCommand = 'STOP';
  let activeKey = null;
  let activePointerId = null;
  let repeatTimer = null;
  let motionPending = false;
  let controlsReady = false;
  let videoReady = false;
  let statusPending = false;
  let retryTimer = null;
  const clientId = (self.crypto && crypto.randomUUID) ? crypto.randomUUID() : String(Date.now()) + '-' + Math.random();
  let nextSequence = 0;

  function showMessage(text, error = false) {
    message.textContent = text;
    message.classList.toggle('error', error);
  }

  function markButton(command) {
    controls.forEach(button => button.classList.toggle('active', command !== 'STOP' && button.dataset.command === command));
  }

  async function send(command, keepalive = false) {
    const response = await fetch('/api/command', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Car-UI': '1' },
      body: JSON.stringify({ command, client_id: clientId, sequence: ++nextSequence }),
      cache: 'no-store',
      keepalive
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || '发送失败');
  }

  function stopMovement(force = false, keepalive = false) {
    clearInterval(repeatTimer);
    repeatTimer = null;
    const wasMoving = activeCommand !== 'STOP';
    activeCommand = 'STOP';
    activeKey = null;
    activePointerId = null;
    commandText.textContent = '停止';
    markButton('STOP');
    if (wasMoving || force) {
      send('STOP', keepalive).catch(() => showMessage('停车指令未送达；请检查连接，ESP32 / STM32 超时保护会停车。', true));
    }
  }

  function transmitMotion(command) {
    if (motionPending || activeCommand !== command || !controlsReady) return;
    motionPending = true;
    send(command).catch(() => {
      stopMovement(true);
      showMessage('控制指令发送失败，已停止持续发送。', true);
    }).finally(() => { motionPending = false; });
  }

  function startMovement(command, key = null) {
    if (command === 'STOP') { stopMovement(true); return; }
    if (!controlsReady) return;
    if (activeCommand === command && repeatTimer) return;
    stopMovement(activeCommand !== 'STOP');
    activeCommand = command;
    activeKey = key;
    commandText.textContent = names[command];
    markButton(command);
    transmitMotion(command);
    repeatTimer = setInterval(() => transmitMotion(command), 180);
  }

  /*
   * 本车舵机是 360° 连续旋转型：脉宽离「它自己的停止点」越远转得越快，
   * 大于停止点朝一个方向，小于停止点朝另一个方向。
   *
   *     SERVO_STOP_ANGLE  停止点。这一颗实测约 1489us -> 角度 88，不是标称的 1500us。
   *     SERVO_SPAN        转速偏移，越小越慢，15 -> ±83us
   *
   *     左转 = 停止点 - 偏移        右转 = 停止点 + 偏移
   *
   * 两个方向要一样快，前提是这两个值「对称分布在舵机真实停止点两侧」，
   * 所以 SERVO_STOP_ANGLE 必须等于真实停止点，不能照搬标称的 1500us（角度 90）：
   *     右转比左转快  ->  真实停止点比当前值低  ->  把这个数调小
   *     左转比右转快  ->  反过来                ->  把这个数调大
   * 一次调 1~2 即可（1 个角度约等于 5.6us）。
   *
   * 也不要改成「左转一个值、右转另一个值」的两个独立常量：之前就是因为
   * SERVO_RIGHT_ANGLE 被改成了和左转相同的值，两个按钮发出同一个脉宽，左右同向。
   */
  const SERVO_STOP_ANGLE = 88;
  const SERVO_SPAN = 15;
  const SERVO_REPEAT_MS = 180;
  const SERVO_LABELS = { LEFT: '左转中', RIGHT: '右转中', STOP: '已停止' };

  /* 设备端（app.py / ESP32 / STM32）都只接受 30~150，这里兜底夹一下 */
  function clampServoAngle(value) {
    return Math.max(30, Math.min(150, Math.round(value)));
  }

  /*
   * 方向映射：+ 偏移 = 左转，- 偏移 = 右转。
   * 2026-10-02 实车测试发现方向反了，所以把这两行的正负号对调了。
   * 以后再反了，把这两行的 + / - 换回来即可——只改网页，不用重烧 STM32。
   * 注意两个方向必须是一个 + 一个 -，偏移量都是 SERVO_SPAN，这样左右转速才一样。
   */
  function currentServoAngle() {
    if (servoRotating === 'LEFT') return clampServoAngle(SERVO_STOP_ANGLE + SERVO_SPAN);
    if (servoRotating === 'RIGHT') return clampServoAngle(SERVO_STOP_ANGLE - SERVO_SPAN);
    return clampServoAngle(SERVO_STOP_ANGLE);
  }

  function setServoEnabled() {
    servoButtons.forEach(button => { button.disabled = !servoReady; });
  }

  function markServoButton(mode) {
    const target = mode ? 'servo-' + mode.toLowerCase() : null;
    servoButtons.forEach(button => button.classList.toggle('active', button.id === target));
  }

  function transmitServo() {
    if (!servoReady || servoPending) { servoQueued = servoReady; return; }
    servoPending = true;
    fetch('/api/servo', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Car-UI': '1' },
      body: JSON.stringify({ angle: currentServoAngle(), client_id: clientId, sequence: ++nextServoSequence }),
      cache: 'no-store',
      keepalive: true
    }).then(response => response.json().then(result => {
      if (!response.ok || !result.ok) throw new Error(result.error || '发送失败');
      servoMessage.classList.remove('error');
      servoMessage.textContent = servoRotating
        ? `正在${SERVO_LABELS[servoRotating]}（左 ${clampServoAngle(SERVO_STOP_ANGLE - SERVO_SPAN)} / 右 ${clampServoAngle(SERVO_STOP_ANGLE + SERVO_SPAN)}）。`
        : '已停止。';
    })).catch(() => {
      clearInterval(servoTimer);
      servoTimer = null;
      servoRotating = null;
      servoState.textContent = SERVO_LABELS.STOP;
      markServoButton(null);
      servoMessage.textContent = '舵机指令发送失败，已停止续发；STM32 超时保护也会停车。';
      servoMessage.classList.add('error');
    }).finally(() => {
      servoPending = false;
      if (servoQueued) { servoQueued = false; transmitServo(); }
    });
  }

  function stopServo(force = false) {
    clearInterval(servoTimer);
    servoTimer = null;
    const wasRotating = servoRotating !== null;
    servoRotating = null;
    activeServoPointerId = null;
    servoState.textContent = SERVO_LABELS.STOP;
    markServoButton(null);
    if (wasRotating || force) transmitServo();
  }

  function startServo(mode) {
    if (!servoReady || servoRotating === mode) return;
    servoRotating = mode;
    servoState.textContent = SERVO_LABELS[mode];
    markServoButton(mode);
    clearInterval(servoTimer);
    transmitServo();
    servoTimer = setInterval(transmitServo, SERVO_REPEAT_MS);
  }

  function renderServo(car) {
    servoReady = Boolean(car.ok && car.stm32_online && car.servo_supported);
    setServoEnabled();
    servoBadge.textContent = servoReady ? '舵机就绪' : '舵机未就绪';
    servoBadge.classList.toggle('ok', servoReady);
    servoBadge.classList.toggle('bad', !servoReady);
    servoMessage.classList.toggle('error', !servoReady);
    if (!servoReady) {
      if (servoRotating !== null) stopServo(true);
      servoState.textContent = SERVO_LABELS.STOP;
      servoMessage.textContent = car.ok && car.stm32_online
        ? '请烧录支持舵机的 STM32 和 ESP32 固件。'
        : '等待 ESP32 与 STM32 连接。';
      return;
    }
    if (servoRotating === null && !servoPending) {
      servoMessage.textContent = '按住「左转」或「右转」舵机才转，松开立即停。';
    }
  }

  /* AHT10 温湿度：数据来自 ESP32 的 /api/status 里的 climate 字段 */
  function renderClimate(car) {
    const climate = (car && car.climate) ? car.climate : {};
    const ok = Boolean(climate.ok);
    const hasTemp = Number.isFinite(climate.temperature_c);
    const hasHum = Number.isFinite(climate.humidity_pct);

    climateBadge.textContent = ok ? `${climate.chip || 'AHT10'} 在线` : '未检测到';
    climateBadge.classList.toggle('ok', ok);
    climateBadge.classList.toggle('bad', !ok);

    climateTemp.textContent = (ok && hasTemp) ? `${climate.temperature_c.toFixed(1)} °C` : '--';
    climateHum.textContent = (ok && hasHum) ? `${climate.humidity_pct.toFixed(1)} %` : '--';
    climateTemp.classList.toggle('bad', !(ok && hasTemp));
    climateHum.classList.toggle('bad', !(ok && hasHum));

    climateMessage.classList.toggle('error', !ok);
    if (ok) {
      const age = Number.isFinite(climate.age_ms) ? ` · ${(climate.age_ms / 1000).toFixed(1)} 秒前` : '';
      climateMessage.textContent =
        `${climate.chip || 'AHT10'} @ ${climate.address || '0x38'}${age} · 已采样 ${climate.samples ?? 0} 次`;
    } else if (climate.present === false) {
      climateMessage.textContent =
        `没找到传感器（${climate.error || '检查接线'}）。当前 I2C 上扫到：${climate.i2c_scan || '无'}`;
    } else {
      climateMessage.textContent = climate.error || '正在检查 AHT10 传感器…';
    }
  }

  async function pollStatus() {
    if (statusPending) return;
    statusPending = true;
    try {
      const response = await fetch('/api/status', { cache: 'no-store' });
      if (!response.ok) throw new Error('本机服务未响应');
      const state = await response.json();
      const car = state.car || {};
      renderServo(car);
      renderClimate(car);
      controlsReady = Boolean(car.ok && car.stm32_online && car.motor_ready !== false);
      driveButtons.forEach(button => { button.disabled = !controlsReady; });
      carDot.classList.toggle('ok', Boolean(car.ok));
      carDot.classList.toggle('bad', !car.ok);
      carStatus.textContent = car.ok ? 'ESP32 已连接' : 'ESP32 未连接';
      stmBadge.textContent = car.stm32_online ? `STM32 在线${car.stm32_version ? ' · ' + car.stm32_version : ''}` : 'STM32 未连接';
      stmBadge.classList.toggle('ok', controlsReady);
      stmBadge.classList.toggle('bad', !controlsReady);
      if (!controlsReady) {
        if (activeCommand !== 'STOP') stopMovement(true);
        showMessage(car.ok && car.stm32_online && car.motor_ready === false
          ? '电机驱动板未就绪，请检查 PB6/PB7 和电源；舵机仍可单独控制。'
          : car.ok ? 'ESP32 在线，但未收到 STM32 回执；请检查串口接线。' : '无法连接 ESP32；请确认电脑与小车在可互通的网络。', true);
      } else if (activeCommand === 'STOP') {
        commandText.textContent = names[car.command] || '停止';
        showMessage('控制已就绪。按住按钮移动，松开停车。');
      }
      const stream = state.video || {};
      videoReady = Boolean(stream.ok);
      videoBadge.textContent = videoReady ? '实时传输中' : '图像未就绪';
      videoBadge.classList.toggle('ok', videoReady);
      videoBadge.classList.toggle('bad', !videoReady);
      videoDetail.textContent = videoReady ? `第 ${stream.frame_id ?? stream.frames ?? '—'} 帧 · ${stream.age_ms} ms` : '请检查 CyberCAM /health 和图像服务';
      overlay.classList.toggle('hidden', videoReady);
      if (!videoReady) videoMessage.textContent = '暂无实时画面，请检查 CyberCAM 的 8081 端口。';
    } catch (_) {
      controlsReady = false;
      renderServo({});
      driveButtons.forEach(button => { button.disabled = true; });
      if (activeCommand !== 'STOP') stopMovement(true);
      showMessage('本机网页服务连接中断。', true);
    } finally {
      statusPending = false;
    }
  }

  video.addEventListener('error', () => {
    overlay.classList.remove('hidden');
    videoMessage.textContent = '视频连接中断，正在重连…';
    clearTimeout(retryTimer);
    retryTimer = setTimeout(() => { video.src = '/video.mjpg?t=' + Date.now(); }, 1500);
  });

  controls.forEach(button => button.addEventListener('pointerdown', event => {
    event.preventDefault();
    button.setPointerCapture?.(event.pointerId);
    startMovement(button.dataset.command);
    if (button.dataset.command !== 'STOP' && controlsReady) activePointerId = event.pointerId;
  }));
  window.addEventListener('pointerup', event => { if (event.pointerId === activePointerId) stopMovement(); });
  window.addEventListener('pointercancel', event => { if (event.pointerId === activePointerId) stopMovement(); });
  window.addEventListener('blur', () => { stopMovement(true); stopServo(true); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) { stopMovement(true, true); stopServo(true); } });
  window.addEventListener('pagehide', () => { stopMovement(true, true); stopServo(true); });
  window.addEventListener('keydown', event => {
    if (event.altKey || event.ctrlKey || event.metaKey || event.target.closest('input,textarea')) return;
    const command = keys[event.key] || keys[event.key.toLowerCase()];
    if (!command) return;
    event.preventDefault();
    if (!event.repeat) startMovement(command, event.key);
  });
  window.addEventListener('keyup', event => {
    if (event.key === activeKey) {
      event.preventDefault();
      stopMovement();
    }
  });

  servoLeft.addEventListener('pointerdown', event => {
    event.preventDefault();
    servoLeft.setPointerCapture?.(event.pointerId);
    activeServoPointerId = event.pointerId;
    startServo('LEFT');
  });
  servoRight.addEventListener('pointerdown', event => {
    event.preventDefault();
    servoRight.setPointerCapture?.(event.pointerId);
    activeServoPointerId = event.pointerId;
    startServo('RIGHT');
  });
  servoStop.addEventListener('pointerdown', event => {
    event.preventDefault();
    stopServo(true);
  });
  window.addEventListener('pointerup', event => { if (event.pointerId === activeServoPointerId) stopServo(); });
  window.addEventListener('pointercancel', event => { if (event.pointerId === activeServoPointerId) stopServo(); });

  pollStatus();
  setInterval(pollStatus, 1500);
})();
