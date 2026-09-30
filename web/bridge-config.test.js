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
  await testDisconnect();
  console.log('bridge-config: token, legacy, radial flags, rumble, telemetry, Apple persistence, reconnect PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
