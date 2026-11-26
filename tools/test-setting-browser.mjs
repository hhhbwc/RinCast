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
// A86 (user, 2026-11-23): the frozen baseline app.js still writes to #profileNote on every
// render, and index.html no longer has that node (the tier slider was deleted on purpose).
// The baseline is here to prove the 20-clicks-into-1 batching win against old JS, not to
// prove old JS can render a page whose control was removed - so the one line that touches
// the deleted node is blanked in the served copy. Resurrecting the node instead would put
// a control the product just deleted back into the page for every future reader.
const baseline = execFileSync('git', ['-C', repo, 'show', '636f9a4:pc/streamer/ui/app.js'])
  .toString('utf8')
  .replace('$("#profileNote").textContent = t(`video.profileNote${profile}`);',
           '/* A86: #profileNote is gone with the tier slider */');
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
    settings: { _revision: 1, pcName: 'Browser fixture', codec: 'auto', tenBit: false, vrBitrateMbps: 200, desktopBitrateMbps: 40, adaptiveBitrate: true, qualityProfileCbr: false, pcBitrateWins: true, fecPercent: 5, foveatedEncoding: false, adaptiveQuantization: true, twoPassEncoding: false, desktopMaxHeight: 0, streamAudio: true, playOnPcToo: false, micPassthrough: true, switchDefaultAudio: true, audioDevices: 'auto', autoRegisterSteamVrDriver: true, requireApproval: true, desktopInput: true, startWithWindows: false, preventSleep: true, cloudUrl: '', trustedDevices: [] },
    cloud: { signedIn: false },
    system: { gpu: 'NVIDIA test fixture', port: 38711, addresses: [], vpnAddresses: [], firewall: { otherNetworksOpen: true }, encoder: { backend: 'nvenc', h264: true, hevc: true, av1: true, hevc10: true }, steamvr: { installed: true, running: false, driverRegistered: true }, openxr: { rincastAvailable: false, rincastActive: false, steamvrActive: true }, audioDriver: { speaker: false, microphone: false, steam: { speaker: true, microphone: true }, active: { speaker: 'steam', microphone: 'steam' } } },
    session: { deviceName: 'Fixture headset', model: 'Fixture', address: '127.0.0.1', connectedSeconds: 60, rttMs: 3, vrState: 'idle', headsetBitrateMbps: 0, headsetCodec: 'auto', desktop: [], client: {} },
    pendingApproval: null,
  };
  const listeners = [], sent = [], pageErrors = [];
  // A86: keep the stack, not just the message. "Cannot set properties of null" names no
  // file and no line, and this gate is the only place a page-side typo shows up at all.
  addEventListener('error', e => pageErrors.push(String((e.error && e.error.stack) || e.error || e.message)));
  addEventListener('unhandledrejection', e => pageErrors.push(String((e.reason && e.reason.stack) || e.reason)));
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
  // A74 removed the two view modes: the streaming page has one view, and the tuning block is a
  // fold at the very bottom of it. Opening it is one real click on its title button, and the
  // state it must leave behind is `#advancedBody` un-hidden plus aria-expanded="true".
  // The baseline UI has no fold at all (its tuning hung off the mode bar), so callers skip it.
  async function setAdvanced(open) {
    if (await js(`document.querySelector('#advancedBody').hidden`) === open) await click('#advancedHead');
    await wait(40);
    assert.equal(await js(`document.querySelector('#advancedBody').hidden`), !open, `advanced did not ${open ? 'open' : 'close'}`);
    assert.equal(await js(`document.querySelector('#advancedHead').getAttribute('aria-expanded')`), String(open));
  }
  // A key that is meant to *activate* something has to carry its character text: Blink only
  // turns Enter/Space into a click when the keyboard event has text, so a bare keyDown would
  // look like a working disclosure in the DOM and do nothing on screen.
  async function pressKey(key, vk, text) {
    await page('Input.dispatchKeyEvent', { type: 'keyDown', key, code: key, windowsVirtualKeyCode: vk, ...(text ? { text } : {}) });
    await page('Input.dispatchKeyEvent', { type: 'keyUp', key, code: key, windowsVirtualKeyCode: vk });
  }
  const enter = () => pressKey('Enter', 13, '\r');
  const focusName = `(() => {const e=document.activeElement;if(!e||e===document.body)return null;return e.dataset.setting||e.id||(e.tagName+':'+(e.getAttribute('data-i18n')||''));})()`;
  // Walks the real Tab order until something matches and reports how many presses it took.
  // A control that can only be reached with a mouse is not a control.
  async function tabUntil(hits, max = 30) {
    for (let i = 1; i <= max; i++) {
      await pressKey('Tab', 9);
      const at = await js(focusName);
      if (hits.includes(at)) return { presses: i, at };
    }
    throw Error(`Tab order never reached ${hits.join('/')}; stuck at ${await js(focusName)}`);
  }
  async function cardReport() {
    return js(`(() => {
      const page = document.querySelector('#videoPage');
      const cards = [...page.querySelectorAll('.card')];
      const fold = page.querySelector('#advanced');
      const visible = cards.filter(c => c.getBoundingClientRect().height > 0);
      const titleOf = c => (c.querySelector('h2')?.innerText || '').trim();
      const rowsOf = c => [...c.querySelectorAll('.row')].filter(r => r.getBoundingClientRect().height > 0).length;
      return {
        cards: cards.length,
        visible: visible.length,
        // A visible title with no visible control under it is the "empty shell" bug. The fold
        // is meant to have no rows showing while collapsed, so it gets its own line below
        // instead of being excused here: collapsed => a real title of its own, expanded => rows.
        danglingTitles: visible.filter(c => !titleOf(c)).length,
        emptyCards: visible.filter(c => c !== fold && rowsOf(c) === 0).length,
        foldIsLast: page.lastElementChild === fold,
        foldHeight: +fold.getBoundingClientRect().height.toFixed(2),
        foldTitle: titleOf(fold),
        foldRows: rowsOf(fold),
        keys: new Set([...page.querySelectorAll('[data-setting]')].map(e => e.dataset.setting)).size,
      };
    })()`);
  }
  // Every control's current value, so a disclosure toggle can be proven not to reset the form.
  const snapshot = () => js(`(() => {
    const out = {};
    for (const el of document.querySelectorAll('#videoPage [data-setting]')) {
      const k = el.dataset.setting;
      if (el.type === 'checkbox') out[k] = el.checked;
      else if (el.type === 'radio') { if (el.checked) out[k] = el.value; }
      else out[k] = el.value;
    }
    return out;
  })()`);
  async function fresh(old = false, width = 1100, height = 1000, advanced = true) {
    useBaseline = old;
    await page('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
    await page('Page.navigate', { url });
    await js(`new Promise((ok,fail)=>{const end=Date.now()+5000;const timer=setInterval(()=>{if(document.querySelector('#pcName')?.value==='Browser fixture'){clearInterval(timer);ok(true);}else if(Date.now()>end){clearInterval(timer);fail(Error('UI did not initialize'));}},20);})`);
    await click('#nav [data-page="video"]');
    await wait(60);
    assert.equal(await js(`document.querySelector('.page.active').dataset.page`), 'video');
    assert.deepEqual(await js('__bridge.pageErrors'), []);
    if (!old && advanced !== null) await setAdvanced(advanced);
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

  // A48 progressive disclosure, re-pointed at A74's single bottom-of-page fold. What has to
  // hold unchanged: hiding a block hides its heading with it, a heading never stands over
  // nothing, the same DOM serves both states so no toggle can reset the form, and the empty
  // card rule (app.css:99) must not eat a collapsed block whole - that is what foldHeight and
  // foldTitle measure with real pixels instead of a screenshot.
  await fresh(false, 1100, 1000, null);  // null = don't touch the fold: observe the real first-load state
  const shut = await cardReport();
  // U1: the expert/standard switch is gone from the DOM, not merely hidden.
  assert.deepEqual(await js(`(() => ({
    setModeButtons: document.querySelectorAll('[data-set-mode]').length,
    modeBars: document.querySelectorAll('.mode-bar').length,
    dataMode: document.querySelector('#videoPage').hasAttribute('data-mode'),
    expertBlocks: document.querySelectorAll('.expert-only').length,
    ariaPressed: document.querySelectorAll('#videoPage [aria-pressed]').length,
  }))()`), { setModeButtons: 0, modeBars: 0, dataMode: false, expertBlocks: 0, ariaPressed: 0 });
  // A closed fold is still a block the user can see and open.
  assert(shut.foldHeight > 0, 'the collapsed advanced block has zero height: app.css:99 ate it');
  assert(shut.foldTitle.length > 0, 'the collapsed advanced block has no visible title');
  assert.equal(shut.foldIsLast, true);
  assert.equal(shut.foldRows, 0, 'a collapsed fold still shows rows');
  assert.equal(shut.cards, 3);
  assert.equal(shut.visible, 3);
  assert.equal(shut.danglingTitles, 0);
  assert.equal(shut.emptyCards, 0);
  assert.equal(shut.keys, 15);  // A86b: 18 - slider - switch - tier-driven-video; hidden, not removed
  const valuesShut = await snapshot();

  // A86 (user, 2026-11-23): the tuner's on/off switch is gone from this page. The
  // sentence that says the tuner exists takes its place, and it has to sit outside the
  // 高级 fold exactly like the switch did: a user reading the streaming page must learn
  // that the picture moves on its own without opening anything.
  const autoNote = await js(`document.querySelector('#autoTuneNote')`);
  assert.equal(autoNote, null);
  check('u5_auto_tune_note_gone', { autoNote });

  // U6: shut means really shut - nothing inside the body can take focus, by script or by
  // keyboard. offsetParent is null only for a display:none subtree, which is what [hidden]
  // gives here; a visibility/height:0 fold would keep its controls focusable and fail this.
  const shutFocus = await js(`(() => {
    const inside = [...document.querySelectorAll('#advancedBody input, #advancedBody select, #advancedBody button')];
    inside.forEach(el => el.focus());
    return { count: inside.length, unfocusable: inside.every(el => el.offsetParent === null),
      active: document.activeElement.closest('#advanced') !== null };
  })()`);
  assert(shutFocus.count > 10 && shutFocus.unfocusable && !shutFocus.active, JSON.stringify(shutFocus));

  // U3: opening it must not move a single always-visible control, at any scroll position.
  // Tops are measured in document space (rect.top + the scroller's offset), because "被顶下去"
  // is a layout fact: plain viewport rects also move when the browser scrolls something into
  // view, which is what Tab does to reach the fold, and that scroll is the page behaving well.
  const anchorTops = () => js(`(() => {
    const page = document.querySelector('#videoPage');
    const main = document.querySelector('#main');
    const at = sel => { const el = page.querySelector(sel); return el ? +(el.getBoundingClientRect().top + main.scrollTop).toFixed(2) : null; };
    return { autoNote: null,
      bitrate: at('[data-setting="vrBitrateMbps"]'), adaptive: at('[data-setting="adaptiveBitrate"]'),
      codec: at('#codecOptions'), desktop: at('[data-setting="desktopMaxHeight"]'),
      desktopBitrate: at('[data-setting="desktopBitrateMbps"]'),
      scrollTop: +main.scrollTop.toFixed(2),
      mainScroll: main.scrollHeight, bodyScroll: document.body.scrollHeight };
  })()`);
  const anchorWidths = () => js(`(() => {
    const main = document.querySelector('#main');
    const labels = [...document.querySelectorAll('#videoPage .cols .options label')].map(label => {
      const span = label.querySelector('span');
      const r = label.getBoundingClientRect();
      return { text: label.textContent.trim(), width: +r.width.toFixed(2), lines: span.getClientRects().length };
    });
    return { mainClientWidth: main.clientWidth, labels };
  })()`);
  const beforeOpen = await anchorTops();
  const beforeWidths = await anchorWidths();
  // Reach the fold with the keyboard only, and open it with a real Enter - a disclosure that
  // only answers to a mouse is not one. The walk starts from a known element, so the number of
  // presses it reports is a fact about the tab order rather than about the previous test.
  await js(`document.querySelector('.rail-foot [data-page="account"]').focus()`);
  const toHead = await tabUntil(['advancedHead'], 30);
  const headAttrs = await js(`(() => { const s = document.querySelector('#advancedHead');
    return { expanded: s.getAttribute('aria-expanded'), label: document.getElementById(s.getAttribute('aria-labelledby'))?.innerText.trim().length > 0,
      body: !!document.getElementById(s.getAttribute('aria-controls')) }; })()`);
  assert.equal(headAttrs.expanded, 'false');
  assert(headAttrs.label && headAttrs.body);
  await enter();
  await wait(40);
  const open = await cardReport();
  assert.equal(open.foldRows, 10, 'A86b: 11 - the "Quality follows profile" row; expanding showed a different number of rows than the block owns');
  assert.equal(open.foldHeight > shut.foldHeight, true);
  assert.equal(open.keys, 15);  // A86b: same 15, nothing appears when the fold opens
  assert.equal(open.danglingTitles, 0);
  assert.equal(open.emptyCards, 0);
  const afterOpen = await anchorTops();
  const afterWidths = await anchorWidths();
  const moved = Object.keys(beforeOpen).filter(k => !['mainScroll', 'bodyScroll', 'scrollTop'].includes(k) && beforeOpen[k] !== afterOpen[k]);
  assert.deepEqual(moved, [], `expanding 高级 moved: ${moved.map(k => `${k} ${beforeOpen[k]}->${afterOpen[k]}`).join(', ')}`);
  assert.equal(afterWidths.mainClientWidth, beforeWidths.mainClientWidth,
    `expanding 高级 changed content width: ${beforeWidths.mainClientWidth}->${afterWidths.mainClientWidth}`);
  assert.deepEqual(afterWidths.labels, beforeWidths.labels, 'expanding 高级 squeezed an adjacent option or changed its wrapping');
  assert(afterOpen.mainScroll > beforeOpen.mainScroll, 'the page did not grow at all: nothing was expanded');
  assert.deepEqual(await snapshot(), valuesShut, 'the disclosure toggle changed a control value');
  const openHead = await js(`document.querySelector('#advancedHead').getAttribute('aria-expanded')`);
  assert.equal(openHead, 'true');
  await enter();  // and closes again the same way
  await wait(40);
  assert.equal(await js(`document.querySelector('#advancedBody').hidden`), true);
  assert.equal(await js(`document.querySelector('#advancedHead').getAttribute('aria-expanded')`), 'false');
  check('u1_no_expert_mode', await js(`(() => ({ setModeButtons: document.querySelectorAll('[data-set-mode]').length, modeBars: document.querySelectorAll('.mode-bar').length, dataMode: document.querySelector('#videoPage').hasAttribute('data-mode'), expertBlocks: document.querySelectorAll('.expert-only').length }))()`));
  check('progressive_disclosure_advanced_fold', {
    collapsed: { visibleCards: shut.visible, foldHeight: shut.foldHeight, foldTitle: shut.foldTitle, foldRows: shut.foldRows, danglingTitles: shut.danglingTitles, controls: shut.keys },
    expanded: { visibleCards: open.visible, foldHeight: open.foldHeight, foldRows: open.foldRows, danglingTitles: open.danglingTitles, controls: open.keys },
    valuesUnchangedByToggle: true, reopenedWithKeyboard: toHead.presses,
  });
  check('u3_advanced_last_and_grows_down', {
    isLastSibling: open.foldIsLast, tabPressesToFold: toHead.presses,
    anchorsUnmoved: Object.fromEntries(Object.keys(beforeOpen).filter(k => !['mainScroll', 'bodyScroll', 'scrollTop'].includes(k)).map(k => [k, +(afterOpen[k] - beforeOpen[k]).toFixed(2)])),
    foldHeight: { collapsed: shut.foldHeight, expanded: open.foldHeight },
    mainScrollHeight: { collapsed: beforeOpen.mainScroll, expanded: afterOpen.mainScroll },
    bodyScrollHeight: { collapsed: beforeOpen.bodyScroll, expanded: afterOpen.bodyScroll },
    // Recorded, not asserted: Tab scrolling the fold into view is correct behaviour, and the
    // anchors above are in document space precisely so this cannot hide a layout shift.
    scrollTop: { beforeKeyboardWalk: beforeOpen.scrollTop, afterOpen: afterOpen.scrollTop },
  });
  check('u6_collapsed_controls_unfocusable', shutFocus);

  // A86 (user, 2026-11-23): the "Priority" slider is GONE from this page - the user's
  // call was that the automatic tuner owns the picture and there is nothing to pick. The
  // guarded facts are therefore the negative ones: no slider, no switch, no leftover stop
  // labels, and the sentence that replaced them is plain text rather than a control that
  // could steal a Tab stop. The bitrate sliders must not have noticed any of this.
  await fresh(false, 1100, 1000, false);
  const tierGone = await js(`(() => ({
    sliders: document.querySelectorAll('[data-setting="streamProfile"]').length,
    switches: document.querySelectorAll('[data-setting="autoTune"]').length,
    radios: document.querySelectorAll('input[name="streamProfile"]').length,
    stops: [...document.querySelectorAll('#videoPage .ends span')].length,
    note: document.querySelector('#autoTuneNote'),
  }))()`);
  assert.equal(tierGone.sliders, 0, 'the tier slider is still in the DOM');
  assert.equal(tierGone.switches, 0, 'the tuner switch is still in the DOM');
  assert.equal(tierGone.radios, 0, 'the old three-way radio row survived');
  assert.equal(tierGone.stops, 0, 'the tier end labels are still in the DOM');
  assert.equal(tierGone.note, null, '#autoTuneNote was removed as an unneeded static banner');
  const bitrateLabels = await js(`(() => { const one = k => { const el = document.querySelector('[data-setting="' + k + '"]'); return el.parentElement.querySelector('output').textContent; };
    return { vr: one('vrBitrateMbps'), desktop: one('desktopBitrateMbps') }; })()`);
  assert(/ Mbps$/.test(bitrateLabels.vr) && / Mbps$/.test(bitrateLabels.desktop), JSON.stringify(bitrateLabels));
  check('u2_tier_controls_gone', { ...tierGone, bitrateLabels });

  // A52: a machine whose vendor encoder failed still streams on Media Foundation. The home
  // page has to say so, otherwise "it got slower after a driver update" has no visible cause.
  // The assertion is language-independent on purpose: the UI follows the browser locale.
  await fresh(false, 1100, 1000, null);
  const readIssues = `[...document.querySelectorAll('#issues li')].map(e=>({level:e.className, text:e.innerText.slice(0,40)}))`;
  const issuesBefore = await js(readIssues);
  await js(`__bridge.emit({...__bridge.base, system:{...__bridge.base.system, encoder:{...__bridge.base.system.encoder, hwFallback:true}}})`);
  await wait(180);
  const issuesAfter = await js(readIssues);
  assert.equal(issuesAfter.length, issuesBefore.length + 1, 'hwFallback did not add a new home-page issue');
  assert.equal(issuesAfter.filter(i => i.level === 'warn').length,
               issuesBefore.filter(i => i.level === 'warn').length + 1, 'the new issue is not a warning');
  check('hw_fallback_issue_visible', { before: issuesBefore.length, after: issuesAfter.map(i => i.level) });

  // A96 (field case, 2026-11-24): 34 fps of a requested 72, loss 0 %, nothing dropped, a 3 ms
  // round trip, and the home page told the user to run a cable to the router. The encoder was
  // idle (4 ms) - the game needed 26 ms for a 14 ms frame. The headset already reports that
  // stage separately, so the fix is UI-only. Six stats ticks because a number only counts as
  // poor after five in a row. Both controls are required: without the under-threshold feed the
  // assertion also passes when the card fires for *any* slow stream, and without the dropped
  // feed the network gate is untested. The discriminator is the per-eye size, which no other
  // issue text contains in any locale, so this holds when the browser language changes.
  // A fresh page first: the hwFallback fixture above leaves its card on the base state, and
  // reloading also zeroes the poor-runs, so the six ticks below are what makes the card appear.
  await fresh(false, 1100, 1000, null);
  const feedRate = (game, droppedFps, ticks = 6) => js(`(async () => {
    for (let i = 0; i < ${ticks}; i++) {
      __bridge.emit({ type: 'stats', session: { ...__bridge.base.session, vrState: 'running',
        vr: { fps: 34, refreshRate: 72, encodeMs: 4.2, mbps: 22, eyeWidth: 2688, eyeHeight: 2688,
              droppedFps: ${droppedFps}, latencyMs: 118, codec: 'HEVC' },
        client: { latencyStages: { uplink: 1.4, game: ${game}, encoderWait: 0.6, encode: 4.4,
                                 downlink: 1.9, decode: 9.1, nextFrame: 2, compositor: 30 } } } });
      await new Promise(r => setTimeout(r, 25));
    }
    return [...document.querySelectorAll('#issues li')].map(e => ({ level: e.className, text: e.innerText }));
  })()`);
  const twoTicks = await feedRate(26, 0, 2);
  const overBudget = await feedRate(26, 0, 4);
  const underBudget = await feedRate(12, 0);
  const losingPackets = await feedRate(26, 6);
  assert.equal(twoTicks.length, 0, 'a trouble card appeared before the five ticks that make a number poor: ' + JSON.stringify(twoTicks));
  assert.equal(overBudget.length, 1, 'the trouble list is not exactly one card: ' + JSON.stringify(overBudget));
  assert(overBudget[0].level === 'warn', 'the game-bound card is not a warning: ' + overBudget[0].level);
  assert(overBudget[0].text.includes('2688'), 'the game-bound card never names the per-eye size: ' + overBudget[0].text);
  assert(!underBudget[0].text.includes('2688'), 'the game-bound card fires under its own threshold: ' + underBudget[0].text);
  assert(!losingPackets[0].text.includes('2688'), 'the card ignores that frames are being dropped: ' + losingPackets[0].text);
  assert.notEqual(overBudget[0].text, underBudget[0].text, 'both fixtures give the same advice, so nothing discriminates');
  check('game_bound_card_replaces_network_advice', { ticks2: twoTicks.length, over: overBudget, under: underBudget, dropped: losingPackets });

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
  // A86b: CBR is an independent switch. Neither a tier number nor a "let the tier
  // drive video" checkbox exists on this page any more - that coupling made the
  // link oscillate. Only the link gates remain.
  for (const [match, slow, disabled] of [[false,false,false],[true,false,true],[false,true,true]]) {
    await js(`__bridge.state({qualityProfileCbr:true},{matchSteamVr:${match},slowPath:${slow}})`);
    assert.equal(await js(`document.querySelector('#cbrRow input').disabled`), disabled);
  }
  check('round7_real_dom_gates', { pcLimitCases: 3, cbrCases: 3, earlyPushes: 'stats/firewall/vrState before state on every fresh load' });

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
  // A86 (user, 2026-11-23) removed two of them with their controls: 'streamProfile' (the
  // tier slider) and 'autoTune' (its on/off switch). 18 -> 16, and the sentence that
  // replaced them carries no data-setting on purpose.
  assert.deepEqual(keys.slice().sort(), [
    'adaptiveBitrate', 'adaptiveQuantization', 'codec', 'desktopBitrateMbps',
    'desktopMaxHeight', 'encodeScalePct', 'fecPercent', 'fillMode', 'foveatedEncoding',
    'l7Autoscale', 'pcBitrateWins', 'qualityProfileCbr',
    'tenBit', 'twoPassEncoding', 'vrBitrateMbps',
  ]);
  const stressStart = performance.now();
  const framesBefore = await js('__bridge.frames');
  for (const key of keys) {
    await js(`__bridge.state({codec:'auto',qualityProfileCbr:false,pcBitrateWins:true},{headsetBitrateMbps:0,headsetGazeFoveation:false,matchSteamVr:false,slowPath:false})`);
    const startCount = await js('__bridge.changes().length');
    for (let i = 0; i < 10; i++) {
      const type = await js(`document.querySelector(${JSON.stringify(selector(key))}).type`);
      if (type === 'radio') {
        const nextValue = await js(`[...document.querySelectorAll(${JSON.stringify(selector(key))})].find(e=>!e.checked&&!e.disabled).value`);
        await click(selector(key) + `[value="${nextValue}"]`);
      } else if (type === 'range') {
        // A click sets whatever value the pointer lands on, so the two test positions have to be
        // far enough apart to hit two different stops: on a three-stop slider 30% and 65% are
        // the same stop, and the loop would click ten times and assert on nothing.
        const stops = await js(`(() => {const el=document.querySelector(${JSON.stringify(selector(key))});return (el.max-el.min)/el.step;})()`);
        const [a, b] = stops <= 4 ? [0.1, 0.9] : [0.3, 0.65];
        const point = await js(`(() => {const el=document.querySelector(${JSON.stringify(selector(key))});if(el.disabled)throw Error('Disabled slider');el.scrollIntoView({block:'center'});const r=el.getBoundingClientRect();return {x:r.x+r.width*${i % 2 ? b : a},y:r.y+r.height/2};})()`);
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

  for (const nav of ['home','audio','devices','general','video']) {
    await click(`.rail [data-page="${nav}"]`); await wait(60);
    assert.equal(await js(`document.querySelector('.page.active').dataset.page`), nav);
  }
  // A53: Account left the main list for the rail footer; the section must still open, and the
  // everyday list must stay at five so a new user has five things to choose between.
  assert.equal(await js(`document.querySelectorAll('#nav [data-page]').length`), 5);
  assert.equal(await js(`document.querySelectorAll('.rail-foot [data-page]').length`), 1);
  await click('.rail-foot [data-page="account"]'); await wait(60);
  assert.equal(await js(`document.querySelector('.page.active').dataset.page`), 'account');
  assert.equal(await js(`document.querySelector('.rail-foot [data-page="account"]').getAttribute('aria-current')`), 'page');
  check('navigation_after_stress', { mainSections: 5, railFoot: 1, pages: await js(`document.querySelectorAll('.page').length`) });
  // U4 (the M-a measurement, re-taken against the new columns): .cols used to be a grid, which
  // puts both columns in one shared row - the taller one set the row height and the other box
  // was stretched to match, dragging everything below the pair down. Two flex columns may not
  // do that: no control in the left column moves, and the left column's own box keeps its
  // height, whatever the right column grows into.
  for (const [width, height] of [[1280, 800], [720, 900]]) {
    await fresh(false, width, height, null);
    await click('.rail [data-page="general"]'); await wait(80);
    const cols = await js(`(() => {
      const box = document.querySelector('.page[data-page="general"] .cols');
      const left = box.children[0], right = box.children[1];
      const ctl = left.querySelector('input,select,button,label');
      const read = () => ({ top: +ctl.getBoundingClientRect().top.toFixed(2), left: +left.getBoundingClientRect().height.toFixed(2), cols: +box.getBoundingClientRect().height.toFixed(2) });
      const at = read();
      const probe = document.createElement('div'); probe.style.height = '200px';
      right.appendChild(probe);
      const with200 = read();
      probe.style.height = '600px';
      const with600 = read();
      probe.remove();
      const back = read();
      const lb = left.getBoundingClientRect(), rb = right.getBoundingClientRect();
      // The visual separation of two stacked columns is the heading margin INSIDE the second
      // column box, so the distance between the boxes is legitimately 0. Measure the content
      // instead: last thing shown in the left column, first thing shown in the right one.
      const firstShown = el => { const k = [...el.children].find(x => x.getBoundingClientRect().height > 0); return k ? firstShown(k) : el; };
      const lastShown = el => { const k = [...el.children].reverse().find(x => x.getBoundingClientRect().height > 0); return k ? lastShown(k) : el; };
      const contentGap = +(firstShown(right).getBoundingClientRect().top - lastShown(left).getBoundingClientRect().bottom).toFixed(2);
      const style = getComputedStyle(box);
      return { display: style.display, flexDirection: style.flexDirection, alignItems: style.alignItems,
        firstControl: ctl.id || ctl.tagName, firstControlTop: at.top,
        controlDelta: { plus200: +(with200.top - at.top).toFixed(2), plus600: +(with600.top - at.top).toFixed(2), afterRemoval: +(back.top - at.top).toFixed(2) },
        leftBoxHeight: { plain: at.left, plus200: with200.left, plus600: with600.left },
        colsHeight: { plain: at.cols, plus600: with600.cols },
        sideBySide: Math.abs(rb.top - lb.top) < 0.5, stacked: rb.top >= lb.bottom - 0.5, stackGap: +(rb.top - lb.bottom).toFixed(2), contentGap,
        widthsEqual: Math.abs(lb.width - rb.width) < 0.5, columnWidth: +lb.width.toFixed(2) };
    })()`);
    assert.equal(cols.display, 'flex');
    // The computed spelling of `align-items: flex-start` is `flow-start` in this Chrome (CSS Box
    // Alignment serialization). Side by side the cross axis must be top-aligned - `normal`/
    // `stretch` is exactly what dragged the short column down to the tall one. Once stacked there
    // is no cross axis to align, so full-width `stretch` is the right value there.
    if (cols.flexDirection === 'column') assert.equal(cols.alignItems, 'stretch', 'the stacked columns stopped filling the width');
    else assert(['flex-start', 'flow-start', 'start'].includes(cols.alignItems), `the columns are top-aligned as ${JSON.stringify(cols.alignItems)}: the taller one will stretch the other`);
    assert.equal(cols.controlDelta.plus200, 0, `the right column still pushes the left one down: ${JSON.stringify(cols.controlDelta)}`);
    assert.equal(cols.controlDelta.plus600, 0, `the right column still pushes the left one down: ${JSON.stringify(cols.controlDelta)}`);
    assert.equal(cols.controlDelta.afterRemoval, 0);
    assert.equal(cols.leftBoxHeight.plus200, cols.leftBoxHeight.plain, `the left column is still stretched to the right one: ${JSON.stringify(cols.leftBoxHeight)}`);
    assert.equal(cols.leftBoxHeight.plus600, cols.leftBoxHeight.plain, `the left column is still stretched to the right one: ${JSON.stringify(cols.leftBoxHeight)}`);
    assert.equal(cols.widthsEqual, true, 'the two columns stopped being equal width');
    if (width >= 1000) assert.equal(cols.sideBySide, true, 'the columns did not sit side by side in a wide window');
    else {
      assert.equal(cols.stacked, true, 'the columns did not fall back to one column in a narrow window');
      assert(cols.contentGap >= 20, `the stacked columns run into each other: ${cols.contentGap}px of content gap`);
    }
    check(`u4_columns_independent_${width}x${height}`, cols);
  }

  // The viewport checks measure a Streaming control, so go back to the page that owns it.
  await click('.rail [data-page="video"]'); await wait(60);
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
