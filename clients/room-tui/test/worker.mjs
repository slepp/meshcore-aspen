import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {once} from "node:events";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {createServer} from "node:net";
import {resolve} from "node:path";
import {setTimeout as sleep} from "node:timers/promises";
import {passwordRecord} from "../../../services/shared-room/tools/web-users.mjs";

const root = resolve(import.meta.dirname, "..");
const service = resolve(root, "../../services/shared-room");
await mkdir(resolve(root, ".tmp"), {recursive: true});
const scratch = await mkdtemp(resolve(root, ".tmp/worker-"));
const probe = createServer();
probe.listen(0, "127.0.0.1");
await once(probe, "listening");
const port = probe.address().port;
await new Promise(done => probe.close(done));
const origin = `http://localhost:${port}`;
let worker, output = "";
async function command(executable, args, env = {}) {
  const child = spawn(executable, args, {cwd: root, env: {...process.env, ...env}, stdio: "inherit"});
  const code = await new Promise((done, reject) => {child.once("error", reject); child.once("exit", done);});
  assert.equal(code, 0, `${executable} failed`);
}
try {
  const config = JSON.parse(await readFile(resolve(service, "wrangler.jsonc"), "utf8"));
  const fixture = JSON.parse(await readFile(resolve(service, "test/native-fixtures.json"), "utf8"));
  config.name = "aspen-room-tui-test";
  config.main = resolve(service, config.main);
  config.assets.directory = resolve(service, config.assets.directory);
  config.vars = {
    MODE: "opaque", HISTORY_LIMIT: "0",
    ALIASES: JSON.stringify({
      A: {backend: "shared", publicKey: fixture.room.publicKey, name: "Harbor", password: "room"},
      B: {backend: "shared", publicKey: fixture.otherRoom.publicKey, name: "Uplink", password: "room"},
    }),
    ROOM_KEYS: JSON.stringify({A: fixture.room.key, B: fixture.otherRoom.key}),
    FRONTENDS: JSON.stringify({one: {token: "one", aliases: ["A", "B"]}}),
    WEB_USERS: JSON.stringify({alice: passwordRecord("fixture password only", "Alice", ["A", "B"])}),
  };
  await writeFile(resolve(scratch, "wrangler.json"), JSON.stringify(config));
  worker = spawn(process.execPath, [resolve(service, "node_modules/wrangler/bin/wrangler.js"),
    "dev", "--local", "--config", resolve(scratch, "wrangler.json"), "--port", String(port),
    "--inspector-port", "0", "--persist-to", resolve(scratch, "state")], {
    cwd: service, env: {...process.env, WRANGLER_SEND_METRICS: "false"}, stdio: ["ignore", "pipe", "pipe"],
  });
  worker.stdout.on("data", data => {output += data;});
  worker.stderr.on("data", data => {output += data;});
  let workerError;
  worker.on("error", error => {workerError = error;});
  const deadline = Date.now() + 30000;
  for (;;) {
    if (workerError) throw workerError;
    if (worker.exitCode !== null) throw Error(output);
    try {if ((await fetch(origin + "/v1/web/rooms")).ok) break;} catch {}
    if (Date.now() >= deadline) throw Error("Local Worker startup timed out\n" + output);
    await sleep(100);
  }
  await command("go", ["test", "-race", "-run", "^TestActualWorker", "-count=1", "./..."],
    {ASPEN_ROOM_TEST_ORIGIN: origin});
  const binary = resolve(scratch, "room-tui");
  await command("go", ["build", "-o", binary, "."]);
  await command("python3", [resolve(root, "test/terminal.py"), binary, origin, resolve(scratch, "desktop")]);
  const passwordFile = resolve(scratch, "fixture-password.txt");
  await writeFile(passwordFile, "fixture password only\n", {mode: 0o600});
  await command("python3", [resolve(root, "test/terminal.py"), binary, origin, "-",
    "--login-only", "alice", passwordFile, "A"], {XDG_CONFIG_HOME: resolve(scratch, "config")});
  console.log("PASS: native Worker device login, shared history, timed RF retry, lost-response UUID recovery and terminal login/post/restart.");
} finally {
  if (worker && worker.exitCode === null) {
    const closed = once(worker, "exit");
    worker.kill("SIGTERM");
    await closed;
  }
  await rm(scratch, {recursive: true, force: true});
}
