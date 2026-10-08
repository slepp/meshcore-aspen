import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {once} from "node:events";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {resolve} from "node:path";
import {createServer} from "node:net";
import {setTimeout as sleep} from "node:timers/promises";

const root = resolve(import.meta.dirname, "..");
const scratchRoot = resolve(root, ".tmp");
await mkdir(scratchRoot, {recursive: true});
const scratch = await mkdtemp(resolve(scratchRoot, "web-browser-"));
const children = [], errors = [], browserErrors = [];
let cdp;
const portProbe = createServer();
portProbe.listen(0, "127.0.0.1");
await once(portProbe, "listening");
const port = portProbe.address().port;
await new Promise(resolveClose => portProbe.close(resolveClose));
const base = `http://localhost:${port}`;

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
      await evaluate(`document.getElementById('display-name').value = ${JSON.stringify(name)};
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
  await desktop.join("Alice");
  await mobile.select("Uplink"); await mobile.join("Sam");
  await desktop.send("The mast is up. Good to have everyone in one room.");
  await mobile.sees("The mast is up. Good to have everyone in one room.");
  await mobile.send("Checking in from my phone. Same conversation, different connection.");
  await desktop.sees("Checking in from my phone. Same conversation, different connection.");
  assert.equal(await desktop.evaluate(`document.querySelectorAll('.message').length`), 2);
  assert.equal(await mobile.evaluate(`document.documentElement.scrollWidth <= innerWidth`), true);
  await desktop.type("x".repeat(145));
  assert.equal(await desktop.evaluate(`document.getElementById('send-button').disabled`), true);
  assert.equal(await desktop.evaluate(`document.getElementById('byte-count').textContent`), "152 / 151 bytes");
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
  await desktop.wait(`document.querySelectorAll('.message').length === 111`, "earlier history paging");
  assert.equal(await desktop.evaluate(`document.getElementById('load-older').hidden`), true);
  assert.deepEqual(browserErrors, []);
  assert.deepEqual(errors, []);
  console.log("PASS: desktop/mobile login, shared aliases, independent channels, live messages, RF byte limits, unread badges, reconnect, paged history, durable uncertain sends, re-login/name changes and idempotent retry.");
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
  await rm(scratch, {recursive: true, force: true});
}
