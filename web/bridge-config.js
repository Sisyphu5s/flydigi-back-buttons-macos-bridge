/* WebHID client for the RP2350 vendor-defined configuration interface. */
(() => {
  'use strict';
  const DEVICES = [
    { vendorId: 0x1209, productId: 0x0001 },
    { vendorId: 0x04b4, productId: 0x2412 },
    { vendorId: 0x1b1c, productId: 0x3a28 },
    { vendorId: 0x18d1, productId: 0x9400 },
  ];
  /* ---------- 协议层：命令常量（协议来源 firmware/rp2350/main.c + docs/16） ----------
   * WebHID：请求走 Feature 报告 0x10（63B），响应读报告 0x11（63B；原始报告含报告
   * ID 时共 64B，桥在响应 [61..62] 回显请求 token）。解析/构造辅助函数按下列命令分组。 */
  const CMD = 0x10, RESP = 0x11;
  const CFG_SIZE = 59, REPORT_SIZE = 63;
  /* 设备端全局配置（设备内 0xA3 读 / 0xA0 写）相关命令。 */
  const GCFG_GET = 8;        /* 0x08 GET_GCFG：返回缓存并排队一次新读取 */
  const GCFG_SET_SLEEP = 9;  /* 0x09 SET_SLEEP：[1]=分钟（0=永不） */
  const GCFG_SET_RATE = 10;  /* 0x0A SET_RATE：[1..2]=Hz u16LE */
  /* 2026-10-18 二次核对：官方扩展命令（响应布局见 docs/16 第 2 节）。 */
  const INFO_GET = 11;       /* 0x0B GET_DEVICE_INFO */
  const FIELD_SET = 12;      /* 0x0C SET_FIELD：[1]=字段 [2..3]=值 u16LE，白名单见 applyField */
  const SLOT_SET = 13;       /* 0x0D SWITCH_SLOT：[1]=槽位 0..3 */
  const CALIB = 14;          /* 0x0E CALIBRATE：[1]=1 开始 / 0 结束 */
  const FP_GET = 15;         /* 0x0F GET_FINGERPRINT */
  const NAME_SET = 18;       /* 0x12 SET_NAME：[1..n]=文本 ≤16B（0x10 与报告 ID 冲突，协议避开） */
  const GCFG_POLL_MS = 600, GCFG_POLL_MAX = 8;
  const GCFG_SLEEP_OPTIONS = [0, 1, 5, 10, 15, 30, 60];
  const GCFG_RATE_OPTIONS = [1000, 500, 250, 125];
  const JSRES_OPTIONS = [8, 9, 10, 11, 12];
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
    for (const id of ['read', 'apply', 'save', 'defaults', 'rumble',
                      'gcfgRead', 'gcfgSleepApply', 'gcfgRateApply',
                      'gcfgFp', 'gcfgJsresApply', 'gcfgNameApply',
                      'slotBtn0', 'slotBtn1', 'slotBtn2', 'slotBtn3',
                      'calibStart', 'calibFinish'])
      $(id).disabled = !configReady || operationBusy;
    $('rumbleStop').disabled = !device?.opened;
    if (appleProfile && !persistentConfigSupported) {
      $('save').disabled = true;
      $('defaults').disabled = true;
    }
    for (const control of document.querySelectorAll('section input, #mapping select'))
      control.disabled = !configReady || operationBusy;
    $('freshXinputSticks').disabled = !configReady || operationBusy || !freshAxesSupported;
    for (const id of ['gcfgSleep', 'gcfgRate'])
      $(id).disabled = !configReady || operationBusy;
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
  /* 0x05 SAVE / 0x04 RESET 响应：[2..5]=保存序号 u32LE（等待 0x12 状态报告的 state/seq 确认落盘）。 */
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
  /* 协议层公共出口：构造 63B 请求（[0]=命令码 [1..]=载荷 [61..62]=token u16LE），
   * 发送到 Feature 报告 0x10，等 20ms 读 0x11 响应；raw[0] 为报告 ID 时去掉，
   * 返回协议索引切片并校验 token 回显。 */
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

  /* ---------- 协议层：0x08/0x09/0x0A/0x0C 设备端全局配置（读缓存并排队新读取） ---------- */

  /* 0x08 GET_GCFG 响应（切片后索引）：
   * [0]=status [1]=版本 [2..3]=rate u16LE [4]=睡眠分钟
   * [5]=flags（bit0 valid bit1 第三方接管 bit2 快速切换 bit3 中心自校准 bit4 板载映射 bit5 XInputHome）
   * [6]=摇杆分辨率 [7..10]=读取计数 u32 [11..14]=错误计数 u32 [15..30]=名称 16B
   * [31]=已排队新读取 [32..35]=0xA0 应答计数 u32 [36]=最近写入状态 [37]=最近写入字段。 */
  function parseGcfgResponse(r) {
    if (r.length < 38) throw new Error('设备配置响应长度不足');
    if (r[0] !== 0) throw new Error(`设备配置读取失败 (${r[0]})`);
    const view = new DataView(r.buffer, r.byteOffset, r.length);
    /* [15..30] 名称 NUL 填充；固件已把非可见字符换成 '.'，这里再去掉 NUL 之后的填充。 */
    const nul = r.indexOf(0, 15);
    const stop = nul === -1 || nul > 31 ? 31 : nul;
    let name = '';
    for (const byte of r.subarray(15, stop))
      name += byte >= 32 && byte < 127 ? String.fromCharCode(byte) : '.';
    return {
      version: r[1],
      rate: view.getUint16(2, true),
      sleep: r[4],
      valid: !!(r[5] & 1),
      allowThirdparty: !!(r[5] & 2),
      quickSwitch: !!(r[5] & 4),
      jsCenterAuto: !!(r[5] & 8),
      localMap: !!(r[5] & 16),
      xinputHome: !!(r[5] & 32),
      jsResolution: r[6],
      reads: view.getUint32(7, true),
      errors: view.getUint32(11, true),
      name: name.trim(),
      readRequested: !!r[31],
      a0Replies: view.getUint32(32, true),
      a0Status: r[36],
      a0Field: r[37],
    };
  }
  /* 0x08 响应 [32..37]：最近一次 0xA0 字段写的结果 → 面板中文标签。 */
  function gcfgA0Label(g) {
    if (!g.a0Replies) return '无记录';
    const names = { 1: '名称', 20: '快速切换', 23: '板载映射', 25: '中心自校准', 32: '睡眠', 33: '回报率', 34: '摇杆分辨率', 38: 'XInputHome' };
    const field = names[g.a0Field] ?? `字段 ${g.a0Field}`;
    return g.a0Status === 0 ? `${field}成功` : `${field}失败 (${g.a0Status})`;
  }
  function refreshGcfgPanel(g) {
    if (!g.valid) {
      $('gcfgStatus').textContent =
        `设备端配置: 尚未成功读取（读取 ${g.reads} 次 / 错误 ${g.errors} 次）| ${g.readRequested ? '已排队新读取，请稍后重试' : '请先连接设备'}`;
      return;
    }
    $('gcfgStatus').textContent =
      `设备端配置: 回报率 ${g.rate ? `${g.rate} Hz` : '未知'} | 睡眠 ${g.sleep ? `${g.sleep} 分钟` : '永不'} | ` +
      `第三方接管 ${g.allowThirdparty ? '允许' : '禁用'} | 摇杆分辨率 ${g.jsResolution} bit | ` +
      `快速切换 ${g.quickSwitch ? '开' : '关'} | 中心自校准 ${g.jsCenterAuto ? '开' : '关'} | ` +
      `板载映射 ${g.localMap ? '开' : '关'} | XInputHome ${g.xinputHome ? '开' : '关'} | ` +
      `名称 ${g.name || '（空）'} | 读取 ${g.reads} 次 / 错误 ${g.errors} 次 | 最近写入：${gcfgA0Label(g)}`;
    if (GCFG_SLEEP_OPTIONS.includes(g.sleep)) $('gcfgSleep').value = String(g.sleep);
    if (GCFG_RATE_OPTIONS.includes(g.rate)) $('gcfgRate').value = String(g.rate);
    if (JSRES_OPTIONS.includes(g.jsResolution)) $('gcfgJsres').value = String(g.jsResolution);
  }
  async function readGcfg() {
    return parseGcfgResponse(await command(GCFG_GET));
  }
  /* 回读同步：GET_GCFG 返回的是“本次新读取之前”的缓存，轮询直到读取计数超过基线。 */
  async function waitGcfgRead(baselineReads) {
    let last;
    for (let attempt = 0; attempt < GCFG_POLL_MAX; attempt++) {
      last = await readGcfg();
      if (last.reads > baselineReads) return last;
      if (attempt + 1 < GCFG_POLL_MAX) await new Promise(resolve => setTimeout(resolve, GCFG_POLL_MS));
    }
    refreshGcfgPanel(last);
    throw new Error('设备配置回读超时，面板显示的可能是旧值');
  }
  /* 0x09 SET_SLEEP：[1]=分钟（0=永不）；回显 [2]=分钟。 */
  async function applySleep(minutes) {
    const before = await readGcfg();
    const r = await command(GCFG_SET_SLEEP, [minutes]);
    if (r[0] !== 0) throw new Error(`睡眠写入失败 (${r[0]})`);
    if (r.length < 3 || r[2] !== minutes) throw new Error('睡眠写入回显不一致，请重新读取');
    refreshGcfgPanel(await waitGcfgRead(before.reads));
  }
  /* 0x0A SET_RATE：[1..2]=Hz u16LE；回显 [2..3]=Hz u16LE。 */
  async function applyRate(hz) {
    const before = await readGcfg();
    const r = await command(GCFG_SET_RATE, [hz & 0xff, (hz >> 8) & 0xff]);
    if (r[0] !== 0) throw new Error(`回报率写入失败 (${r[0]})`);
    if (r.length < 4 || (r[2] | r[3] << 8) !== hz) throw new Error('回报率写入回显不一致，请重新读取');
    refreshGcfgPanel(await waitGcfgRead(before.reads));
  }
  /* 0x0C SET_FIELD：[1]=字段 [2..3]=值 u16LE；回显 [2]=字段。
   * 白名单 {20,23,25,32,33,34,38}：jsResolution(34) 等。 */
  async function applyField(field, value) {
    const before = await readGcfg();
    const r = await command(FIELD_SET, [field, value & 0xff, (value >> 8) & 0xff]);
    if (r[0] !== 0) throw new Error(`字段 ${field} 写入失败 (${r[0]})`);
    if (r.length < 3 || r[2] !== field) throw new Error('字段写入回显不一致，请重新读取');
    refreshGcfgPanel(await waitGcfgRead(before.reads));
  }

  /* ---------- 协议层：0x0B/0x0D/0x0E/0x0F/0x12 设备信息 / 指纹 / 切槽 / 校准 / 改名 ---------- */

  /* 0x0B GET_DEVICE_INFO 响应：
   * [2]=model [3]=conn（1 无线 / 2 有线） [4]=电量（0xff 未知） [5]=芯片 lo=主 hi=接收
   * [6]=陀螺仪类型 [7]=能力位 [8..21]=7×fw u16BE（各 4 nibble：a.b.c.d）
   * [22..41]=SKU 20B [42]=skuValid [43]=exValid。 */
  function parseInfoResponse(r) {
    if (r.length < 44) throw new Error('设备信息响应长度不足');
    if (r[0] !== 0) throw new Error(`设备信息读取失败 (${r[0]})`);
    const view = new DataView(r.buffer, r.byteOffset, r.length);
    const fw = [];
    for (let i = 0; i < 7; i++) {
      const hi = r[8 + 2 * i], lo = r[9 + 2 * i];
      fw.push(hi || lo ? `${(hi >> 4) & 0xf}.${hi & 0xf}.${(lo >> 4) & 0xf}.${lo & 0xf}` : '-');
    }
    const nul = r.indexOf(0, 22);
    const stop = nul === -1 || nul > 41 ? 42 : nul;
    let sku = '';
    for (const byte of r.subarray(22, stop))
      sku += byte >= 32 && byte < 127 ? String.fromCharCode(byte) : '.';
    const FEATURES = ['IMU', 'LED', 'Switch', '屏幕', '扳机触觉', '扳机震动', '音频', '触觉'];
    return {
      model: r[2],
      connection: r[3],
      battery: r[4],
      chipMain: r[5] & 0xf, chipRecv: (r[5] >> 4) & 0xf,
      gyroType: r[6],
      features: r[7],
      featureNames: FEATURES.filter((_, i) => r[7] & (1 << i)),
      fw,
      sku: sku.trim(),
      skuValid: !!r[42],
      exValid: !!r[43],
    };
  }
  function refreshInfoPanel(info) {
    const connText = info.connection === 1 ? '无线' : info.connection === 2 ? '有线' : `?${info.connection}`;
    $('devInfo').textContent =
      `设备: model ${info.model}（${connText}）| 电量 ${info.battery === 255 ? '未知' : `${info.battery}%`} | ` +
      `SKU ${info.skuValid ? info.sku : '未知'} | 芯片 ${info.chipMain}/${info.chipRecv} | 陀螺仪 ${info.gyroType} | ` +
      `固件 主${info.fw[0]} 接收${info.fw[1]} SI${info.fw[2]} 扳机${info.fw[3]} 屏${info.fw[4]} ADC${info.fw[5]} Sstar${info.fw[6]} | ` +
      `能力 ${info.featureNames.join('/') || '无'}`;
  }
  async function readInfo() {
    const info = parseInfoResponse(await command(INFO_GET));
    refreshInfoPanel(info);
    return info;
  }
  /* 0x0F GET_FINGERPRINT 响应：
   * [2]=valid [3]=活动槽 [4..11]=槽 CRC16×4 LE [12..13]=全局 CRC16 [14..17]=推送计数 u32。 */
  function parseFingerprint(r) {
    if (r.length < 18) throw new Error('指纹响应长度不足');
    const view = new DataView(r.buffer, r.byteOffset, r.length);
    const crc = [];
    for (let i = 0; i < 4; i++) crc.push(view.getUint16(4 + 2 * i, true));
    return {
      valid: !!r[2], slot: r[3], crc,
      global: view.getUint16(12, true),
      pushes: view.getUint32(14, true),
    };
  }
  function refreshFingerprint(fp) {
    $('fpStatus').textContent = fp.valid
      ? `配置指纹: 活动槽 ${fp.slot + 1} | 槽 CRC ${fp.crc.map(c => c.toString(16).padStart(4, '0')).join('/')} | ` +
        `全局 CRC ${fp.global.toString(16).padStart(4, '0')} | 变更推送 ${fp.pushes} 次`
      : '配置指纹: 未读取（先连接设备）';
  }
  async function readFingerprint() {
    const fp = parseFingerprint(await command(FP_GET));
    refreshFingerprint(fp);
    return fp;
  }
  /* 0x0D SWITCH_SLOT：[1]=槽位 0..3；回显 [2]=槽位。切换后刷新指纹与 GCFG 面板。 */
  async function switchSlot(slot) {
    const before = await readGcfg();
    const r = await command(SLOT_SET, [slot]);
    if (r[0] !== 0) throw new Error(`切槽失败 (${r[0]})`);
    await new Promise(resolve => setTimeout(resolve, 300));
    await readFingerprint();
    refreshGcfgPanel(await waitGcfgRead(before.reads));
  }
  /* 0x0E CALIBRATE：[1]=1 开始 / 0 结束；回显 [2]。 */
  async function setCalibration(start) {
    const r = await command(CALIB, [start ? 1 : 0]);
    if (r[0] !== 0) throw new Error(`校准命令失败 (${r[0]})`);
  }
  /* 0x12 SET_NAME：[1..n]=UTF-8 名称 ≤16B；回显 [2]=字节数。 */
  async function setName(text) {
    const before = await readGcfg();
    const bytes = [];
    for (const ch of text) bytes.push(ch.charCodeAt(0) & 0xff);
    if (bytes.length > 16) throw new Error('名称最多 16 字节');
    const r = await command(NAME_SET, bytes);
    if (r[0] !== 0) throw new Error(`名称写入失败 (${r[0]})`);
    refreshGcfgPanel(await waitGcfgRead(before.reads));
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
    $('gcfgStatus').textContent = '设备端配置: -';
    $('devInfo').textContent = '设备信息: -';
    $('fpStatus').textContent = '配置指纹: -';
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
  /* 0x01 GET_INFO：legacy 信息（[4] 是否有电量 [5] 电量% [12] 输出按钮数 [13] token 支持
   * [14] Apple profile [15] 能力位）；0x02 GET_CONFIG：[2..60]=59B 配置体。 */
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
  /* 0x03 SET_CONFIG：载荷为 59B 配置体；[0]=status。 */
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
  $('gcfgRead').onclick = () => runExclusive(async () => {
    const g = await readGcfg();
    refreshGcfgPanel(g);
    setStatus(g.valid ? '已读取设备端全局配置' : '设备端全局配置尚未就绪（已排队新读取，请稍后重试）');
  }).catch(showError);
  $('gcfgSleepApply').onclick = () => runExclusive(async () => {
    const minutes = Number($('gcfgSleep').value) || 0;
    await applySleep(minutes);
    setStatus(`睡眠定时器已写入：${minutes ? `${minutes} 分钟` : '永不'}`);
  }).catch(showError);
  $('gcfgRateApply').onclick = () => runExclusive(async () => {
    const hz = Number($('gcfgRate').value) || 1000;
    await applyRate(hz);
    setStatus(`回报率已写入：${hz} Hz`);
  }).catch(showError);
  $('gcfgFp').onclick = () => runExclusive(async () => {
    await readFingerprint();
    await readInfo().catch(() => {});
    setStatus('已读取配置指纹');
  }).catch(showError);
  $('gcfgJsresApply').onclick = () => runExclusive(async () => {
    const res = Number($('gcfgJsres').value) || 10;
    await applyField(34, res);
    setStatus(`摇杆分辨率已写入：${res} bit`);
  }).catch(showError);
  $('gcfgNameApply').onclick = () => runExclusive(async () => {
    const text = $('gcfgName').value || '';
    await setName(text);
    setStatus(text ? `名称已写入：${text}` : '名称已清空');
  }).catch(showError);
  for (let slot = 0; slot < 4; slot++)
    $(`slotBtn${slot}`).onclick = () => runExclusive(async () => {
      await switchSlot(slot);
      setStatus(`已切换到配置槽 ${slot + 1}`);
    }).catch(showError);
  $('calibStart').onclick = () => runExclusive(async () => {
    await setCalibration(true);
    setStatus('校准已开始：请把两个摇杆各方向转动 2-3 圈，然后点「结束校准」');
  }).catch(showError);
  $('calibFinish').onclick = () => runExclusive(async () => {
    await setCalibration(false);
    setStatus('校准已结束');
  }).catch(showError);
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
        $('gcfgStatus').textContent = '设备端配置: -';
      }
      if (sensorDevice === disconnected) {
        sensorDevice = undefined;
        $('sensorStatus').textContent = '传感器已断开';
      }
    });
  }
})();
