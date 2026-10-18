const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(`${__dirname}/bridge-config.js`, 'utf8');

function deferred() {
  let resolve;
  const promise = new Promise(r => { resolve = r; });
  return {promise, resolve};
}

function setup() {
  const elements = new Map();
  const listeners = new Map();
  let now = 0;
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      id, disabled: false, textContent: '', value: '0', checked: false,
      append() {}, setAttribute() {},
      closest() { return {firstChild: {textContent: id}}; },
      checkValidity() { return true; },
    });
    return elements.get(id);
  };
  const document = {
    getElementById: element,
    createElement: () => ({append() {}, value: '', textContent: '', dataset: {}}),
    querySelectorAll: () => [],
  };
  const hid = {
    nextDevice: null,
    requestedFilters: null,
    async requestDevice({filters}) { this.requestedFilters = filters; return [this.nextDevice]; },
    addEventListener(name, handler) { listeners.set(name, handler); },
  };
  vm.runInNewContext(source, {
    document, navigator: {hid}, crypto: {getRandomValues: array => { array[0] = 100; return array; }},
    performance: {now: () => now}, setTimeout,
  });
  return {element, hid, advanceTime: ms => { now += ms; },
    disconnect: device => listeners.get('disconnect')({device})};
}

function fakeDevice() {
  const device = {
    opened: false, sends: [], responses: [], pending: null, listeners: new Map(), capabilities: 15,
    async open() { this.opened = true; },
    addEventListener(name, handler) { this.listeners.set(name, handler); },
    async sendFeatureReport(id, bytes) { this.sends.push({id, bytes: Uint8Array.from(bytes)}); },
    async receiveFeatureReport() {
      if (this.pending) return this.pending.promise;
      const request = this.sends.at(-1).bytes;
      const response = new Uint8Array(64);
      response[0] = 0x11;
      response[2] = 1;
      if (request[0] === 1) { response[5] = 1; response[6] = 80; response[14] = 1; response[16] = this.capabilities; }
      response[62] = request[61];
      response[63] = request[62];
      if (this.badToken) response[62] ^= 1;
      this.responses.push(response);
      return new DataView(response.buffer);
    },
  };
  return device;
}

/* 构造 GET_GCFG(8) 应答：协议布局见 firmware/rp2350/main.c；
 * receiveFeatureReport 返回的原始报告 [0] 是 0x11 报告 ID，[i+1] 才对应协议索引 i。 */
function gcfgResponse(g, request) {
  const bytes = new Uint8Array(64);
  const view = new DataView(bytes.buffer);
  bytes[0] = 0x11;                                        /* 报告 ID */
  bytes[2] = 1;                                           /* [1] 版本 */
  view.setUint16(3, g.rate, true);                        /* [2..3] 回报率 Hz */
  bytes[5] = g.sleep;                                     /* [4] 睡眠分钟 */
  bytes[6] = (g.valid ? 1 : 0) | (g.thirdparty ? 2 : 0);  /* [5] flags */
  bytes[7] = g.jsResolution;                              /* [6] 摇杆分辨率 */
  view.setUint32(8, g.reads, true);                       /* [7..10] 成功读取计数 */
  view.setUint32(12, g.errors || 0, true);                /* [11..14] 失败计数 */
  const name = new TextEncoder().encode(g.name || '');    /* [15..30] 名称 */
  for (let i = 0; i < 16; i++) bytes[16 + i] = name[i] || 0;
  bytes[32] = 1;                                          /* [31] 本次已排队新读取 */
  view.setUint32(33, g.a0Replies || 0, true);             /* [32..35] 0xA0 应答计数 */
  bytes[37] = g.a0Status || 0;                            /* [36] 最近写入状态 */
  bytes[38] = g.a0Field || 0;                             /* [37] 最近写入字段 */
  bytes[62] = request[61];
  bytes[63] = request[62];
  return new DataView(bytes.buffer);
}

function gcfgEcho(request) {
  const bytes = new Uint8Array(64);
  bytes[0] = 0x11;
  bytes[2] = 1;
  bytes[3] = request[1];                                  /* [2..3] 回显 */
  bytes[4] = request[2];
  bytes[62] = request[61];
  bytes[63] = request[62];
  return new DataView(bytes.buffer);
}

