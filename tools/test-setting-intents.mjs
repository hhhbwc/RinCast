import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import vm from 'node:vm';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ui = resolve(repo, 'pc/streamer/ui');
const current = readFileSync(resolve(ui, 'app.js'), 'utf8');
const i18n = readFileSync(resolve(ui, 'i18n.js'), 'utf8');
const keys = ['streamProfile', 'codec', 'tenBit', 'vrBitrateMbps', 'pcBitrateWins', 'desktopBitrateMbps', 'adaptiveBitrate', 'videoPacingMode', 'qualityProfileCbr', 'profileControlsVideo', 'fecPercent', 'foveatedEncoding', 'adaptiveQuantization', 'twoPassEncoding', 'desktopMaxHeight', 'desktopFps'];
const bools = new Set(['tenBit', 'pcBitrateWins', 'adaptiveBitrate', 'qualityProfileCbr', 'profileControlsVideo', 'foveatedEncoding', 'adaptiveQuantization', 'twoPassEncoding']);
const strings = new Set(['codec']);
const initial = Object.fromEntries(keys.map(k => [k, bools.has(k) ? true : strings.has(k) ? 'auto' : 1]));
initial.pcName = 'fixture';
initial._revision = 1;

class Element {
  constructor(key = '') {
    this.dataset = { setting: key };
    this.type = bools.has(key) ? 'checkbox' : strings.has(key) ? 'radio' : 'range';
    this.checked = true;
    this.value = '1';
    this.min = '0'; this.max = '500';
    this.style = { setProperty() {} };
    this.classList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
    this.events = new Map();
    this.parentElement = this;
  }
  querySelector() { return this; }
  querySelectorAll() { return []; }
  addEventListener(name, fn) { this.events.set(name, fn); }
  setAttribute() {}
  removeAttribute() {}
  replaceChildren() {}
  closest() { return this; }
  after(el) { this.nextElementSibling = el; }
  getContext() { return {}; }
}

async function fixture(source = current) {
  const sent = [], timers = new Map(), elements = new Map(keys.map(k => [k, new Element(k)]));
  const other = new Map();
  let timerId = 0, revision = 1;
  let server = { ...initial };
  const dom = {
    activeElement: null,
    querySelectorAll: s => s === '[data-setting]' ? [...elements.values()] : [],
    querySelector: s => { if (!other.has(s)) other.set(s, new Element()); return other.get(s); },
    createElement: () => new Element(),
    documentElement: new Element(), body: new Element(),
  };
  const context = vm.createContext({
    console, document: dom,
    window: { chrome: { webview: { postMessage(s) { sent.push(JSON.parse(s)); }, addEventListener() {} } }, addEventListener() {} },
    navigator: { language: 'en-US', languages: ['en-US'] },
    localStorage: { getItem() { return null; }, setItem() {}, removeItem() {} },
    location: { search: '' }, URLSearchParams, Intl, Date, structuredClone,
    Option: class {},
    setTimeout(fn, ms) { const id = ++timerId; timers.set(id, { fn, ms }); return id; },
    clearTimeout(id) { timers.delete(id); }, setInterval() { return 0; }, clearInterval() {},
  });
  const exports = `
export { commitSetting, receive, wireSetting };
const originalDependents = renderDependents;
export function testRenderDependents(settings, session) {
  state = { settings, session, system: { encoder: { backend: 'nvenc', hevc: true }, audioDriver: {} } };
  originalDependents();
}
export function testEarlyPushes() {
  state = null;
  receive({ type: 'stats', session: null });
  receive({ type: 'firewall', firewall: {} });
  receive({ type: 'vrState', state: 'idle' });
}
export function testInit(settings) {
  state = { settings: structuredClone(settings) };
  if (typeof confirmedSettings !== 'undefined') { confirmedSettings = state.settings; settingsRevision = settings._revision; }
  renderAll = () => bindSettings(); renderDependents = () => {}; renderAudio = () => {}; drawSpark = () => {};
}
export function testView() { return structuredClone(state.settings); }
`;
  const module = new vm.SourceTextModule(source + exports, { context, identifier: 'app.js' });
  const language = new vm.SourceTextModule(i18n, { context, identifier: 'i18n.js' });
  await module.link(specifier => { assert.equal(specifier, './i18n.js'); return language; });
  await module.evaluate();
  const api = module.namespace;
  api.testInit(initial);
  sent.length = 0;
  const changes = () => sent.filter(m => m.type === 'setSettings');
  function click(key, value) {
    const el = elements.get(key);
    if (el.type === 'checkbox') el.checked = value;
    else el.value = String(value);
    api.commitSetting(el);
  }
  function apply(message) {
    assert.deepEqual(Object.keys(message).sort(), ['requestId', 'settings', 'type']);
    assert.equal(Object.keys(message.settings).length, 1);
    server = { ...server, ...message.settings, _revision: ++revision };
    return { type: 'reply', requestId: message.requestId, ok: true, settings: structuredClone(server) };
  }
  async function deliver(message) { api.receive(structuredClone(message)); await Promise.resolve(); await Promise.resolve(); }
  async function state(settings) {
    server = { ...server, ...settings, _revision: ++revision };
    await deliver({ type: 'state', settings: server });
  }
  return { api, elements, changes, click, apply, deliver, state, timers, other };
}

const results = [];
const baseline = execFileSync('git', ['-C', repo, 'show', '636f9a4:pc/streamer/ui/app.js'], { encoding: 'utf8' });
const old = await fixture(baseline);
for (let i = 0; i < 20; i++) old.click('adaptiveBitrate', !!(i % 2));
assert.equal(old.changes().length, 20);
results.push({ test: 'baseline_same_key_inflight', requests: 20 });

