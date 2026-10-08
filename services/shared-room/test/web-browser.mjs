import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {once} from "node:events";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {resolve} from "node:path";
import {createServer} from "node:net";
import {setTimeout as sleep} from "node:timers/promises";
import {passwordRecord} from "../tools/web-users.mjs";

const root = resolve(import.meta.dirname, "..");
const scratchRoot = resolve(root, ".tmp");
await mkdir(scratchRoot, {recursive: true});
const scratch = await mkdtemp(resolve(scratchRoot, "web-browser-"));
const children = [], errors = [], browserErrors = [];
const axeSource = await readFile(resolve(root, "node_modules/axe-core/axe.min.js"), "utf8");
let cdp;
const portProbe = createServer();
portProbe.listen(0, "127.0.0.1");
await once(portProbe, "listening");
const port = portProbe.address().port;
await new Promise(resolveClose => portProbe.close(resolveClose));
const base = `http://localhost:${port}`;
const accountTest = process.argv.includes("--accounts");

function start(command, args) {
  const child = spawn(command, args, {cwd: root, stdio: ["ignore", "pipe", "pipe"]});
  children.push(child);
  let output = "";
  const append = chunk => {output += chunk;};
  child.stdout.on("data", append); child.stderr.on("data", append);
  child.on("error", error => errors.push(error));
  return {child, output: () => output};
}
async function until(test, label, timeout = 20000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    if (await test()) return;
    await sleep(75);
  }
  throw new Error(`Timed out: ${label}`);
}
async function devtools(url) {
  const socket = new WebSocket(url), pending = new Map(), listeners = new Set();
  let id = 0;
  await once(socket, "open");
  socket.addEventListener("message", event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const call = pending.get(message.id);
      if (!call) return;
      pending.delete(message.id); clearTimeout(call.timer);
      if (message.error) call.reject(new Error(message.error.message));
      else call.resolve(message.result);
    } else for (const listener of listeners) listener(message);
  });
  return {
    call(method, params = {}, sessionId) {
      return new Promise((resolveCall, reject) => {
        const n = ++id;
        const timer = setTimeout(() => {pending.delete(n); reject(new Error(`DevTools timeout: ${method}`));}, 15000);
        pending.set(n, {resolve: resolveCall, reject, timer});
        socket.send(JSON.stringify({id: n, method, params, ...(sessionId ? {sessionId} : {})}));
      });
    },
    listen(fn) {listeners.add(fn); return () => listeners.delete(fn);},
    close() {socket.close();},
  };
}
async function page(context, width = 1280, height = 900) {
  const {targetId} = await cdp.call("Target.createTarget", {url: "about:blank", browserContextId: context});
  const {sessionId} = await cdp.call("Target.attachToTarget", {targetId, flatten: true});
  const call = (method, params) => cdp.call(method, params, sessionId);
  await call("Page.enable"); await call("Runtime.enable"); await call("Network.enable");
  await call("Emulation.setDeviceMetricsOverride", {width, height, deviceScaleFactor: 1, mobile: width < 640});
  cdp.listen(event => {
    if (event.sessionId === sessionId && event.method === "Runtime.exceptionThrown")
      browserErrors.push(event.params.exceptionDetails.exception?.description ?? event.params.exceptionDetails.text);
  });
  const evaluate = async expression => {
    const result = await call("Runtime.evaluate", {expression, awaitPromise: true, returnByValue: true});
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
    return result.result.value;
  };
  const wait = (expression, label) => until(() => evaluate(expression), label);
  const open = async () => {
    await call("Page.navigate", {url: base});
    await wait(`document.getElementById('channel-count')?.textContent === '3'`, "room listing");
    await wait(`!document.getElementById('join-panel').hidden || !document.getElementById('composer-area').hidden`, "room selection");
  };
  await open();
  return {
    targetId, sessionId, call, evaluate, wait, open,
    async select(name) {
      await evaluate(`Array.from(document.querySelectorAll('.channel')).find(b => b.querySelector('.channel-label').textContent === ${JSON.stringify(name)}).click()`);
      await wait(`document.getElementById('room-name').textContent === ${JSON.stringify(name)}`, `select ${name}`);
    },
    async join(name, password = "room") {
      await evaluate(`document.getElementById(${JSON.stringify(accountTest ? "account-username" : "display-name")}).value = ${JSON.stringify(name)};
        document.getElementById('room-password').value = ${JSON.stringify(password)};
        document.getElementById('join-form').requestSubmit()`);
      await wait(`document.getElementById('connection-text').textContent === 'Connected'`, "room login and socket");
    },
    async type(text) {
      await evaluate(`document.getElementById('message-input').value = ${JSON.stringify(text)};
        document.getElementById('message-input').dispatchEvent(new Event('input', {bubbles: true}))`);
    },
    async send(text) {
      await this.type(text);
      await evaluate(`document.getElementById('compose-form').requestSubmit()`);
      await wait(`!document.getElementById('message-input').disabled && document.getElementById('message-input').value === ''`, "confirmed post");
    },
    async sees(text) {
      await wait(`Array.from(document.querySelectorAll('.message-text')).some(p => p.textContent === ${JSON.stringify(text)})`, `message ${text}`);
    },
    async screenshot(path) {
      const capture = await call("Page.captureScreenshot", {format: "png"});
      await writeFile(path, Buffer.from(capture.data, "base64"));
    },
  };
}
async function accountsFlow(desktop, mobile) {
  await accessibility(desktop, "account login");
  assert.equal(await desktop.evaluate(`document.getElementById('display-name').hidden`), true);
  await desktop.join("alice", "fixture password only");
  await mobile.select("Uplink"); await mobile.join("sam", "second fixture password");
  const desktopKey = await desktop.evaluate(`document.getElementById('room-info').click(); document.getElementById('detail-author').textContent`);
  assert.match(desktopKey, /^[a-f0-9]{64}$/);
  await key(desktop, "Escape", "Escape");
  await desktop.send("Desktop keys stay on each desktop.");
  await mobile.sees("Desktop keys stay on each desktop.");
  assert.equal(await mobile.evaluate(`document.querySelector('.message-name').textContent`), "Alice");
  assert.equal(await desktop.evaluate(`document.getElementById('room-password').value`), "");
  assert.equal(await desktop.evaluate(`Object.values(localStorage).some(v => /fixture password|privateKey|verificationKey/.test(v))`), false);
  for (const scheme of ["light", "dark"]) {
    await palette(desktop, scheme); await accessibility(desktop, `${scheme} account conversation`);
  }
  await desktop.type("Draft stays with this device.");
  await desktop.open();
  await desktop.wait(`document.getElementById('connection-text').textContent === 'Connected'`, "account session restore");
  assert.equal(await desktop.evaluate(`document.getElementById('message-input').value`), "Draft stays with this device.");
  await desktop.evaluate(`document.getElementById('room-info').click()`);
  assert.equal(await desktop.evaluate(`document.getElementById('detail-author').textContent`), desktopKey);
  await key(desktop, "Escape", "Escape");
  let dropped = false;
  const remove = cdp.listen(event => {
    if (event.method !== "Fetch.requestPaused" || event.sessionId !== desktop.sessionId) return;
    dropped = true;
    desktop.call("Fetch.failRequest", {requestId: event.params.requestId, errorReason: "ConnectionReset"})
      .then(() => desktop.call("Fetch.disable")).catch(error => errors.push(error));
  });
  await desktop.call("Fetch.enable", {patterns: [{urlPattern: "*/A/posts", requestStage: "Response"}]});
  await desktop.type("One account post, even after a lost response.");
  await desktop.evaluate(`document.getElementById('compose-form').requestSubmit()`);
  await desktop.wait(`!document.getElementById('pending-post').hidden && !document.getElementById('retry-post').disabled`, "account uncertain outbox");
  await mobile.sees("One account post, even after a lost response.");
  assert.equal(dropped, true); remove();
  await desktop.evaluate(`fetch('/v1/web/rooms/A/logout', {method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})`);
  await desktop.wait(`!document.getElementById('join-panel').hidden`, "account expired-session recovery");
  await desktop.open(); await desktop.join("alice", "fixture password only");
  await desktop.evaluate(`document.getElementById('retry-post').click()`);
  await desktop.wait(`document.getElementById('pending-post').hidden`, "account post retry");
  assert.equal(await desktop.evaluate(`Array.from(document.querySelectorAll('.message-text')).filter(p=>p.textContent==='One account post, even after a lost response.').length`), 1);
  const fixture = JSON.parse(await readFile(resolve(root, "test/native-fixtures.json"), "utf8"));
  const rf = async packet => {
    const response = await fetch(`${base}/v1/aliases/A/operations`, {method: "POST",
      headers: {Authorization: "Bearer one"}, body: JSON.stringify({op:"rf", packet})});
    assert.equal(response.status, 200);
    assert.equal((await response.json()).accepted, true);
  };
  await rf(fixture.authorLogin); await rf(fixture.post);
  await desktop.sees("hello");
  assert.equal(await desktop.evaluate(`Array.from(document.querySelectorAll('.message')).find(p=>p.querySelector('.message-text').textContent==='hello').querySelector('.message-name').textContent`),
    fixture.author.publicKey.slice(0, 8) + "…");
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(await readFile(resolve(root, "src/native-crypto.wasm"))), {}).exports;
  const arena = new Uint8Array(wasm.memory.buffer, wasm.mc_arena(), 2048);
  const stamp = Buffer.alloc(4); stamp.writeUInt32LE(Math.floor(Date.now()/1000));
  const app = Buffer.concat([Buffer.from([0x81]), Buffer.from("VE6SLP Radio")]);
  const signable = Buffer.concat([Buffer.from(fixture.author.publicKey,"hex"), stamp, app]);
  let advert;
  try {
    arena.set(Buffer.from(fixture.author.key,"hex")); wasm.mc_pub(); arena.set(signable,160); wasm.mc_sign(signable.length);
    advert = Buffer.concat([Buffer.from([17,0x80]), signable.subarray(0,36), Buffer.from(arena.slice(1024,1088)), app]).toString("base64");
  } finally {arena.fill(0);}
  await rf(advert);
  await desktop.wait(`Array.from(document.querySelectorAll('.message')).some(p=>p.querySelector('.message-text').textContent==='hello'&&p.querySelector('.message-name').textContent==='VE6SLP Radio')`, "live signed radio name");
  await desktop.open();
  await desktop.wait(`document.getElementById('connection-text').textContent === 'Connected'`, "account and profile restore");
  await desktop.wait(`Array.from(document.querySelectorAll('.message-name')).some(p=>p.textContent==='VE6SLP Radio')`, "persisted radio name");
  assert.deepEqual(browserErrors, []); assert.deepEqual(errors, []);
  console.log("PASS: operator accounts, nonextractable persistent desktop keys, light/dark account a11y, shared content, signed radio names, drafts and same-author idempotent recovery.");
}
function contrast(a, b) {
  const luminance = color => {
    const hex = color.trim().replace("#", "");
    const expanded = hex.length === 3 ? [...hex].map(c => c + c).join("") : hex;
    assert.match(expanded, /^[a-f0-9]{6}$/i);
    const values = [0, 2, 4].map(offset => parseInt(expanded.slice(offset, offset + 2), 16) / 255)
      .map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4);
    return values[0] * .2126 + values[1] * .7152 + values[2] * .0722;
  };
  const x = luminance(a), y = luminance(b);
  return (Math.max(x, y) + .05) / (Math.min(x, y) + .05);
}
async function accessibility(page, label) {
  await page.evaluate(axeSource);
  const violations = await page.evaluate(`axe.run(document, {runOnly: {type: 'tag', values: ['wcag2a','wcag2aa','wcag21aa','wcag22aa']}})
    .then(r => r.violations.map(v => ({id:v.id,impact:v.impact,nodes:v.nodes.map(n=>({target:n.target,summary:n.failureSummary}))})))`);
  assert.deepEqual(violations, [], `${label}: accessibility violations`);
  const smallTargets = await page.evaluate(`Array.from(document.querySelectorAll('button,a[href],input,textarea')).filter(e =>
    !e.disabled && !e.closest('[inert]') && e.getClientRects().length && getComputedStyle(e).visibility === 'visible')
    .map(e=>({id:e.id || e.className,w:e.getBoundingClientRect().width,h:e.getBoundingClientRect().height}))
    .filter(r=>r.w<44 || r.h<44)`);
  assert.deepEqual(smallTargets, [], `${label}: controls below 44px`);
}
async function palette(page, scheme) {
  await page.call("Emulation.setEmulatedMedia", {features: [{name: "prefers-color-scheme", value: scheme}]});
  const values = await page.evaluate(`(() => {const s=getComputedStyle(document.documentElement); return Object.fromEntries(
    ['paper','sidebar','surface','ink','muted','green','green-hover','on-green','green-soft','control-border','error','warning','warning-soft','danger-soft','profile-ink','profile-bg',
      ...Array.from({length:6},(_,i)=>['avatar-'+i+'-ink','avatar-'+i+'-bg']).flat()]
      .map(n=>[n,s.getPropertyValue('--'+n).trim()]));})()`);
  assert.equal(await page.evaluate(`getComputedStyle(document.documentElement).colorScheme`), scheme);
  const pairs = [
    ["ink", "paper"], ["ink", "surface"], ["ink", "sidebar"],
    ["muted", "paper"], ["muted", "surface"], ["muted", "sidebar"], ["muted", "warning-soft"],
    ["green", "paper"], ["green", "green-soft"], ["on-green", "green"], ["on-green", "green-hover"],
    ["error", "paper"], ["error", "surface"], ["error", "danger-soft"],
    ["warning", "paper"], ["warning", "warning-soft"], ["profile-ink", "profile-bg"],
    ...Array.from({length: 6}, (_, i) => [`avatar-${i}-ink`, `avatar-${i}-bg`]),
  ];
  for (const [foreground, background] of pairs)
    assert(contrast(values[foreground], values[background]) >= 4.5, `${scheme}: ${foreground}/${background} text contrast`);
  for (const background of ["paper", "surface", "sidebar"]) {
    assert(contrast(values["control-border"], values[background]) >= 3, `${scheme}: control border/${background}`);
    assert(contrast(values.green, values[background]) >= 3, `${scheme}: focus/${background}`);
  }
  return Math.min(...pairs.map(([a, b]) => contrast(values[a], values[b])));
}
async function key(page, key, code, modifiers = 0) {
  const windowsVirtualKeyCode = {Enter: 13, Escape: 27, Tab: 9}[key];
  await page.call("Input.dispatchKeyEvent", {type: "keyDown", key, code, modifiers, windowsVirtualKeyCode,
    ...(key === "Enter" ? {text: "\r", unmodifiedText: "\r"} : {})});
  await page.call("Input.dispatchKeyEvent", {type: "keyUp", key, code, modifiers, windowsVirtualKeyCode});
}
try {
  const config = JSON.parse(await readFile(resolve(root, "wrangler.jsonc"), "utf8"));
  config.name = "aspen-room-browser-test";
  config.main = resolve(root, config.main);
  config.assets.directory = resolve(root, config.assets.directory);
  config.vars = {MODE: "decoded", HISTORY_LIMIT: "0", ALIASES: JSON.stringify({
    A: {backend: "shared", publicKey: "11".repeat(32), name: "Harbor", password: "room"},
    B: {backend: "shared", publicKey: "22".repeat(32), name: "Uplink", password: "room"},
    C: {backend: "field", publicKey: "33".repeat(32), name: "Field notes", password: "private"},
  }), FRONTENDS: "{}"};
  if (accountTest) {
    const fixture = JSON.parse(await readFile(resolve(root, "test/native-fixtures.json"), "utf8"));
    config.vars.MODE = "opaque";
    const aliases = JSON.parse(config.vars.ALIASES);
    aliases.A.publicKey = fixture.room.publicKey; aliases.B.publicKey = fixture.otherRoom.publicKey;
    config.vars.ALIASES = JSON.stringify(aliases);
    config.vars.ROOM_KEYS = JSON.stringify({A: fixture.room.key, B: fixture.otherRoom.key});
    config.vars.FRONTENDS = JSON.stringify({one: {token:"one",aliases:["A","B"]}});
    config.vars.WEB_USERS = JSON.stringify({
      alice: passwordRecord("fixture password only", "Alice", ["A","B","C"]),
      sam: passwordRecord("second fixture password", "Sam", ["A","B"]),
    });
  }
  await writeFile(resolve(scratch, "wrangler.json"), JSON.stringify(config));
  const worker = start(process.execPath, [resolve(root, "node_modules/wrangler/bin/wrangler.js"),
    "dev", "--local", "--config", resolve(scratch, "wrangler.json"), "--port", String(port), "--inspector-port", "0",
    "--persist-to", resolve(scratch, "state")]);
  await until(async () => {
    if (worker.child.exitCode !== null) throw new Error(worker.output());
    try {return (await fetch(`${base}/v1/web/rooms`)).ok;}
    catch {return false;}
  }, "local Worker startup", 30000);
  const chrome = start(process.env.CHROME_BIN ?? "google-chrome", [
    "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage",
    "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0",
    `--user-data-dir=${resolve(scratch, "chrome")}`,
  ]);
  await until(() => {
    if (errors.length) throw errors[0];
    if (chrome.child.exitCode !== null) throw new Error(chrome.output());
    return /DevTools listening on (ws:\/\/[^\s]+)/.test(chrome.output());
  }, "Chrome startup");
  cdp = await devtools(chrome.output().match(/DevTools listening on (ws:\/\/[^\s]+)/)[1]);
  const desktopContext = (await cdp.call("Target.createBrowserContext")).browserContextId;
  const mobileContext = (await cdp.call("Target.createBrowserContext")).browserContextId;
  const desktop = await page(desktopContext), mobile = await page(mobileContext, 390, 844);
  if (accountTest) {
    await accountsFlow(desktop, mobile);
  } else {
  for (const scheme of ["light", "dark"]) {
    await palette(desktop, scheme); await accessibility(desktop, `${scheme} desktop login`);
    await palette(mobile, scheme); await accessibility(mobile, `${scheme} mobile login`);
  }
  await desktop.join("Alice");
  await mobile.select("Uplink"); await mobile.join("Sam");
  await desktop.send("The mast is up. Good to have everyone in one room.");
  await mobile.sees("The mast is up. Good to have everyone in one room.");
  await mobile.send("Checking in from my phone. Same conversation, different connection.");
  await desktop.sees("Checking in from my phone. Same conversation, different connection.");
  assert.equal(await desktop.evaluate(`document.getElementById('message-announcement').textContent`),
    "Sam in Harbor: Checking in from my phone. Same conversation, different connection.");
  const contrastResults = {};
  for (const scheme of ["light", "dark"]) {
    contrastResults[scheme] = await palette(desktop, scheme);
    await palette(mobile, scheme);
    await accessibility(desktop, `${scheme} desktop conversation`);
    await accessibility(mobile, `${scheme} mobile conversation`);
    await desktop.evaluate(`document.getElementById('room-info').focus(); document.getElementById('room-info').click()`);
    await accessibility(desktop, `${scheme} room details`);
    await key(desktop, "Escape", "Escape");
    assert.equal(await desktop.evaluate("document.activeElement.id"), "room-info");
    await desktop.evaluate(`document.getElementById('edit-profile').click()`);
    await accessibility(desktop, `${scheme} profile dialog`);
    await key(desktop, "Escape", "Escape");
  }
  assert(await desktop.evaluate(`parseFloat(getComputedStyle(document.querySelector('.message-text')).fontSize) >= 16`));
  assert(await desktop.evaluate(`parseFloat(getComputedStyle(document.querySelector('.message time')).fontSize) >= 14`));
  await mobile.call("Emulation.setDeviceMetricsOverride", {width: 320, height: 844, deviceScaleFactor: 1, mobile: true});
  await accessibility(mobile, "320px reflow");
  assert.equal(await mobile.evaluate(`document.documentElement.scrollWidth <= innerWidth`), true);
  await mobile.evaluate(`document.querySelectorAll('*').forEach(e => {
    e.style.lineHeight='1.5'; e.style.letterSpacing='.12em'; e.style.wordSpacing='.16em';
    if(e.tagName==='P') e.style.marginBottom='2em';
  })`);
  await accessibility(mobile, "user text spacing");
  assert.equal(await mobile.evaluate(`document.documentElement.scrollWidth <= innerWidth`), true);
  await mobile.evaluate(`document.querySelectorAll('*').forEach(e => {
    for(const p of ['line-height','letter-spacing','word-spacing','margin-bottom']) e.style.removeProperty(p);
  })`);
  await mobile.call("Emulation.setEmulatedMedia", {features: [{name: "prefers-color-scheme", value: "dark"}, {name: "prefers-reduced-motion", value: "reduce"}]});
  assert.equal(await mobile.evaluate(`getComputedStyle(document.getElementById('sidebar')).transitionDuration`), "0s");
  await mobile.call("Emulation.setEmulatedMedia", {features: [{name: "forced-colors", value: "active"}]});
  await accessibility(mobile, "system high contrast");
  await palette(mobile, "dark");
  await desktop.evaluate(`document.documentElement.style.fontSize='200%'`);
  await accessibility(desktop, "200% text size");
  assert.equal(await desktop.evaluate(`document.documentElement.scrollWidth <= innerWidth`), true);
  await desktop.evaluate(`document.documentElement.style.fontSize=''`);
  await mobile.call("Emulation.setDeviceMetricsOverride", {width: 390, height: 844, deviceScaleFactor: 1, mobile: true});
  await mobile.evaluate(`document.getElementById('mobile-menu').focus()`);
  await key(mobile, "Enter", "Enter");
  assert.equal(await mobile.evaluate(`document.getElementById('main-content').inert && document.getElementById('sidebar').getAttribute('aria-modal') === 'true'`), true);
  await accessibility(mobile, "mobile channel drawer");
  await mobile.evaluate(`document.getElementById('edit-profile').focus()`);
  await key(mobile, "Tab", "Tab");
  assert.equal(await mobile.evaluate("document.activeElement.className"), "brand");
  await key(mobile, "Tab", "Tab", 8);
  assert.equal(await mobile.evaluate("document.activeElement.id"), "edit-profile");
  await key(mobile, "Escape", "Escape");
  assert.equal(await mobile.evaluate(`document.activeElement.id === 'mobile-menu' && document.getElementById('sidebar').inert`), true);
  await desktop.evaluate(`document.getElementById('message-input').focus()`);
  assert.equal(await desktop.evaluate(`getComputedStyle(document.querySelector('.composer')).outlineStyle`), "solid");
  await desktop.evaluate(`document.querySelector('.channel[data-room-id=A]').focus()`);
  await mobile.send("Keyboard focus stays put while messages arrive.");
  await desktop.sees("Keyboard focus stays put while messages arrive.");
  assert.equal(await desktop.evaluate("document.activeElement.dataset.roomId"), "A");
  assert.equal(await desktop.evaluate(`document.querySelectorAll('.message').length`), 3);
  assert.equal(await mobile.evaluate(`document.documentElement.scrollWidth <= innerWidth`), true);
  await desktop.type("x".repeat(145));
  assert.equal(await desktop.evaluate(`document.getElementById('send-button').disabled`), true);
  assert.equal(await desktop.evaluate(`document.getElementById('byte-count').textContent`), "152 / 151 bytes");
  assert.equal(await desktop.evaluate(`document.getElementById('message-input').getAttribute('aria-invalid')`), "true");
  await accessibility(desktop, "over-limit feedback");
  await desktop.type("");

  await mobile.evaluate(`document.getElementById('mobile-menu').click()`);
  assert.equal(await mobile.evaluate(`document.getElementById('sidebar').classList.contains('open')`), true);
  await mobile.select("Field notes"); await mobile.join("Sam", "private");
  assert.equal(await mobile.evaluate(`document.querySelectorAll('.message').length`), 0);
  await mobile.send("Private field notes stay in this room.");
  await desktop.send("Uplink is receiving the shared room while Field notes stays separate.");
  await mobile.wait(`document.querySelector('.channel-badge')?.textContent === '1'`, "inactive channel unread badge");
  assert.equal(await mobile.evaluate(`document.querySelectorAll('.message').length`), 1);
  await mobile.select("Uplink");
  await mobile.sees("Uplink is receiving the shared room while Field notes stays separate.");
  assert.equal(await mobile.evaluate(`document.querySelectorAll('.channel-badge').length`), 0);

  await cdp.call("Target.closeTarget", {targetId: mobile.targetId});
  await desktop.send("A message waiting while your browser is away.");
  const returning = await page(mobileContext, 390, 844);
  await returning.wait(`document.getElementById('connection-text').textContent === 'Connected'`, "saved login reconnect");
  await returning.sees("A message waiting while your browser is away.");

  let dropped = false;
  const remove = cdp.listen(event => {
    if (event.method !== "Fetch.requestPaused" || event.sessionId !== desktop.sessionId) return;
    if (event.params.request.url.endsWith("/A/posts") && !dropped) {
      dropped = true;
      desktop.call("Fetch.failRequest", {requestId: event.params.requestId, errorReason: "ConnectionReset"})
        .then(() => desktop.call("Fetch.disable")).catch(error => errors.push(error));
    }
  });
  await desktop.call("Fetch.enable", {patterns: [{urlPattern: "*/A/posts", requestStage: "Response"}]});
  await desktop.type("Saved once, even when the response goes missing.");
  await desktop.evaluate(`document.getElementById('compose-form').requestSubmit()`);
  await desktop.wait(`!document.getElementById('pending-post').hidden && !document.getElementById('retry-post').disabled`, "uncertain send state");
  assert.equal(dropped, true);
  await returning.sees("Saved once, even when the response goes missing.");
  remove();
  await desktop.evaluate(`fetch('/v1/web/rooms/A/logout', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'}).then(r => {if (!r.ok) throw new Error('Logout failed')})`);
  await desktop.wait(`!document.getElementById('join-panel').hidden`, "expired-login recovery");
  await desktop.open();
  await desktop.join("Alice North");
  await desktop.wait(`!document.getElementById('pending-post').hidden`, "persistent uncertain outbox");
  await desktop.evaluate(`document.getElementById('retry-post').click()`);
  await desktop.wait(`document.getElementById('pending-post').hidden`, "idempotent retry confirmation");
  assert.equal(await desktop.evaluate(`Array.from(document.querySelectorAll('.message-text')).filter(p => p.textContent === 'Saved once, even when the response goes missing.').length`), 1);
  await returning.send("All caught up. Web and mesh share the same history.");
  await desktop.sees("All caught up. Web and mesh share the same history.");
  const screenshotDirectory = process.env.BROWSER_SCREENSHOTS;
  if (screenshotDirectory) {
    await mkdir(resolve(screenshotDirectory), {recursive: true});
    await desktop.screenshot(resolve(screenshotDirectory, "rooms-desktop.png"));
    await returning.screenshot(resolve(screenshotDirectory, "rooms-mobile.png"));
  }
  await desktop.evaluate(`(async () => {for (let i = 0; i < 105; i++) {
    const response = await fetch('/v1/web/rooms/A/posts', {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({id: crypto.randomUUID(), text: 'History message ' + i})});
    if (!response.ok) throw new Error('History fixture post failed');
  }})()`);
  await desktop.open();
  await desktop.wait(`document.getElementById('connection-text').textContent === 'Connected'`, "paged history reconnect");
  assert.equal(await desktop.evaluate(`document.querySelectorAll('.message').length`), 100);
  assert.equal(await desktop.evaluate(`document.getElementById('load-older').hidden`), false);
  await desktop.evaluate(`document.getElementById('timeline').scrollTop = 0; document.getElementById('load-older').click()`);
  await desktop.wait(`document.querySelectorAll('.message').length === 112`, "earlier history paging");
  assert.equal(await desktop.evaluate(`document.getElementById('load-older').hidden`), true);
  assert.deepEqual(browserErrors, []);
  assert.deepEqual(errors, []);
  console.log("PASS: messaging, light/dark WCAG checks, 44px targets, 320px reflow, 200% text, keyboard focus, mobile drawer, announcements, reconnect and idempotent retry. Minimum palette text contrast:", contrastResults);
  }
} finally {
  cdp?.close();
  for (const child of children.reverse()) {
    if (child.exitCode === null) {
      const stopped = once(child, "exit");
      child.kill("SIGTERM");
      await Promise.race([stopped, sleep(5000)]);
      if (child.exitCode === null) {child.kill("SIGKILL"); await stopped;}
    }
  }
  await rm(scratch, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
