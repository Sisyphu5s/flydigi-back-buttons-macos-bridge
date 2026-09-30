/* WebHID client for the RP2350 vendor-defined configuration interface. */
(() => {
  'use strict';
  const DEVICES = [
    { vendorId: 0x1209, productId: 0x0001 },
    { vendorId: 0x04b4, productId: 0x2412 },
    { vendorId: 0x1b1c, productId: 0x3a28 },
    { vendorId: 0x18d1, productId: 0x9400 },
  ];
  const CMD = 0x10, RESP = 0x11;
  const CFG_SIZE = 59, REPORT_SIZE = 63;
  const names = ['A','B','D-pad Up','X','Y','D-pad Down','LB','RB','D-pad Left','D-pad Right','Select','Start','L3','R3','M1','M2','M3','M4',null,'Home','C','Z','LM','RM','O'];
  const standardSlots = ['A','B','X','Y','LB','RB','LT','RT','Select','Start','L3','R3','D-pad Up','D-pad Down','D-pad Left','D-pad Right','Home','M1','M2','M3','M4','C','Z','LM','RM','O'];
  const vaderSlots = ['A','B','C','X','Y','Z','LB','RB','LM','RM','Select','Start','L3','R3','M1','M2','M3','M4','O','Home'];
  let outputSlots = 20;
  let appleProfile = 0;
  let freshAxesSupported = false;
  let overlayTelemetrySupported = false;
  let rawButtonsSupported = false;
  let rumbleAckSupported = false;
  let persistentConfigSupported = false;
  const $ = id => document.getElementById(id);
  let device;
  let sensorDevice;
  let previousTelemetry;
  let saveAwaiting = null;
  let lastSaveTelemetry = null;
  let cfg = new Uint8Array(CFG_SIZE);
  let appliedCfg;
  let configReady = false;
  let operationBusy = false;
  let commandQueue = Promise.resolve();
  let connectionGeneration = 0;
  let tokenSupported = false;
  let nextToken = crypto.getRandomValues(new Uint16Array(1))[0];
  const statusListeners = new WeakSet();
  const sensorListeners = new WeakSet();

  function setStatus(s) { $('status').textContent = s; }
  function staleConnection() {
    const error = new Error('设备连接已变化，请重新读取配置');
    error.stale = true;
    return error;
  }
  function showError(error) { if (!error.stale) setStatus(error.message); }
  function assertCurrent(target, generation) {
    if (device !== target || connectionGeneration !== generation) throw staleConnection();
    if (!target?.opened) throw new Error('请先连接设备');
  }
  function updateCommandButtons() {
    $('connect').disabled = operationBusy;
    for (const id of ['read', 'apply', 'save', 'defaults', 'rumble'])
      $(id).disabled = !configReady || operationBusy;
    $('rumbleStop').disabled = !device?.opened;
    if (appleProfile && !persistentConfigSupported) {
      $('save').disabled = true;
      $('defaults').disabled = true;
    }
    for (const control of document.querySelectorAll('section input, #mapping select'))
      control.disabled = !configReady || operationBusy;
    $('freshXinputSticks').disabled = !configReady || operationBusy || !freshAxesSupported;
  }
  function setConfigReady(ready) {
    configReady = ready;
    updateCommandButtons();
    if (!ready) $('mapping').textContent = '';
  }
  async function runExclusive(task) {
    if (operationBusy) return;
    const result = task();
    operationBusy = true;
    updateCommandButtons();
    try { return await result; }
    finally { operationBusy = false; updateCommandButtons(); }
  }
  function saveSequence(bytes, offset) {
    return new DataView(bytes.buffer, bytes.byteOffset + offset, 4).getUint32(0, true);
  }
  function updateSaveStatus() {
    if (saveAwaiting === null || !lastSaveTelemetry || lastSaveTelemetry.seq !== saveAwaiting) return;
    if (lastSaveTelemetry.state === 2 || lastSaveTelemetry.state === 3) {
      setStatus(lastSaveTelemetry.state === 2 ? '配置已保存到 Flash' : 'Flash 写入失败');
      saveAwaiting = null;
    }
  }
  function crc32(bytes, length = bytes.length) {
    let crc = 0xffffffff;
    for (let i = 0; i < length; i++) {
      crc ^= bytes[i];
      for (let b = 0; b < 8; b++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (~crc) >>> 0;
  }
  function put16(o, v) { new DataView(cfg.buffer).setUint16(o, Number(v) || 0, true); }
  function get16(o) { return new DataView(cfg.buffer).getUint16(o, true); }
  function putS16(o, v) { new DataView(cfg.buffer).setInt16(o, Number(v) || 0, true); }
  function getS16(o) { return new DataView(cfg.buffer).getInt16(o, true); }

  function renderMapping() {
    const root = $('mapping'); root.textContent = '';
    const appleExtendedSlots = ['A','B','slot 2','X','Y','slot 5','LB','RB','slot 8','slot 9',
      'Select','Start','L3','R3','M1','M2','M3','M4','slot 18','Home','C','Z','LM','RM','O','slot 25'];
    const scufSlots = ['A','B','slot 2','X','Y','slot 5','LB','RB','slot 8','slot 9',
      'Select','Start','Home','L3','R3','slot 15','slot 16','slot 17','Side Left','Side Right',
      'M1','M3','M4','M2','C','Z','LM','RM','O'];
    const labels = appleProfile === 2 ? scufSlots : appleProfile === 1 ? appleExtendedSlots :
      outputSlots === 26 && (get16(6) & 1) ? standardSlots :
      outputSlots === 26 ? [...vaderSlots, 'C', 'Z', 'LM', 'RM', 'O', 'slot 25'] : vaderSlots;
    for (let i = 0; i < names.length; i++) {
      if (!names[i]) continue;
      const label = document.createElement('label');
      const select = document.createElement('select'); select.dataset.source = i;
      for (let v = 0; v < outputSlots; v++) {
        const option = document.createElement('option'); option.value = v;
        option.textContent = `${v}: ${labels[v] || 'slot'}`;
        select.append(option);
      }
      const off = document.createElement('option'); off.value = 255; off.textContent = 'disabled'; select.append(off);
      select.value = cfg[30 + i];
      label.textContent = names[i] + ' '; label.append(select); root.append(label);
    }
  }
  function loadForm() {
    $('leftDeadzone').value = get16(8); $('rightDeadzone').value = get16(10);
    $('leftCurve').value = cfg[12]; $('rightCurve').value = cfg[13];
    $('leftCurveOut').value = cfg[12]; $('rightCurveOut').value = cfg[13];
    for (const [id, bit] of [['invLx',0],['invLy',1],['invRx',2],['invRy',3]]) $(id).checked = !!(cfg[14] & (1 << bit));
    const flags = get16(6);
    $('swapLeftAxes').checked = !!(flags & 0x20);
    $('swapRightAxes').checked = !!(flags & 0x40);
    $('radialLeft').checked = !!(flags & 2);
    $('radialRight').checked = !!(flags & 4);
    $('freshXinputSticks').checked = !!(flags & 8);
    $('rumbleLeft').value = cfg[24]; $('rumbleRight').value = cfg[25];
    $('rumbleInterval').value = get16(26); $('rumbleWatchdog').value = get16(28);
    $('gyroScale').value = get16(16); $('gyroBias').value = `${getS16(18)},${getS16(20)},${getS16(22)}`;
    for (const [id, bit] of [['invGx',0],['invGy',1],['invGz',2]]) $(id).checked = !!(cfg[15] & (1 << bit));
    renderMapping();
  }
  function saveForm() {
    for (const input of document.querySelectorAll('input[type="number"]')) {
      if (!input.value.trim() || !input.checkValidity())
        throw new Error(`${input.closest('label').firstChild.textContent.trim()}超出有效范围`);
    }
    const parts = $('gyroBias').value.split(',').map(value => value.trim());
    if (parts.length !== 3 || parts.some(value => !/^-?\d+$/.test(value) ||
        Number(value) < -32768 || Number(value) > 32767))
      throw new Error('陀螺仪偏置须为 -32768 到 32767 的三个整数');
    const bias = parts.map(Number);
    put16(8, $('leftDeadzone').value); put16(10, $('rightDeadzone').value);
    cfg[12] = Number($('leftCurve').value); cfg[13] = Number($('rightCurve').value);
    cfg[14] = 0; for (const [id, bit] of [['invLx',0],['invLy',1],['invRx',2],['invRy',3]]) if ($(id).checked) cfg[14] |= 1 << bit;
    put16(6, (get16(6) & ~(freshAxesSupported ? 14 : 6)) |
      ($('radialLeft').checked ? 2 : 0) | ($('radialRight').checked ? 4 : 0) |
      (freshAxesSupported && $('freshXinputSticks').checked ? 8 : 0) |
      ($('swapLeftAxes').checked ? 0x20 : 0) |
      ($('swapRightAxes').checked ? 0x40 : 0));
    cfg[24] = Number($('rumbleLeft').value); cfg[25] = Number($('rumbleRight').value);
    put16(26, $('rumbleInterval').value); put16(28, $('rumbleWatchdog').value); put16(16, $('gyroScale').value);
    for (let i = 0; i < 3; i++) putS16(18 + i * 2, bias[i]);
    cfg[15] = 0; for (const [id, bit] of [['invGx',0],['invGy',1],['invGz',2]]) if ($(id).checked) cfg[15] |= 1 << bit;
    document.querySelectorAll('#mapping select').forEach(s => { cfg[30 + Number(s.dataset.source)] = Number(s.value); });
    cfg[55] = crc32(cfg, 55); cfg[56] = crc32(cfg, 55) >>> 8; cfg[57] = crc32(cfg, 55) >>> 16; cfg[58] = crc32(cfg, 55) >>> 24;
  }
  function command(code, payload = []) {
    const out = new Uint8Array(REPORT_SIZE); out[0] = code; out.set(payload, 1);
    nextToken = (nextToken + 1) & 0xffff || 1;
    out[61] = nextToken & 0xff;
    out[62] = nextToken >> 8;
    const token = nextToken;
    const target = device, generation = connectionGeneration;
    const result = commandQueue.then(async () => {
      assertCurrent(target, generation);
      await target.sendFeatureReport(CMD, out);
      assertCurrent(target, generation);
      await new Promise(r => setTimeout(r, 20));
      assertCurrent(target, generation);
      const view = await target.receiveFeatureReport(RESP);
      assertCurrent(target, generation);
      const raw = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
      const response = raw[0] === RESP ? raw.subarray(1) : raw;
      if (response.length < 2) throw new Error('配置响应长度不足');
      if (response.length >= REPORT_SIZE) {
        const receivedToken = response[61] | response[62] << 8;
        if ((tokenSupported || receivedToken !== 0) && receivedToken !== token)
          throw new Error('配置响应被其他请求覆盖，操作结果未知，请先读取状态');
      } else if (tokenSupported) {
        throw new Error('配置响应长度不足，无法校验请求');
      }
      return response;
    });
    commandQueue = result.catch(() => {});
    return result;
  }
  async function connect() {
    const filters = DEVICES.map(d => ({...d, usagePage: 0xff00, usage: 1}));
    const list = await navigator.hid.requestDevice({filters});
    if (!list.length) return;
    const selected = list[0], generation = ++connectionGeneration;
    device = selected;
    tokenSupported = false;
    appleProfile = 0;
    freshAxesSupported = false;
    overlayTelemetrySupported = false;
    rawButtonsSupported = false;
    rumbleAckSupported = false;
    persistentConfigSupported = false;
    appliedCfg = undefined;
    setConfigReady(false);
    previousTelemetry = undefined;
    lastSaveTelemetry = null;
    saveAwaiting = null;
    if (!selected.opened) await selected.open();
    assertCurrent(selected, generation);
    updateCommandButtons();
    if (!statusListeners.has(selected)) selected.addEventListener('inputreport', ({reportId, data}) => {
      if (device !== selected || reportId !== 0x12) return;
      const offset = data.byteLength === 64 && data.getUint8(0) === reportId ? 1 : 0;
      if (data.byteLength - offset < 44) return;
      const battery = data.getUint8(offset);
      const now = performance.now();
      const frames = data.getUint32(offset + 8, true);
      const reports = data.getUint32(offset + 12, true);
      const rumbleSent = data.getUint32(offset + 16, true);
      const rumbleFailed = data.getUint32(offset + 20, true);
      const lastRumble = rumbleAckSupported ?
        ` | 最近完成 OUT 左 ${data.getUint8(offset + 6)} / 右 ${data.getUint8(offset + 7)}` : '';
      const xinput = data.getUint32(offset + 36, true);
      const badFrames = data.getUint32(offset + 40, true);
      const samples = data.byteLength - offset >= 60 ? data.getUint32(offset + 56, true) : null;
      const overlays = overlayTelemetrySupported && data.byteLength - offset >= 63 ?
        data.getUint8(offset + 60) | data.getUint8(offset + 61) << 8 |
        data.getUint8(offset + 62) << 16 : null;
      if (rawButtonsSupported && data.byteLength - offset >= 36) {
        const buttons = (data.getUint8(offset + 33) |
          data.getUint8(offset + 34) << 8 |
          data.getUint8(offset + 35) << 16 |
          data.getUint8(offset + 5) << 24) >>> 0;
        const pressed = names.filter((name, i) => name && (buttons & (1 << i)));
        $('pressedKeys').textContent = `按键: ${pressed.join(' / ') || '无'}`;
      }
      let rates = '';
      if (previousTelemetry && now > previousTelemetry.at) {
        const seconds = (now - previousTelemetry.at) / 1000;
        const efHz = (frames - previousTelemetry.frames) / seconds;
        const usbHz = (reports - previousTelemetry.reports) / seconds;
        const xiHz = (xinput - previousTelemetry.xinput) / seconds;
        rates = ` | XInput ${xiHz.toFixed(0)} Hz | EF ${efHz.toFixed(0)} Hz | USB 报告 ${usbHz.toFixed(0)} Hz`;
        if (samples !== null && previousTelemetry.samples !== null)
          rates += ` | 输入变化 ${((samples - previousTelemetry.samples) / seconds).toFixed(0)} Hz`;
        if (overlays !== null && previousTelemetry.overlays !== null)
          rates += ` | XInput 摇杆输出 ${(((overlays - previousTelemetry.overlays + 0x1000000) % 0x1000000) / seconds).toFixed(0)} Hz`;
      }
      previousTelemetry = {at: now, frames, reports, xinput, samples, overlays};
      lastSaveTelemetry = {state: data.getUint8(offset + 28), seq: data.getUint32(offset + 29, true)};
      updateSaveStatus();
      const ageMs = data.getUint32(offset + 24, true) / 1000;
      let bridgeAge = '';
      if (samples) {
        const last = data.getUint32(offset + 44, true) / 1000;
        const min = data.getUint32(offset + 48, true) / 1000;
        const max = data.getUint32(offset + 52, true) / 1000;
        bridgeAge = ` | 板内输入年龄 ${last.toFixed(2)} ms (${min.toFixed(2)}-${max.toFixed(2)})`;
      }
      $('telemetry').textContent = `接收器 ${data.getUint8(offset + 1) ? '已连接' : '未连接'} | ` +
        `电量 ${battery === 255 ? '未知' : `${battery}%`} | ` +
        `输入 ${data.getUint8(offset + 2) ? '在线' : '等待中'}` +
        rates + ` | 震动 OUT 成功 ${rumbleSent} / 失败 ${rumbleFailed}` + lastRumble +
        ` | EF 错帧 ${badFrames} | EF 年龄 ${ageMs.toFixed(1)} ms` + bridgeAge;
    });
    statusListeners.add(selected);
    setStatus(`已连接 ${selected.productName || 'Flydigi Bridge'}`);
    await readConfig();
  }
  async function readConfig() {
    const info = await command(1);
    const level = info[4] && info[5] !== 255 ? `${info[5]}%` : '未知';
    tokenSupported = info[13] === 1;
    outputSlots = info[12] || 20;
    appleProfile = info[14] || 0;
    freshAxesSupported = !!(info[15] & 1);
    overlayTelemetrySupported = !!(info[15] & 2);
    rawButtonsSupported = !!(info[15] & 4);
    rumbleAckSupported = !!(info[15] & 8);
    persistentConfigSupported = !!(info[15] & 16);
    $('pressedKeys').textContent = rawButtonsSupported ? '按键: 无' : '按键: 固件不支持实时状态';
    const response = await command(2);
    if (response.length < 2 + CFG_SIZE || response[0] !== 0) throw new Error('配置读取失败');
    cfg = response.slice(2, 2 + CFG_SIZE);
    appliedCfg = cfg.slice();
    loadForm(); setConfigReady(true);
    setStatus(`已读取配置 | 电量 ${level} | 输出按钮 ${outputSlots}` +
      (appleProfile && !persistentConfigSupported ? ' | 实验身份，Flash 保存已禁用' : ''));
  }
  async function apply() {
    saveForm();
    const r = await command(3, cfg);
    if (r[0] !== 0) throw new Error(`配置应用失败 (${r[0]})`);
    appliedCfg = cfg.slice();
    setStatus('配置已应用');
  }
  $('connect').onclick = () => runExclusive(connect).catch(showError);
  $('read').onclick = () => runExclusive(readConfig).catch(showError);
  $('apply').onclick = () => runExclusive(apply).catch(showError);
  $('save').onclick = () => runExclusive(async () => {
    await apply();
    const r = await command(5);
    if (r[0] !== 0) throw new Error(`保存请求失败 (${r[0]})`);
    saveAwaiting = saveSequence(r, 2);
    setStatus('正在保存到 Flash');
    updateSaveStatus();
  }).catch(e => { saveAwaiting = null; showError(e); });
  $('defaults').onclick = () => runExclusive(async () => {
    const r = await command(4);
    if (r[0] !== 0) throw new Error(`恢复默认失败 (${r[0]})`);
    await readConfig();
    saveAwaiting = saveSequence(r, 2);
    setStatus('正在保存默认配置到 Flash');
    updateSaveStatus();
  }).catch(e => { saveAwaiting = null; showError(e); });
  $('rumble').onclick = () => runExclusive(async () => {
    const left = Number($('testLeft').value), right = Number($('testRight').value);
    const r = await command(7, [left, right]);
    if (r[0] !== 0) throw new Error(`震动命令失败 (${r[0]})`);
    const gainLeft = appliedCfg[24], gainRight = appliedCfg[25];
    const effectiveLeft = Math.floor((left * gainLeft + 127) / 255);
    const effectiveRight = Math.floor((right * gainRight + 127) / 255);
    const watchdog = new DataView(appliedCfg.buffer).getUint16(28, true);
    setStatus(effectiveLeft || effectiveRight
      ? `震动命令已接受 | 已应用增益后左 ${effectiveLeft} / 右 ${effectiveRight} | ${watchdog}ms 自动停止`
      : '停振命令已接受（已应用增益后左右强度均为 0）');
  }).catch(showError);
  $('rumbleStop').onclick = async () => {
    const target = device, generation = connectionGeneration;
    try {
      assertCurrent(target, generation);
      const report = new Uint8Array(REPORT_SIZE);
      report[0] = 7;
      nextToken = (nextToken + 1) & 0xffff || 1;
      report[61] = nextToken & 0xff;
      report[62] = nextToken >> 8;
      await target.sendFeatureReport(CMD, report);
      assertCurrent(target, generation);
      setStatus('停振请求已发送');
    } catch (e) { showError(e); }
  };
  $('sensorConnect').onclick = async () => {
    try {
      const filters = DEVICES.map(d => ({...d, usagePage: 0x20, usage: 0x76}));
      const list = await navigator.hid.requestDevice({filters});
      if (!list.length) return;
      sensorDevice = list[0];
      if (!sensorDevice.opened) await sensorDevice.open();
      const selected = sensorDevice;
      if (!sensorListeners.has(selected)) selected.addEventListener('inputreport', ({reportId, data}) => {
        if (sensorDevice !== selected || (reportId !== 1 && reportId !== 2)) return;
        const offset = data.byteLength === 7 && data.getUint8(0) === reportId ? 1 : 0;
        if (data.byteLength - offset < 6) return;
        const values = [0, 2, 4].map(index => data.getInt16(offset + index, true));
        $(reportId === 1 ? 'gyroValues' : 'accelValues').textContent =
          `${reportId === 1 ? '陀螺仪' : '加速度计'}: ${values.join(', ')}`;
      });
      sensorListeners.add(sensorDevice);
      $('sensorStatus').textContent = '传感器已连接';
    } catch (e) { $('sensorStatus').textContent = e.message; }
  };
  $('leftCurve').oninput = e => $('leftCurveOut').value = e.target.value;
  $('rightCurve').oninput = e => $('rightCurveOut').value = e.target.value;
  $('testLeft').oninput = e => $('testLeftOut').value = e.target.value;
  $('testRight').oninput = e => $('testRightOut').value = e.target.value;
  setConfigReady(false);
  if (!navigator.hid) {
    for (const button of document.querySelectorAll('button')) button.disabled = true;
    setStatus('当前浏览器不支持 WebHID');
    $('sensorStatus').textContent = 'WebHID 不可用';
  } else {
    navigator.hid.addEventListener('disconnect', ({device: disconnected}) => {
      if (device === disconnected) {
        connectionGeneration++;
        device = undefined;
        tokenSupported = false;
        appleProfile = 0;
        freshAxesSupported = false;
        overlayTelemetrySupported = false;
        rawButtonsSupported = false;
        rumbleAckSupported = false;
        appliedCfg = undefined;
        previousTelemetry = undefined;
        saveAwaiting = null;
        setConfigReady(false);
        setStatus('设备已断开');
        $('telemetry').textContent = '接收器状态: -';
        $('pressedKeys').textContent = '按键: -';
      }
      if (sensorDevice === disconnected) {
        sensorDevice = undefined;
        $('sensorStatus').textContent = '传感器已断开';
      }
    });
  }
})();
