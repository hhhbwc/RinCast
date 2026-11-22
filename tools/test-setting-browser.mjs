import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { once } from 'node:events';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ui = join(repo, 'pc/streamer/ui');
const out = resolve(process.argv[2] || join(repo, 'build'));
assert(existsSync(out), 'Output directory must already exist');
const browserExe = process.env.RINCAST_TEST_BROWSER || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const baseline = execFileSync('git', ['-C', repo, 'show', '636f9a4:pc/streamer/ui/app.js']);
let useBaseline = false;
const allowed = new Map([['/', ['index.html', 'text/html']], ['/index.html', ['index.html', 'text/html']], ['/app.js', ['app.js', 'text/javascript']], ['/i18n.js', ['i18n.js', 'text/javascript']], ['/app.css', ['app.css', 'text/css']]]);
const server = createServer((req, res) => {
  const entry = allowed.get(new URL(req.url, 'http://127.0.0.1').pathname);
  if (!entry) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': entry[1], 'Cache-Control': 'no-store' });
  res.end(useBaseline && entry[0] === 'app.js' ? baseline : readFileSync(join(ui, entry[0])));
});
const results = [], errors = [];
let browser, ws, profile;
const report = { status: 'FAIL', evidence: 'Real Chrome, unchanged production HTML/CSS/JS, CDP mouse/keyboard input; native bridge and stream state are fixtures, no native/video/device acceptance', results, errors };
const wait = ms => new Promise(r => setTimeout(r, ms));

function bridgeFixture() {
  const clone = x => structuredClone(x);
  Object.defineProperty(navigator, 'language', { get: () => 'zh-CN' });
  Object.defineProperty(navigator, 'languages', { get: () => ['zh-CN'] });
  const base = {
    type: 'state', version: 'ROUND8 browser fixture', downloadUrl: '', downloadSite: '',
    settings: { _revision: 1, pcName: 'Browser fixture', codec: 'auto', tenBit: false, vrBitrateMbps: 200, desktopBitrateMbps: 40, adaptiveBitrate: true, streamProfile: 2, profileControlsVideo: true, qualityProfileCbr: false, pcBitrateWins: true, fecPercent: 5, foveatedEncoding: false, adaptiveQuantization: true, twoPassEncoding: false, desktopMaxHeight: 0, streamAudio: true, playOnPcToo: false, micPassthrough: true, switchDefaultAudio: true, audioDevices: 'auto', autoRegisterSteamVrDriver: true, requireApproval: true, desktopInput: true, startWithWindows: false, preventSleep: true, cloudUrl: '', trustedDevices: [] },
    cloud: { signedIn: false },
    system: { gpu: 'NVIDIA test fixture', port: 38711, addresses: [], vpnAddresses: [], firewall: { otherNetworksOpen: true }, encoder: { backend: 'nvenc', h264: true, hevc: true, av1: true, hevc10: true }, steamvr: { installed: true, running: false, driverRegistered: true }, openxr: { rincastAvailable: false, rincastActive: false, steamvrActive: true }, audioDriver: { speaker: false, microphone: false, steam: { speaker: true, microphone: true }, active: { speaker: 'steam', microphone: 'steam' } } },
    session: { deviceName: 'Fixture headset', model: 'Fixture', address: '127.0.0.1', connectedSeconds: 60, rttMs: 3, vrState: 'idle', headsetBitrateMbps: 0, headsetCodec: 'auto', desktop: [], client: {} },
    pendingApproval: null,
  };
  const listeners = [], sent = [], pageErrors = [];
  addEventListener('error', e => pageErrors.push(String(e.error || e.message)));
  addEventListener('unhandledrejection', e => pageErrors.push(String(e.reason)));
  const b = window.__bridge = {
    base, sent, pageErrors, auto: true, frames: 0, early: false,
    changes: () => sent.filter(m => m.type === 'setSettings'),
    emit(m) { listeners.forEach(fn => fn({ data: clone(m) })); },
    apply(i) {
      const m = b.changes()[i];
      if (!m || Object.keys(m.settings).length !== 1 || Object.keys(m).sort().join() !== 'requestId,settings,type') throw Error('Invalid settings envelope');
      Object.assign(base.settings, m.settings);
      base.settings._revision++;
      return { type: 'reply', requestId: m.requestId, ok: true, settings: clone(base.settings) };
    },
    ack(i) { b.emit(b.apply(i)); },
    state(settings = {}, session = {}) {
      Object.assign(base.settings, settings);
      Object.assign(base.session, session);
      base.settings._revision++;
      b.emit(base);
    },
  };
  const frame = () => { b.frames++; requestAnimationFrame(frame); };
  requestAnimationFrame(frame);
  window.chrome ||= {};
  window.chrome.webview = {
    addEventListener(event, fn) { if (event === 'message') listeners.push(fn); },
    postMessage(text) {
      const m = JSON.parse(text);
      sent.push(m);
      if (m.type === 'getState') {
        if (!b.early) {
          b.early = true;
          b.emit({ type: 'stats', session: null });
          b.emit({ type: 'firewall', firewall: {} });
          b.emit({ type: 'vrState', state: 'idle' });
        }
        setTimeout(() => b.emit({ ...base, requestId: m.requestId }), 10);
      } else if (m.type === 'setSettings' && b.auto) {
        const i = b.changes().length - 1;
        setTimeout(() => b.ack(i), 30);
      }
    },
  };
}

