const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync(`${__dirname}/gamepad-probe.html`, 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, 'probe script exists');

function setup(pads = [], device = null, hidSupported = true) {
  const elements = new Map();
  const frameCallbacks = [];
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      value: '', disabled: false, textContent: '', children: [],
      replaceChildren() { this.children = []; },
      append(child) { this.children.push(child); },
    });
    return elements.get(id);
  };
  const document = {
    querySelector: selector => element(selector.slice(1)),
    createElement: () => ({value: '', textContent: ''}),
  };
  vm.runInNewContext(script, {
    document, navigator: {
      getGamepads: () => pads,
      hid: hidSupported ? {
        getDevices: async () => device ? [device] : [],
        requestDevice: async () => device ? [device] : [],
      } : undefined,
    },
    crypto: {getRandomValues: array => { array[0] = 100; return array; }},
    requestAnimationFrame: callback => { frameCallbacks.push(callback); },
    setTimeout: callback => { queueMicrotask(callback); },
    addEventListener() {},
  });
  return {
    element,
    frame: timestamp => frameCallbacks.shift()?.(timestamp),
    snapshot: () => JSON.parse(element('out').textContent),
    events: () => JSON.parse(element('out').textContent).log,
  };
}

function fakeDevice() {
  return {
    vendorId: 0x1209, productId: 1, productName: 'Flydigi Bridge', opened: false,
    collections: [{usagePage: 0xff00, usage: 1}], sends: [],
    async open() { this.opened = true; },
    async sendFeatureReport(id, bytes) {
      this.sends.push({id, bytes: Uint8Array.from(bytes)});
      if (this.failStartSend && this.sends.length === 1) throw new Error('start transfer failed');
    },
    async receiveFeatureReport() {
      const request = this.sends.at(-1).bytes;
      const reply = new Uint8Array(64);
      reply[0] = 0x11;
      reply[1] = this.rejectStart && request[1] !== 0 ? 2 : 0;
      reply[62] = request[61];
      reply[63] = request[62];
      return new DataView(reply.buffer);
    },
  };
}

async function testSelectedGamepad() {
  const calls = [[], []];
  const pads = calls.map((list, index) => ({
    index, id: `pad ${index}`, connected: true, mapping: '', buttons: [], axes: [],
    vibrationActuator: {effects: ['dual-rumble'], playEffect: async (...args) => {
      list.push(args);
      return index ? 'preempted' : 'complete';
    }},
  }));
  const {element, events} = setup(pads);
  assert.equal(element('pad').value, '', 'multiple gamepads require explicit selection');
  await element('pulse').onclick();
  assert.equal(calls[0].length + calls[1].length, 0);
  element('pad').value = '1';
  await element('pulse').onclick();
  assert.equal(calls[0].length, 0);
  assert.equal(calls[1].length, 1);
  assert.equal(events().at(-1).result, 'preempted');
}

function testFrameSampling() {
  const button = {pressed: false, value: 0};
  const pad = {index: 0, id: 'pad 0', connected: true, mapping: '',
    buttons: [button], axes: [0], vibrationActuator: null};
  const {frame, snapshot, events} = setup([pad]);
  button.pressed = true;
  button.value = 0.5;
  frame(64);
  assert.deepEqual(snapshot().pads[0].activeButtonValues, [{index: 0, value: 0.5}]);
  button.pressed = false;
  button.value = 0;
  frame(80);
  assert.equal(snapshot().pads[0].activeButtonValues.length, 1,
    'sampling a release does not force a JSON repaint');
  frame(128);
  assert.deepEqual(events().filter(event => event.event.startsWith('button-'))
    .map(event => event.event), ['button-down', 'button-up']);
}

async function testWithoutWebHID() {
  const {element} = setup([], null, false);
  assert.equal(element('hidPulse').disabled, true);
  await element('hidPulse').onclick();
  assert.equal(element('hidPulse').disabled, true);
}

async function testWebHIDStop(startFailure, stadia = false) {
  const device = fakeDevice();
  if (stadia) { device.vendorId = 0x18d1; device.productId = 0x9400; }
  if (startFailure === 'send') device.failStartSend = true;
  if (startFailure === 'reject') device.rejectStart = true;
  const {element, events} = setup([], device);
  const pulse = element('hidPulse');
  const first = pulse.onclick();
  assert.equal(pulse.disabled, true);
  await pulse.onclick();
  await first;
  assert.equal(pulse.disabled, false);
  assert.deepEqual(device.sends.map(send => [...send.bytes.slice(0, 3)]),
    [[7, 180, 180], [7, 0, 0]]);
  assert.deepEqual(device.sends.map(send => send.id), [0x10, 0x10]);
  assert.ok(events().some(event => event.result === 'stop accepted'));
  if (startFailure) assert.ok(events().some(event =>
    event.result.includes(startFailure === 'send' ? 'start transfer failed' : 'bridge rejected')));
  else assert.ok(events().some(event => event.result === 'start accepted'));
}

(async () => {
  testFrameSampling();
  await testWithoutWebHID();
  await testSelectedGamepad();
  await testWebHIDStop(null);
  await testWebHIDStop(null, true);
  await testWebHIDStop('send');
  await testWebHIDStop('reject');
  console.log('gamepad probe: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