async function testMismatch() {
  const {element, hid} = setup();
  const device = fakeDevice();
  device.badToken = true;
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.equal(device.sends.length, 1);
  assert.match(element('status').textContent, /其他请求/);
  assert.equal(element('apply').disabled, true);
}

async function testStadiaConfigFilter() {
  const {element, hid} = setup();
  hid.nextDevice = fakeDevice();
  await element('connect').onclick();
  assert.ok(hid.requestedFilters.some(filter => filter.vendorId === 0x18d1 &&
    filter.productId === 0x9400 && filter.usagePage === 0xff00 && filter.usage === 1));
}

async function testLegacyResponse() {
  const {element, hid} = setup();
  const device = fakeDevice();
  device.badToken = true;
  device.receiveFeatureReport = async function () {
    const response = new Uint8Array(64);
    response[0] = 0x11;
    response[2] = 1;
    if (this.sends.at(-1).bytes[0] === 1) response[6] = 80;
    if (this.sends.at(-1).bytes[0] === 2) response[3 + 6] = 8;
    return new DataView(response.buffer);
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.match(element('status').textContent, /已读取配置/);
  assert.equal(element('freshXinputSticks').disabled, true);
  await element('apply').onclick();
  assert.equal(device.sends.at(-1).bytes[1 + 6] & 8, 8);
}

async function testUnknownBattery() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async function () {
    const response = await receive.call(this);
    if (this.sends.at(-1).bytes[0] === 1) response.setUint8(5, 0);
    return response;
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.match(element('status').textContent, /电量 未知/);
}

async function testRadialFlags() {
  const {element, hid} = setup();
  const device = fakeDevice();
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.equal(element('freshXinputSticks').disabled, false);
  element('gyroBias').value = '0,0,0';
  element('radialLeft').checked = true;
  element('radialRight').checked = false;
  element('freshXinputSticks').checked = true;
  await element('apply').onclick();
  const request = device.sends.at(-1).bytes;
  assert.equal(request[0], 3);
  assert.equal(request[1 + 6] & 14, 10);
}

async function testIndependentRumble() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async function () {
    const response = await receive.call(this);
    if (this.sends.at(-1).bytes[0] === 2) {
      response.setUint8(3 + 24, 128);
      response.setUint8(3 + 25, 64);
      response.setUint16(3 + 28, 250, true);
    }
    if (this.sends.at(-1).bytes[0] === 3 && this.rejectApply)
      response.setUint8(1, 1);
    return response;
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  element('testLeft').value = '220';
  element('testRight').value = '40';
  element('rumbleLeft').value = '255';
  element('rumbleWatchdog').value = '2000';
  await element('rumble').onclick();
  assert.deepEqual(Array.from(device.sends.at(-1).bytes.slice(0, 3)), [7, 220, 40]);
  assert.match(element('status').textContent, /左 110 \/ 右 10 \| 250ms 自动停止/);
  device.rejectApply = true;
  element('gyroBias').value = '0,0,0';
  await element('apply').onclick();
  assert.match(element('status').textContent, /配置应用失败/);
  await element('rumble').onclick();
  assert.match(element('status').textContent, /左 110 \/ 右 10 \| 250ms 自动停止/);
  await element('rumbleStop').onclick();
  assert.deepEqual(Array.from(device.sends.at(-1).bytes.slice(0, 3)), [7, 0, 0]);
}

async function testStopDuringBusyCommand() {
  const {element, hid} = setup();
  const device = fakeDevice();
  hid.nextDevice = device;
  await element('connect').onclick();
  device.pending = deferred();
  const applying = element('apply').onclick();
  await new Promise(resolve => setTimeout(resolve, 25));
  assert.equal(element('apply').disabled, true);
  assert.equal(element('rumbleStop').disabled, false);
  const applyReport = device.sends.at(-1).bytes;
  assert.equal(applyReport[0], 3);
  await element('rumbleStop').onclick();
  assert.deepEqual(Array.from(device.sends.at(-1).bytes.slice(0, 3)), [7, 0, 0]);
  assert.match(element('status').textContent, /停振请求已发送/);
  const response = new Uint8Array(64);
  response[0] = 0x11;
  response[2] = 1;
  response[62] = applyReport[61];
  response[63] = applyReport[62];
  device.pending.resolve(new DataView(response.buffer));
  await applying;
}

async function testBridgeAgeTelemetry() {
  const {element, hid, advanceTime} = setup();
  const device = fakeDevice();
  hid.nextDevice = device;
  await element('connect').onclick();
  const bytes = new Uint8Array(63);
  const view = new DataView(bytes.buffer);
  bytes[0] = 80;
  bytes[1] = 1;
  bytes[2] = 1;
  bytes[6] = 110;
  bytes[7] = 10;
  view.setUint32(16, 3, true);
  view.setUint32(20, 1, true);
  view.setUint32(44, 850, true);
  view.setUint32(48, 200, true);
  view.setUint32(52, 1300, true);
  view.setUint32(56, 3, true);
  bytes[60] = 2;
  device.listeners.get('inputreport')({reportId: 0x12, data: view});
  assert.match(element('telemetry').textContent,
    /板内输入年龄 0\.85 ms \(0\.20-1\.30\)/);
  assert.match(element('telemetry').textContent, /震动 OUT 成功 3 \/ 失败 1/);
  assert.match(element('telemetry').textContent, /最近完成 OUT 左 110 \/ 右 10/);
  advanceTime(1000);
  view.setUint32(8, 100, true);
  view.setUint32(12, 248, true);
  view.setUint32(56, 13, true);
  bytes[60] = 12;
  device.listeners.get('inputreport')({reportId: 0x12, data: view});
  assert.match(element('telemetry').textContent,
    /EF 100 Hz \| USB 报告 248 Hz \| 输入变化 10 Hz/);
  assert.match(element('telemetry').textContent, /XInput 摇杆输出 10 Hz/);
}

async function testLiveRawButtons() {
  const {element, hid, disconnect} = setup();
  const device = fakeDevice();
  hid.nextDevice = device;
  await element('connect').onclick();
  const bytes = new Uint8Array(63);
  bytes[5] = 1; // O, source index 24
  bytes[33] = 1; // A, source index 0
  bytes[34] = 1 << 6; // M1, source index 14
  const input = device.listeners.get('inputreport');
  input({reportId: 0x12, data: new DataView(bytes.buffer)});
  assert.equal(element('pressedKeys').textContent, '按键: A / M1 / O');
  bytes[5] = bytes[33] = bytes[34] = 0;
  input({reportId: 0x12, data: new DataView(bytes.buffer)});
  assert.equal(element('pressedKeys').textContent, '按键: 无');
  disconnect(device);
  assert.equal(element('pressedKeys').textContent, '按键: -');
}

async function testAppleExtendedMode(profile, count) {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async function () {
    const response = await receive.call(this);
    if (this.sends.at(-1).bytes[0] === 1) {
      response.setUint8(13, count);
      response.setUint8(15, profile);
    }
    return response;
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.match(element('status').textContent, /实验身份，Flash 保存已禁用/);
  assert.match(element('status').textContent, new RegExp(`输出按钮 ${count}`));
  assert.equal(element('apply').disabled, false);
  assert.equal(element('save').disabled, true);
  assert.equal(element('defaults').disabled, true);
}

async function testPersistentAppleProfile() {
  const {element, hid} = setup();
  const device = fakeDevice();
  device.capabilities = 31;
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async function () {
    const response = await receive.call(this);
    if (this.sends.at(-1).bytes[0] === 1) {
      response.setUint8(13, 26);
      response.setUint8(15, 1);
    }
    return response;
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.equal(element('save').disabled, false);
  assert.equal(element('defaults').disabled, false);
  assert.doesNotMatch(element('status').textContent, /实验身份/);
  element('gyroBias').value = '0,0,0';
  await element('save').onclick();
  assert.equal(device.sends.at(-1).bytes[0], 5);
  await element('defaults').onclick();
  assert.ok(device.sends.some(send => send.bytes[0] === 4));
}

async function testGcfgReadDisplay() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  const state = {rate: 500, sleep: 15, valid: true, thirdparty: true, jsResolution: 12,
    reads: 3, errors: 1, name: 'VADER 5 PRO', a0Replies: 2, a0Status: 0, a0Field: 32};
  device.receiveFeatureReport = async function () {
    if (this.sends.at(-1).bytes[0] === 8) return gcfgResponse(state, this.sends.at(-1).bytes);
    return receive.call(this);
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.equal(element('gcfgRead').disabled, false);
  await element('gcfgRead').onclick();
  assert.equal(device.sends.at(-1).bytes[0], 8);
  assert.match(element('status').textContent, /已读取设备端全局配置/);
  assert.match(element('gcfgStatus').textContent, /回报率 500 Hz/);
  assert.match(element('gcfgStatus').textContent, /睡眠 15 分钟/);
  assert.match(element('gcfgStatus').textContent, /第三方接管 允许/);
  assert.match(element('gcfgStatus').textContent, /摇杆分辨率 12/);
  assert.match(element('gcfgStatus').textContent, /名称 VADER 5 PRO/);
  assert.match(element('gcfgStatus').textContent, /读取 3 次 \/ 错误 1 次/);
  assert.match(element('gcfgStatus').textContent, /最近写入：睡眠成功/);
  assert.equal(element('gcfgSleep').value, '15');
  assert.equal(element('gcfgRate').value, '500');
  /* flags bit0=0：尚未成功读取过，展示计数并提示稍后重试。 */
  state.valid = false; state.reads = 0; state.errors = 2; state.a0Replies = 0;
  await element('gcfgRead').onclick();
  assert.match(element('gcfgStatus').textContent, /尚未成功读取/);
  assert.match(element('gcfgStatus').textContent, /读取 0 次 \/ 错误 2 次/);
  assert.match(element('gcfgStatus').textContent, /已排队新读取/);
  assert.match(element('status').textContent, /尚未就绪/);
}

async function testGcfgWriteSync() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  const common = {name: 'PAD', jsResolution: 10, thirdparty: false, errors: 0};
  /* 三次 GET_GCFG：#1 基线，#2 读取计数未增（旧值），#3 计数增加（新值）。 */
  const readsQueue = [
    {reads: 5, sleep: 10, rate: 1000, a0Replies: 0, a0Field: 0, a0Status: 0, valid: true},
    {reads: 5, sleep: 10, rate: 1000, a0Replies: 0, a0Field: 0, a0Status: 0, valid: true},
    {reads: 6, sleep: 30, rate: 1000, a0Replies: 1, a0Field: 32, a0Status: 0, valid: true},
    {reads: 6, sleep: 30, rate: 1000, a0Replies: 1, a0Field: 32, a0Status: 0, valid: true},
    {reads: 7, sleep: 30, rate: 250, a0Replies: 2, a0Field: 33, a0Status: 0, valid: true},
  ];
  let readIndex = 0;
  device.receiveFeatureReport = async function () {
    const request = this.sends.at(-1).bytes;
    if (request[0] === 8)
      return gcfgResponse(Object.assign({}, common,
        readsQueue[Math.min(readIndex++, readsQueue.length - 1)]), request);
    if (request[0] === 9 || request[0] === 10) return gcfgEcho(request);
    return receive.call(this);
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  element('gcfgSleep').value = '30';
  const applying = element('gcfgSleepApply').onclick();
  await new Promise(r => setTimeout(r, 1));
  assert.equal(element('gcfgSleepApply').disabled, true);   /* 回读同步期间禁用写入 */
  assert.equal(element('gcfgSleep').disabled, true);
  assert.equal(element('gcfgRead').disabled, true);
  await applying;
  assert.deepEqual(Array.from(device.sends.find(s => s.bytes[0] === 9).bytes.slice(0, 2)), [9, 30]);
  assert.match(element('gcfgStatus').textContent, /睡眠 30 分钟/);
  assert.match(element('gcfgStatus').textContent, /最近写入：睡眠成功/);
  assert.equal(element('gcfgSleep').value, '30');
  assert.equal(element('gcfgSleepApply').disabled, false);
  /* SET_RATE：Hz u16LE 编码 + 回读后展示新值。 */
  element('gcfgRate').value = '250';
  await element('gcfgRateApply').onclick();
  const rateSend = device.sends.find(s => s.bytes[0] === 10).bytes;
  assert.deepEqual(Array.from(rateSend.slice(0, 3)), [10, 250, 0]);
  assert.equal(rateSend[1] | rateSend[2] << 8, 250);
  assert.match(element('gcfgStatus').textContent, /回报率 250 Hz/);
  assert.equal(element('gcfgRate').value, '250');
  assert.match(element('status').textContent, /回报率已写入：250 Hz/);
}

async function testGcfgDisabledState() {
  const {element, hid, disconnect} = setup();
  assert.equal(element('gcfgRead').disabled, true);         /* 未连接：全部禁用 */
  assert.equal(element('gcfgSleepApply').disabled, true);
  assert.equal(element('gcfgRateApply').disabled, true);
  assert.equal(element('gcfgSleep').disabled, true);
  assert.equal(element('gcfgRate').disabled, true);
  const device = fakeDevice();
  hid.nextDevice = device;
  await element('connect').onclick();
  assert.equal(element('gcfgRead').disabled, false);
  assert.equal(element('gcfgSleepApply').disabled, false);
  assert.equal(element('gcfgRateApply').disabled, false);
  assert.equal(element('gcfgSleep').disabled, false);
  assert.equal(element('gcfgRate').disabled, false);
  disconnect(device);
  assert.equal(element('gcfgRead').disabled, true);
  assert.equal(element('gcfgSleepApply').disabled, true);
  assert.equal(element('gcfgRateApply').disabled, true);
  assert.equal(element('gcfgSleep').disabled, true);
  assert.equal(element('gcfgRate').disabled, true);
  assert.equal(element('gcfgStatus').textContent, '设备端配置: -');
}

/* ---------- 二次核对新增：设备信息 / 指纹 / 校准 / 切槽 / 分辨率 / 名称 ---------- */

function infoResponse(request) {
  const bytes = new Uint8Array(64);
  bytes[0] = 0x11; bytes[2] = 1;
  bytes[3] = 130;   /* [2] model */
  bytes[4] = 1;     /* [3] conn */
  bytes[5] = 100;   /* [4] battery */
  bytes[6] = 0x45;  /* [5] chips */
  bytes[7] = 1;     /* [6] gyro */
  bytes[8] = 0x27;  /* [7] features */
  [0x7226, 0x0483, 0x3605, 0, 0, 0, 0x1043].forEach((v, i) => {
    bytes[9 + 2 * i] = v >> 8;         /* [8..21] 7×fw u16BE */
    bytes[10 + 2 * i] = v & 0xff;
  });
  const sku = new TextEncoder().encode('F5');
  for (let i = 0; i < 20; i++) bytes[23 + i] = sku[i] || 0;   /* [22..41] */
  bytes[43] = 1;    /* [42] skuValid */
  bytes[44] = 1;    /* [43] exValid */
  bytes[62] = request[61]; bytes[63] = request[62];
  return new DataView(bytes.buffer);
}

function fpResponse(request) {
  const bytes = new Uint8Array(64);
  const view = new DataView(bytes.buffer);
  bytes[0] = 0x11; bytes[2] = 1;
  bytes[3] = 1;                                    /* [2] valid */
  bytes[4] = 1;                                    /* [3] slot */
  view.setUint16(5, 0xdad9, true);                 /* [4..5] 槽1 CRC16 */
  view.setUint16(7, 0xcad9, true);                 /* [6..7] 槽2 */
  view.setUint16(9, 0, true);                      /* [8..9] 槽3 */
  view.setUint16(11, 0, true);                     /* [10..11] 槽4 */
  view.setUint16(13, 0xc0c0, true);                /* [12..13] 全局 */
  view.setUint32(15, 3, true);                     /* [14..17] 推送计数 */
  bytes[62] = request[61]; bytes[63] = request[62];
  return new DataView(bytes.buffer);
}

async function testDeviceInfoPanel() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async function () {
    const request = this.sends.at(-1).bytes;
    if (request[0] === 15) return fpResponse(request);
    if (request[0] === 11) return infoResponse(request);
    return receive.call(this);
  };
  hid.nextDevice = device;
  await element('connect').onclick();
  await element('gcfgFp').onclick();
  assert.match(element('devInfo').textContent, /SKU F5/);
  assert.match(element('devInfo').textContent, /固件 主7\.2\.2\.6/);
  assert.match(element('devInfo').textContent, /能力 IMU\/LED\/Switch\/扳机震动/);
  assert.match(element('fpStatus').textContent, /活动槽 2/);
  assert.match(element('fpStatus').textContent, /dad9\/cad9\/0000\/0000/);
  assert.match(element('fpStatus').textContent, /变更推送 3 次/);
}

async function testExtrasEncoding() {
  const {element, hid} = setup();
  const device = fakeDevice();
  const receive = device.receiveFeatureReport;
  const state = {rate: 1000, sleep: 0, valid: true, thirdparty: true, jsResolution: 10,
    reads: 4, errors: 0, name: ''};
  let reads = 4;
  device.receiveFeatureReport = async function () {
    const request = this.sends.at(-1).bytes;
    if (request[0] === 8) { reads++; return gcfgResponse(Object.assign({}, state, {reads}), request); }
    if (request[0] === 15) return fpResponse(request);
    if (request[0] === 12 || request[0] === 13 || request[0] === 14 || request[0] === 18) {
      const b = new Uint8Array(64); b[0] = 0x11; b[2] = 1;
      if (request[0] === 12) b[3] = request[1];
      if (request[0] === 14) b[3] = request[1];
      if (request[0] === 18) b[3] = 3;
      b[62] = request[61]; b[63] = request[62];
      return new DataView(b.buffer);
    }
    return receive.call(this);
  };
  hid.nextDevice = device;
  await element('connect').onclick();

  /* 摇杆分辨率：SET_FIELD [1]=34 [2]=12 [3]=0 */
  element('gcfgJsres').value = '12';
  await element('gcfgJsresApply').onclick();
  const fieldSend = device.sends.filter(s => s.bytes[0] === 12).at(-1).bytes;
  assert.equal(fieldSend[1], 34);
  assert.equal(fieldSend[2], 12);
  assert.equal(fieldSend[3], 0);
  assert.match(element('status').textContent, /摇杆分辨率已写入：12 bit/);

  /* 校准：开始 [1]=1 / 结束 [1]=0 */
  await element('calibStart').onclick();
  assert.equal(device.sends.filter(s => s.bytes[0] === 14).at(-1).bytes[1], 1);
  assert.match(element('status').textContent, /校准已开始/);
  await element('calibFinish').onclick();
  assert.equal(device.sends.filter(s => s.bytes[0] === 14).at(-1).bytes[1], 0);
  assert.match(element('status').textContent, /校准已结束/);

  /* 切槽：SLOT_SET [1]=2 → 指纹刷新显示活动槽 2 */
  await element('slotBtn2').onclick();
  const slotSend = device.sends.filter(s => s.bytes[0] === 13).at(-1).bytes;
  assert.equal(slotSend[1], 2);
  assert.match(element('status').textContent, /已切换到配置槽 3/);
  assert.match(element('fpStatus').textContent, /活动槽 2/);

  /* 名称：SET_NAME 载荷 [1..3]='BRG' */
  element('gcfgName').value = 'BRG';
  await element('gcfgNameApply').onclick();
  const nameSend = device.sends.filter(s => s.bytes[0] === 18).at(-1).bytes;
  assert.deepEqual(Array.from(nameSend.slice(1, 4)), [0x42, 0x52, 0x47]);
  assert.match(element('status').textContent, /名称已写入：BRG/);
}

async function testExtrasDisabledState() {
  const {element} = setup();
  for (const id of ['gcfgFp', 'gcfgJsresApply', 'gcfgNameApply',
                    'slotBtn0', 'slotBtn3', 'calibStart', 'calibFinish'])
    assert.equal(element(id).disabled, true);
}

async function testDisconnect() {
  const {element, hid, disconnect} = setup();
  const device = fakeDevice();
  device.pending = deferred();
  hid.nextDevice = device;
  const connecting = element('connect').onclick();
  await new Promise(r => setTimeout(r, 25));
  disconnect(device);
  device.opened = false;
  device.pending.resolve(new DataView(new Uint8Array(64).buffer));
  await connecting;
  assert.equal(element('status').textContent, '设备已断开');
  assert.equal(element('apply').disabled, true);
  hid.nextDevice = fakeDevice();
  await element('connect').onclick();
  assert.match(element('status').textContent, /已读取配置/);
  assert.equal(element('apply').disabled, false);
}

(async () => {
  await testMismatch();
  await testStadiaConfigFilter();
  await testLegacyResponse();
  await testUnknownBattery();
  await testRadialFlags();
  await testIndependentRumble();
  await testStopDuringBusyCommand();
  await testBridgeAgeTelemetry();
  await testLiveRawButtons();
  await testAppleExtendedMode(1, 26);
  await testAppleExtendedMode(2, 29);
  await testPersistentAppleProfile();
  await testGcfgReadDisplay();
  await testGcfgWriteSync();
  await testGcfgDisabledState();
  await testDeviceInfoPanel();
  await testExtrasEncoding();
  await testExtrasDisabledState();
  await testDisconnect();
  console.log('bridge-config: token, legacy, radial flags, rumble, telemetry, gcfg read/write sync, Apple persistence, reconnect PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