try {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const url = `http://127.0.0.1:${server.address().port}/`;
  profile = mkdtempSync(join(tmpdir(), 'rincast-round8-browser-'));
  browser = spawn(browserExe, ['--headless=new', '--remote-debugging-port=0', '--remote-debugging-address=127.0.0.1', `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
  let startup = '';
  const wsUrl = await new Promise((ok, fail) => {
    const timer = setTimeout(() => fail(Error('Browser startup timed out')), 20000);
    browser.once('error', e => { clearTimeout(timer); fail(e); });
    browser.stderr.on('data', d => {
      startup += d.toString();
      const match = startup.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) { clearTimeout(timer); ok(match[1]); }
    });
  });
  ws = new WebSocket(wsUrl);
  await once(ws, 'open');
  let nextId = 0;
  const pending = new Map();
  ws.addEventListener('message', e => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) {
      const p = pending.get(m.id); pending.delete(m.id); clearTimeout(p.timer);
      m.error ? p.fail(Error(m.error.message)) : p.ok(m.result);
    } else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map(a => a.value || a.description).join(' '));
  });
  const send = (method, params = {}, sessionId) => new Promise((ok, fail) => {
    const id = ++nextId;
    const timer = setTimeout(() => { pending.delete(id); fail(Error(`CDP timeout: ${method}`)); }, 45000);
    pending.set(id, { ok, fail, timer }); ws.send(JSON.stringify({ id, method, params, sessionId }));
  });
  report.browser = await send('Browser.getVersion');
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  const page = (method, params = {}) => send(method, params, sessionId);
  await page('Page.enable'); await page('Runtime.enable');
  await page('Page.addScriptToEvaluateOnNewDocument', { source: `(${bridgeFixture.toString()})()` });
  const js = async expression => {
    const r = await page('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result?.value;
  };
  const check = (name, detail) => { results.push({ test: name, status: 'PASS', ...detail }); console.log(name, JSON.stringify(detail)); };
  async function click(selector) {
    const point = await js(`(() => {
      const el = document.querySelector(${JSON.stringify(selector)});
      if (!el || el.disabled) throw Error('Missing/disabled: ' + ${JSON.stringify(selector)});
      const target = el.matches('input[type=checkbox],input[type=radio]') ? el.closest('label') : el;
      target.scrollIntoView({block:'center'});
      const r = target.getBoundingClientRect(), x = r.x+r.width/2, y = r.y+r.height/2;
      const hit = document.elementFromPoint(x,y);
      if (r.width < 2 || r.height < 2 || !target.contains(hit)) throw Error('Not visible/hit-testable: ' + ${JSON.stringify(selector)});
      return {x,y};
    })()`);
    await page('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', clickCount: 1, ...point });
    await page('Input.dispatchMouseEvent', { type: 'mouseReleased', button: 'left', clickCount: 1, ...point });
  }
  const selector = key => `[data-setting="${key}"]`;
  const value = key => js(`(() => {const a=[...document.querySelectorAll(${JSON.stringify(selector(key))})];const el=a.find(x=>x.type!=='radio'||x.checked);return el.type==='checkbox'?el.checked:el.type==='range'||/^\\d+$/.test(el.value)?Number(el.value):el.value;})()`);
  // Progressive disclosure (A48): the streaming page has two views over the SAME DOM.
  // Driving the mode with real clicks keeps every later per-setting test deterministic
  // no matter what a previous run left in localStorage.
  async function setMode(mode) {
    await click(`#videoPage [data-set-mode="${mode}"]`);
    await wait(40);
    assert.equal(await js(`document.querySelector('#videoPage').dataset.mode`), mode);
  }
  async function cardReport() {
    return js(`(() => {
      const page = document.querySelector('#videoPage');
      const cards = [...page.querySelectorAll('.card')];
      const visible = cards.filter(c => c.getBoundingClientRect().height > 0);
      const titleOf = c => (c.querySelector('h2')?.innerText || '').trim();
      return {
        mode: page.dataset.mode,
        cards: cards.length,
        visible: visible.length,
        // A visible title with no visible control under it is the "empty shell" bug.
        danglingTitles: visible.filter(c => !titleOf(c)).length,
        emptyCards: visible.filter(c => ![...c.querySelectorAll('.row')].some(r => r.getBoundingClientRect().height > 0)).length,
        expertShown: page.querySelector('.expert-only').getBoundingClientRect().height > 0,
        keys: new Set([...page.querySelectorAll('[data-setting]')].map(e => e.dataset.setting)).size,
      };
    })()`);
  }
  async function fresh(old = false, width = 1100, height = 1000, mode = 'expert') {
    useBaseline = old;
    await page('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
    await page('Page.navigate', { url });
    await js(`new Promise((ok,fail)=>{const end=Date.now()+5000;const timer=setInterval(()=>{if(document.querySelector('#pcName')?.value==='Browser fixture'){clearInterval(timer);ok(true);}else if(Date.now()>end){clearInterval(timer);fail(Error('UI did not initialize'));}},20);})`);
    await click('#nav [data-page="video"]');
    await wait(60);
    assert.equal(await js(`document.querySelector('.page.active').dataset.page`), 'video');
    assert.deepEqual(await js('__bridge.pageErrors'), []);
    // The baseline app.js predates the mode switcher, so only the candidate gets driven.
    if (!old && mode) await setMode(mode);
  }
  async function shot(name) {
    const { data } = await page('Page.captureScreenshot', { format: 'png' });
    writeFileSync(join(out, name + '.png'), Buffer.from(data, 'base64'));
  }

  for (const old of [true, false]) {
    await fresh(old);
    await js('__bridge.auto=false');
    const start = performance.now();
    for (let i = 0; i < 20; i++) await click(selector('adaptiveBitrate'));
    const ms = Math.round(performance.now() - start);
    const count = await js('__bridge.changes().length');
    assert.equal(count, old ? 20 : 1);
    assert(ms <= 3000, `20 clicks took ${ms}ms`);
    if (!old) {
      assert.equal(await value('adaptiveBitrate'), true);
      await js('__bridge.state({adaptiveBitrate:false})');
      assert.equal(await value('adaptiveBitrate'), true);
      await js('__bridge.ack(0)');
      assert.equal(await js('__bridge.changes().length'), 2);
      await js('__bridge.ack(1)');
      assert.equal(await value('adaptiveBitrate'), true);
    }
    check(old ? 'baseline_20_real_clicks' : 'candidate_20_real_clicks_last_intent', { clicks: 20, ms, inflightRequests: count, totalRequests: await js('__bridge.changes().length') });
  }

  // A48 progressive disclosure: standard view must hide the whole tuning block (title
  // included - never a bare heading over nothing), expert view must show every control,
  // and the choice must survive a reload. Same DOM both ways, so settings never reset.
  await js(`localStorage.removeItem('rincast.streamMode')`);
  await fresh(false, 1100, 1000, null);  // null = don't drive the mode: observe the real first-run default
  const stdMode = await cardReport();
  assert.equal(stdMode.mode, 'standard');
  assert.equal(stdMode.expertShown, false);
  assert.equal(stdMode.visible, 2);
  assert.equal(stdMode.danglingTitles, 0);
  assert.equal(stdMode.emptyCards, 0);
  assert.equal(stdMode.keys, 18);  // hidden, not removed: the form state survives a mode flip
  await setMode('expert');
  const expMode = await cardReport();
  assert.equal(expMode.expertShown, true);
  assert.equal(expMode.visible, 3);
  assert.equal(expMode.danglingTitles, 0);
  assert.equal(expMode.emptyCards, 0);
  assert.equal(expMode.keys, 18);
  await fresh(false, 1100, 1000, null);
  const keptMode = await cardReport();
  assert.equal(keptMode.mode, 'expert');  // remembered across reloads
  assert.equal(keptMode.expertShown, true);
  await setMode('standard');
  const backMode = await cardReport();
  assert.equal(backMode.expertShown, false);
  assert.equal(backMode.visible, 2);
  check('progressive_disclosure_standard_vs_expert', {
    standard: { visibleCards: stdMode.visible, danglingTitles: stdMode.danglingTitles, controls: stdMode.keys },
    expert: { visibleCards: expMode.visible, danglingTitles: expMode.danglingTitles, controls: expMode.keys },
    persistedAfterReload: keptMode.mode,
  });

  await fresh(); await js('__bridge.auto=false');
  await click(selector('tenBit'));
  await js('window.earlyReply=__bridge.apply(0)');
  await click(selector('adaptiveBitrate'));
  await js('window.lateReply=__bridge.apply(1); __bridge.emit(lateReply); __bridge.emit(earlyReply)');
  assert.equal(await value('tenBit'), true); assert.equal(await value('adaptiveBitrate'), false);
  await js('__bridge.emit({...__bridge.base, settings:{...__bridge.base.settings, _revision:1, tenBit:false, adaptiveBitrate:true}})');
  assert.equal(await value('tenBit'), true); assert.equal(await value('adaptiveBitrate'), false);
  check('real_dom_cross_key_reply_and_state_reordering', { tenBit: true, adaptiveBitrate: false });

  await fresh(); await js('__bridge.auto=false');
  await click(selector('adaptiveBitrate'));
  await js('window.heldReply=__bridge.apply(0); __bridge.state({adaptiveBitrate:true})');
  await click(selector('adaptiveBitrate'));
  await click(selector('adaptiveBitrate'));
  await js('__bridge.emit(heldReply)');
  assert.equal(await js('__bridge.changes().length'),2);
  await js('__bridge.ack(1)');
  assert.equal(await value('adaptiveBitrate'),false);
  check('real_dom_same_value_new_intent', { requests:2, finalValue:false });

  await fresh(); await js('__bridge.auto=false');
  await click(selector('adaptiveBitrate'));
  await js(`__bridge.emit({type:'reply', requestId:__bridge.changes()[0].requestId, ok:false, error:'fixture failure'})`);
  assert.equal(await value('adaptiveBitrate'), true);
  assert.equal(await js(`document.querySelector('.failed').textContent`), 'fixture failure');
  await click(selector('adaptiveBitrate')); await js('__bridge.ack(1)');
  assert.equal(await value('adaptiveBitrate'), false);
  assert.equal(await js(`document.querySelector('.failed').textContent`), '');
  check('real_dom_failure_recovery', { rollback: true, nextSave: false });

  await fresh();
  for (const [pcWins, headset, disabled] of [[true,123,false],[false,123,true],[true,0,false]]) {
    await js(`__bridge.state({pcBitrateWins:${pcWins},vrBitrateMbps:100},{headsetBitrateMbps:${headset}})`);
    assert.equal(await js(`document.querySelector('#vrBitrateBar input').disabled`), disabled);
    assert.equal(await js(`document.querySelector('#vrBitrateBar output').textContent`), `${disabled ? headset : 100} Mbps`);
  }
  for (const [profileValue, controls, match, slow, disabled] of [[2,true,false,false,false],[2,true,true,false,true],[2,true,false,true,true],[1,true,false,false,true],[2,false,false,false,true]]) {
    await js(`__bridge.state({streamProfile:${profileValue},profileControlsVideo:${controls}},{matchSteamVr:${match},slowPath:${slow}})`);
    assert.equal(await js(`document.querySelector('#cbrRow input').disabled`), disabled);
  }
  check('round7_real_dom_gates', { pcLimitCases: 3, cbrCases: 5, earlyPushes: 'stats/firewall/vrState before state on every fresh load' });

  await fresh(); await js('__bridge.auto=false');
  const before = await js('__bridge.changes().length');
  await js(`(() => {const e=document.querySelector('[data-setting="vrBitrateMbps"]');for(let i=0;i<100;i++){e.value=20+(i%49)*10;e.dispatchEvent(new Event('input',{bubbles:true}));}})()`);
  assert.equal(await js('__bridge.changes().length'), before);
  await js(`document.querySelector('[data-setting="vrBitrateMbps"]').dispatchEvent(new Event('change',{bubbles:true}))`);
  assert.equal(await js('__bridge.changes().length'), before + 1);
  await js('__bridge.ack(0)');
  check('real_dom_slider_input_does_not_save', { syntheticInputEvents: 100, changeRequests: 1 });

  await fresh();
  const keys = await js(`[...new Set([...document.querySelectorAll('.page[data-page="video"] [data-setting]')].map(e=>e.dataset.setting))]`);
  // Full expected set, not a bare count: a dropped or renamed setting must fail loudly.
  // (This page's controls were 14 when the gate was written; the count had drifted since.
  //  Verified against HEAD before A48: HEAD also has these 18 keys, so this is drift in
  //  the gate, not something A48 changed.)
  assert.deepEqual(keys.slice().sort(), [
    'adaptiveBitrate', 'adaptiveQuantization', 'autoTune', 'codec', 'desktopBitrateMbps',
    'desktopMaxHeight', 'encodeScalePct', 'fecPercent', 'fillMode', 'foveatedEncoding',
    'l7Autoscale', 'pcBitrateWins', 'profileControlsVideo', 'qualityProfileCbr', 'streamProfile',
    'tenBit', 'twoPassEncoding', 'vrBitrateMbps',
  ]);
  const stressStart = performance.now();
  const framesBefore = await js('__bridge.frames');
  for (const key of keys) {
    await js(`__bridge.state({codec:'auto',streamProfile:2,profileControlsVideo:true,pcBitrateWins:true},{headsetBitrateMbps:0,headsetGazeFoveation:false,matchSteamVr:false,slowPath:false})`);
    const startCount = await js('__bridge.changes().length');
    for (let i = 0; i < 10; i++) {
      const type = await js(`document.querySelector(${JSON.stringify(selector(key))}).type`);
      if (type === 'radio') {
        const nextValue = await js(`[...document.querySelectorAll(${JSON.stringify(selector(key))})].find(e=>!e.checked&&!e.disabled).value`);
        await click(selector(key) + `[value="${nextValue}"]`);
      } else if (type === 'range') {
        const point = await js(`(() => {const el=document.querySelector(${JSON.stringify(selector(key))});if(el.disabled)throw Error('Disabled slider');el.scrollIntoView({block:'center'});const r=el.getBoundingClientRect();return {x:r.x+r.width*${i % 2 ? 0.65 : 0.3},y:r.y+r.height/2};})()`);
        await page('Input.dispatchMouseEvent', { type:'mousePressed', button:'left', clickCount:1, ...point });
        await page('Input.dispatchMouseEvent', { type:'mouseReleased', button:'left', clickCount:1, ...point });
      } else await click(selector(key));
      await wait(230);
      assert.equal(await value(key), await js(`__bridge.base.settings[${JSON.stringify(key)}]`), key);
    }
    const requests = await js('__bridge.changes().length') - startCount;
    assert.equal(requests, 10, key);
    check(`real_click_${key}`, { operations: 10, requests, finalValue: await value(key) });
  }
  const stressMs = Math.round(performance.now() - stressStart);
  assert(stressMs >= 30000);
  assert.equal(await js(`[...document.querySelectorAll('.failed')].filter(e=>e.textContent).length`), 0);
  check('all_actual_video_controls_30s', { controls: keys.length, operations: keys.length * 10, ms: stressMs, animationFrames: await js('__bridge.frames') - framesBefore, absentPcControls: ['videoPacingMode', 'desktopFps'], video: 'NOT_TESTED' });

  for (const nav of ['home','audio','vr','devices','general','account','video']) {
    await click(`#nav [data-page="${nav}"]`); await wait(60);
    assert.equal(await js(`document.querySelector('.page.active').dataset.page`), nav);
  }
  check('navigation_after_stress', { pages: 7 });
  for (const [width,height] of [[1100,1000],[760,560]]) {
    await page('Emulation.setDeviceMetricsOverride', {width,height,deviceScaleFactor:1,mobile:false});
    await js(`document.querySelector('#main').scrollTop=0`); await wait(150);
    const metrics = await js(`(() => {const main=document.querySelector('#main'),el=document.querySelector('[data-setting="adaptiveBitrate"]'),r=el.getBoundingClientRect();return {width:innerWidth,height:innerHeight,x:r.x,y:r.y,w:r.width,h:r.height,mainClient:main.clientWidth,mainScroll:main.scrollWidth};})()`);
    assert.equal(metrics.width,width); assert.equal(metrics.height,height);
    assert(metrics.x >= 0 && metrics.x+metrics.w <= width && metrics.y >= 0 && metrics.y+metrics.h <= height && metrics.w > 2 && metrics.h > 2);
    assert(metrics.mainScroll <= metrics.mainClient + 1);
    await shot(`settings-browser-${width}x${height}`);
    check(`real_viewport_${width}x${height}`, metrics);
  }

  await fresh(); await js('__bridge.auto=false');
  await click(selector('adaptiveBitrate'));
  const timeoutStart = performance.now();
  await js(`new Promise((ok,fail)=>{const end=Date.now()+34000;const timer=setInterval(()=>{const note=document.querySelector('.failed');if(note?.textContent){clearInterval(timer);ok(note.textContent);}else if(Date.now()>end){clearInterval(timer);fail(Error('Timeout error was not rendered'));}},100);})`);
  const elapsed = Math.round(performance.now()-timeoutStart);
  assert(elapsed >= 29000);
  await shot('settings-browser-delayed-reply');
  await click(selector('adaptiveBitrate'));
  assert.equal(await js('__bridge.changes().length'),1);
  await js('__bridge.ack(0)');
  assert.equal(await js('__bridge.changes().length'),2);
  await js('__bridge.ack(1)');
  assert.equal(await value('adaptiveBitrate'),true);
  assert.equal(await js(`document.querySelector('.failed').textContent`),'');
  check('real_30s_timeout_late_reply_recovers', { elapsedMs:elapsed, requestsBeforeLateReply:1, totalRequests:2 });
  assert.deepEqual(await js('__bridge.pageErrors'),[]);
  assert.deepEqual(errors,[]);
  report.status='PASS';
  await send('Browser.close');
} catch (e) {
  report.failure = e.stack;
  console.error(e.stack);
  process.exitCode = 1;
} finally {
  ws?.close();
  if (browser && browser.exitCode === null) {
    browser.kill();
    await Promise.race([once(browser,'exit'),wait(3000)]);
  }
  server.closeAllConnections();
  await new Promise(r=>server.close(r));
  if (profile) {
    try { rmSync(profile,{recursive:true,force:true,maxRetries:4,retryDelay:250}); report.profileRemoved=true; }
    catch(e) { report.profileRemoved=false; report.profilePath=profile; report.cleanupError=e.message; process.exitCode=1; report.status='FAIL'; }
  }
  writeFileSync(join(out,'settings-browser-result.json'),JSON.stringify(report,null,2));
}