const serial = await fixture();
serial.click('adaptiveBitrate', false);
for (let i = 0; i < 20; i++) serial.click('adaptiveBitrate', !!(i % 2));
assert.equal(serial.changes().length, 1);
await serial.state({ adaptiveBitrate: false });
assert.equal(serial.api.testView().adaptiveBitrate, true);
await serial.deliver(serial.apply(serial.changes()[0]));
assert.equal(serial.changes().length, 2);
assert.equal(serial.changes()[1].settings.adaptiveBitrate, true);
await serial.deliver(serial.apply(serial.changes()[1]));
assert.equal(serial.api.testView().adaptiveBitrate, true);
results.push({ test: '21_clicks_last_intent', requestsWhileInflight: 1, totalRequests: 2 });

const order = await fixture();
order.click('tenBit', false);
const early = order.apply(order.changes()[0]);
order.click('adaptiveBitrate', false);
const late = order.apply(order.changes()[1]);
await order.deliver(late);
await order.deliver(early);
assert.equal(order.api.testView().adaptiveBitrate, false);
await order.deliver({ type: 'state', settings: { ...initial, _revision: 1 } });
assert.equal(order.api.testView().adaptiveBitrate, false);
results.push({ test: 'cross_key_reply_and_state_reordering', status: 'PASS' });

const returnedIntent = await fixture();
returnedIntent.click('adaptiveBitrate', false);
const oldReply = returnedIntent.apply(returnedIntent.changes()[0]);
await returnedIntent.state({ adaptiveBitrate: true });
returnedIntent.click('adaptiveBitrate', true);
returnedIntent.click('adaptiveBitrate', false);
await returnedIntent.deliver(oldReply);
assert.equal(returnedIntent.changes().length, 2, 'a newer A-to-B-to-A intent must survive an obsolete A reply');
await returnedIntent.deliver(returnedIntent.apply(returnedIntent.changes()[1]));
assert.equal(returnedIntent.api.testView().adaptiveBitrate, false);
results.push({ test: 'same_value_new_intent_after_newer_snapshot', requests: 2, finalValue: false });

const timeout = await fixture();
timeout.click('adaptiveBitrate', false);
for (const timer of [...timeout.timers.values()]) if (timer.ms === 30000) timer.fn();
timeout.click('adaptiveBitrate', true);
assert.equal(timeout.changes().length, 1);
await timeout.deliver(timeout.apply(timeout.changes()[0]));
assert.equal(timeout.changes().length, 2);
await timeout.deliver(timeout.apply(timeout.changes()[1]));
assert.equal(timeout.api.testView().adaptiveBitrate, true);
results.push({ test: 'timeout_does_not_cancel_native_request', status: 'PASS' });

const fail = await fixture();
fail.click('tenBit', false);
await fail.deliver({ type: 'reply', requestId: fail.changes()[0].requestId, ok: false, error: 'fixture failure' });
assert.equal(fail.api.testView().tenBit, true);
fail.click('tenBit', false);
await fail.deliver(fail.apply(fail.changes()[1]));
assert.equal(fail.api.testView().tenBit, false);
results.push({ test: 'failure_rollback_and_next_edit', status: 'PASS' });

for (const key of keys) {
  const f = await fixture();
  const value = bools.has(key) ? false : strings.has(key) ? 'hevc' : 2;
  for (let i = 0; i < 10; i++) f.click(key, value);
  assert.equal(f.changes().length, 1, key);
  await f.deliver(f.apply(f.changes()[0]));
  assert.equal(f.api.testView()[key], value, key);
  results.push({ test: `request_path_${key}`, operations: 10, requests: 1 });
}
const range = await fixture();
const slider = range.elements.get('vrBitrateMbps');
range.api.wireSetting(slider);
for (let i = 0; i < 100; i++) { slider.value = String(i); slider.events.get('input')(); }
assert.equal(range.changes().length, 0);
await slider.events.get('change')();
assert.equal(range.changes().length, 1);
results.push({ test: 'range_input_label_only_change_commits', status: 'PASS' });

const regression = await fixture();
for (const [pcWins, headset, disabled] of [[true, 123, false], [false, 123, true], [true, 0, false]]) {
  regression.api.testRenderDependents({ ...initial, pcBitrateWins: pcWins, vrBitrateMbps: 100 }, { headsetBitrateMbps: headset });
  assert.equal(regression.other.get('#vrBitrateBar').disabled, disabled);
  assert.equal(regression.other.get('#vrBitrateBar output').textContent, `${disabled ? headset : 100} Mbps`);
  assert.equal(regression.other.get('#overrideNote').textContent.includes('123'), disabled);
}
results.push({ test: 'round7_pc_bitrate_wins', cases: 3 });
for (const [profile, controls, match, slow, disabled] of [[2, true, false, false, false], [2, true, true, false, true], [2, true, false, true, true], [1, true, false, false, true], [2, false, false, false, true]]) {
  regression.api.testRenderDependents({ ...initial, streamProfile: profile, profileControlsVideo: controls }, { matchSteamVr: match, slowPath: slow });
  assert.equal(regression.other.get('#cbrRow').disabled, disabled);
}
results.push({ test: 'round7_cbr_gates', cases: 5 });
regression.api.testEarlyPushes();
results.push({ test: 'round7_pushes_before_initial_state', status: 'PASS' });

const output = { status: 'PASS', evidence: 'actual app.js/i18n.js module import; DOM and native bridge stubbed, no browser/video verification', results };
if (process.argv[2]) writeFileSync(resolve(process.argv[2]), JSON.stringify(output, null, 2));
console.log(JSON.stringify(output, null, 2));
