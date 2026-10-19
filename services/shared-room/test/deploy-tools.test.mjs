// setup.mjs, admin.mjs and doctor.mjs against a mocked Wrangler and a local
// stand-in for the Worker's /v1/info and /v1/admin routes. Nothing is deployed.
import {test, before, after} from "node:test";
import assert from "node:assert/strict";
import {execFile} from "node:child_process";
import {createServer} from "node:http";
import {createHash} from "node:crypto";
import {cp, mkdtemp, mkdir, readFile, rm, stat, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {fileURLToPath} from "node:url";
import {stripJsonc} from "../tools/deploy-common.mjs";

const here = fileURLToPath(new URL("..", import.meta.url));
const scratch = await mkdtemp(join(tmpdir(), "aspen-tools-"));
const pkg = join(scratch, "pkg");
const mock = join(scratch, "wrangler-mock.mjs");
const log = join(scratch, "wrangler.jsonl");
const state = join(scratch, "wrangler-state.json");
const failFlag = join(scratch, "fail-deploy");
let server, origin;
const adminCalls = [];
// The stand-in Worker learns ADMIN_TOKEN_HASH the way the real one does: from the secret upload.
const currentHash = async () => JSON.parse(await readFile(state, "utf8").catch(() => "{}")).adminHash ?? null;

const mockSource = `
import {appendFileSync, readFileSync, writeFileSync, existsSync} from "node:fs";
const args = process.argv.slice(2);
let stdin = ""; for await (const c of process.stdin) stdin += c;
appendFileSync(${JSON.stringify(log)}, JSON.stringify({args, stdin}) + "\\n");
const st = existsSync(${JSON.stringify(state)}) ? JSON.parse(readFileSync(${JSON.stringify(state)}, "utf8")) : {secrets: []};
if (args[0] === "whoami") process.exit(0);
if (args[0] === "deploy" && existsSync(${JSON.stringify(failFlag)})) process.exit(1);
if (args[0] === "deploy") {console.log("Deployed worker\\n  https://aspen-test.example.workers.dev"); process.exit(0);}
if (args[0] === "secret" && args[1] === "list") {console.log(JSON.stringify(st.secrets.map(name => ({name, type: "secret_text"})))); process.exit(0);}
if (args[0] === "secret" && args[1] === "put") {
  if (!st.secrets.includes(args[2])) st.secrets.push(args[2]);
  if (args[2] === "ADMIN_TOKEN_HASH") st.adminHash = stdin;
  writeFileSync(${JSON.stringify(state)}, JSON.stringify(st)); process.exit(0);
}
process.exit(2);
`;

before(async () => {
  await mkdir(pkg);
  await cp(join(here, "tools"), join(pkg, "tools"), {recursive: true});
  // The checked-in config gains migration v3 and the REGISTRY binding with the
  // hosted-room Worker; add them here so the tools run against either tree.
  const base = JSON.parse(stripJsonc(await readFile(join(here, "wrangler.jsonc"), "utf8")));
  if (!base.migrations.some(m => m.tag === "v3")) base.migrations.push({tag: "v3", new_sqlite_classes: ["Registry"]});
  if (!base.durable_objects.bindings.some(b => b.name === "REGISTRY")) base.durable_objects.bindings.push({name: "REGISTRY", class_name: "Registry"});
  await writeFile(join(pkg, "wrangler.jsonc"), JSON.stringify(base, null, 2));
  await cp(join(here, "src"), join(pkg, "src"), {recursive: true});
  await writeFile(mock, mockSource);
  server = createServer(async (req, res) => {
    let body = ""; for await (const c of req) body += c;
    const reply = (status, json) => {res.writeHead(status, {"content-type": "application/json"}); res.end(JSON.stringify(json));};
    if (req.url === "/v1/info") return reply(200, {service: "aspen-shared-room", version: "0.1.20", protocol: "aspen-node.v1", signup: "code", time: 1790000000});
    if (req.url.startsWith("/v1/admin/")) {
      const token = (req.headers.authorization ?? "").replace(/^Bearer /, "");
      const adminHash = await currentHash();
      if (!adminHash) return reply(404, {ok: 0, e: "not_found", m: "Not found."});
      if (createHash("sha256").update(token).digest("hex") !== adminHash) return reply(401, {ok: 0, e: "auth", m: "Unauthorized."});
      const json = JSON.parse(body);
      adminCalls.push({path: req.url, body: json});
      if (req.url === "/v1/admin/mint") return reply(200, {ok: 1, codes: ["ABCD-EFGH-JKMN-PQRS"], expires: 1790604800});
      if (req.url === "/v1/admin/rooms") return reply(200, {ok: 1, rooms: [{alias: "r2q7x3bndkamw4qzfxe7abcdmn2xyz", name: "Fleet", state: "active", created: 1790000000, last_active: 1790000100, owner: "ab12cd34"}], next: null});
      if (req.url === "/v1/admin/invite") return reply(200, {ok: 1, code: "WXYZ-0123-4567-89AB", ttl: 900});
      if (req.url === "/v1/admin/audit") return reply(200, {ok: 1, rows: [{id: 1, ts: 1790000000, actor: "admin", action: "mint", target: "-", result: "ok"}], next: null});
      return reply(200, {ok: 1});
    }
    reply(404, {ok: 0, e: "not_found", m: "Not found."});
  });
  await new Promise(r => server.listen(0, "127.0.0.1", r));
  origin = `http://127.0.0.1:${server.address().port}`;
});

after(async () => {server.close(); await rm(scratch, {recursive: true, force: true});});

function run(script, args = [], input = "") {
  return new Promise(resolve => {
    const child = execFile(process.execPath, [join(pkg, "tools", script), ...args], {
      cwd: pkg, env: {...process.env, ASPEN_WRANGLER: mock},
    }, (error, stdout, stderr) => resolve({code: error ? error.code : 0, stdout, stderr}));
    child.stdin.end(input);
  });
}
const calls = async () => (await readFile(log, "utf8").catch(() => "")).trim().split("\n").filter(Boolean).map(l => JSON.parse(l));
const mode = async p => (await stat(p)).mode & 0o777;

test("setup deploys, stores secrets through stdin only and prints a create command", async () => {
  const r = await run("setup.mjs", ["--name", "aspen-test", "--url", origin]);
  assert.equal(r.code, 0, r.stderr);
  const kekText = await readFile(join(pkg, ".deploy/room-kek.json"), "utf8");
  const token = (await readFile(join(pkg, ".deploy/admin-token"), "utf8")).trim();
  const kek = JSON.parse(kekText);
  assert.equal(kek.current, "k1");
  assert.match(kek.keys.k1, /^[a-f0-9]{64}$/);
  assert.match(token, /^[a-f0-9]{64}$/);
  for (const f of [".deploy/room-kek.json", ".deploy/admin-token"]) assert.equal(await mode(join(pkg, f)), 0o600);
  assert.equal(await mode(join(pkg, ".deploy")), 0o700);

  const all = await calls();
  const puts = all.filter(c => c.args[0] === "secret" && c.args[1] === "put");
  assert.deepEqual(puts.map(c => c.args[2]).sort(), ["ADMIN_TOKEN_HASH", "ROOM_KEK"]);
  assert.equal(JSON.parse(puts.find(c => c.args[2] === "ROOM_KEK").stdin).keys.k1, kek.keys.k1);
  const adminHash = puts.find(c => c.args[2] === "ADMIN_TOKEN_HASH").stdin;
  assert.equal(adminHash, createHash("sha256").update(token).digest("hex"));
  for (const c of all) for (const secret of [kek.keys.k1, token, adminHash])
    assert.equal(c.args.join(" ").includes(secret), false, "a secret reached argv");
  assert.equal(r.stdout.includes(kek.keys.k1) || r.stdout.includes(token), false, "a secret was printed");
  assert.match(r.stdout, /Copy \.deploy\/room-kek\.json to your password manager/);
  assert.match(r.stdout, /cloudroom create ABCD-EFGH-JKMN-PQRS@127\.0\.0\.1:\d+ My Room/);

  const local = JSON.parse(await readFile(join(pkg, "wrangler.local.jsonc"), "utf8").then(t => t.replace(/^\/\/.*\n/, "")));
  assert.equal(local.name, "aspen-test");
  assert.equal(local.routes, undefined);
  assert.equal(local.workers_dev, true);
  assert.equal(local.observability.enabled, false);
  assert.equal(local.vars.SIGNUP_MODE, "code");
});

test("rerunning keeps secrets and uploads nothing new", async () => {
  const before = {kek: await readFile(join(pkg, ".deploy/room-kek.json"), "utf8"), token: await readFile(join(pkg, ".deploy/admin-token"), "utf8")};
  const count = (await calls()).length;
  const r = await run("setup.mjs", ["--url", origin]);
  assert.equal(r.code, 0, r.stderr);
  assert.equal(await readFile(join(pkg, ".deploy/room-kek.json"), "utf8"), before.kek);
  assert.equal(await readFile(join(pkg, ".deploy/admin-token"), "utf8"), before.token);
  const added = (await calls()).slice(count);
  assert.equal(added.some(c => c.args[0] === "secret" && c.args[1] === "put"), false);
  assert.equal(added.some(c => c.args[0] === "deploy"), true);
  assert.equal(r.stdout.includes("cloudroom create"), false, "a rerun mints no code unless asked");
  const renamed = await run("setup.mjs", ["--name", "other", "--url", origin]);
  assert.equal(renamed.code, 1);
  assert.match(renamed.stderr, /already names the Worker "aspen-test"/);
});

test("a rerun after a failed first deploy still mints the create code and reminds about the key backup", async () => {
  await rm(join(pkg, ".deploy/url"));
  await writeFile(failFlag, "");
  const failed = await run("setup.mjs", ["--url", origin]);
  assert.equal(failed.code, 1);
  await rm(failFlag);
  const r = await run("setup.mjs", ["--url", origin]);
  assert.equal(r.code, 0, r.stderr);
  assert.match(r.stdout, /Copy \.deploy\/room-kek\.json to your password manager/);
  assert.match(r.stdout, /cloudroom create ABCD-EFGH-JKMN-PQRS@/);
});

test("rotate adds a key id and keeps the old key; restore re-uploads a backup", async () => {
  const old = JSON.parse(await readFile(join(pkg, ".deploy/room-kek.json"), "utf8"));
  const r = await run("setup.mjs", ["--rotate-kek"]);
  assert.equal(r.code, 0, r.stderr);
  const next = JSON.parse(await readFile(join(pkg, ".deploy/room-kek.json"), "utf8"));
  assert.equal(next.current, "k2");
  assert.equal(next.keys.k1, old.keys.k1);
  assert.match(next.keys.k2, /^[a-f0-9]{64}$/);
  const put = (await calls()).filter(c => c.args[2] === "ROOM_KEK").at(-1);
  assert.deepEqual(JSON.parse(put.stdin), next);

  const backup = join(scratch, "kek-backup.json");
  await writeFile(backup, JSON.stringify(old) + "\n", {mode: 0o600});
  const restored = await run("setup.mjs", ["--restore-kek", backup]);
  assert.equal(restored.code, 0, restored.stderr);
  assert.deepEqual(JSON.parse(await readFile(join(pkg, ".deploy/room-kek.json"), "utf8")), old);
  assert.deepEqual(JSON.parse((await calls()).filter(c => c.args[2] === "ROOM_KEK").at(-1).stdin), old);

  const loose = join(scratch, "kek-loose.json");
  await writeFile(loose, JSON.stringify(old), {mode: 0o644});
  const refused = await run("setup.mjs", ["--restore-kek", loose]);
  assert.equal(refused.code, 1);
  assert.match(refused.stderr, /mode 0600/);
});

test("admin commands read the token file, validate arguments and print results", async () => {
  const mint = await run("admin.mjs", ["--url", origin, "mint", "--count", "1"]);
  assert.equal(mint.code, 0, mint.stderr);
  assert.match(mint.stdout, /^ABCD-EFGH-JKMN-PQRS$/m);
  const rooms = await run("admin.mjs", ["--url", origin, "rooms"]);
  assert.match(rooms.stdout, /r2q7x3bndkamw4qzfxe7abcdmn2xyz\s+active/);
  const invite = await run("admin.mjs", ["--url", origin, "invite", "r2q7x3bndkamw4qzfxe7abcdmn2xyz"]);
  assert.match(invite.stdout, /^WXYZ-0123-4567-89AB$/m);
  assert.deepEqual(adminCalls.at(-1), {path: "/v1/admin/invite", body: {alias: "r2q7x3bndkamw4qzfxe7abcdmn2xyz", role: "owner"}});
  assert.equal((await run("admin.mjs", ["--url", origin, "delete", "r2q7x3bndkamw4qzfxe7abcdmn2xyz"])).code, 1);
  assert.equal((await run("admin.mjs", ["--url", origin, "delete", "r2q7x3bndkamw4qzfxe7abcdmn2xyz", "--yes"])).code, 0);
  assert.deepEqual(adminCalls.at(-1).body, {alias: "r2q7x3bndkamw4qzfxe7abcdmn2xyz", action: "delete"});
  assert.equal((await run("admin.mjs", ["--url", origin, "ban", "not-hex"])).code, 1);
  assert.equal((await run("admin.mjs", ["--url", origin, "ban", "ab".repeat(32), "--until", "1790000000"])).code, 0);
  assert.deepEqual(adminCalls.at(-1).body, {node: "ab".repeat(32), action: "ban", until: 1790000000});
  assert.equal((await run("admin.mjs", ["--url", origin, "revoke-code", "375c09ce"])).code, 0);
});

test("admin reads the token from stdin and never prints it", async () => {
  const token = (await readFile(join(pkg, ".deploy/admin-token"), "utf8")).trim();
  await rm(join(pkg, ".deploy/admin-token"));
  const ok = await run("admin.mjs", ["--url", origin, "--token-stdin", "audit"], token + "\n");
  assert.equal(ok.code, 0, ok.stderr);
  const wrong = await run("admin.mjs", ["--url", origin, "--token-stdin", "audit"], "f".repeat(64) + "\n");
  assert.equal(wrong.code, 1);
  assert.equal((wrong.stdout + wrong.stderr).includes("f".repeat(64)), false);
  assert.match(wrong.stderr, /auth/);
  await writeFile(join(pkg, ".deploy/admin-token"), token + "\n", {mode: 0o600});
});

test("admin refuses a token file that other users can read", async () => {
  const path = join(pkg, ".deploy/admin-token");
  const token = await readFile(path, "utf8");
  await rm(path);
  await writeFile(path, token, {mode: 0o644});
  const r = await run("admin.mjs", ["--url", origin, "rooms"]);
  assert.equal(r.code, 1);
  assert.match(r.stderr, /mode 0600/);
  await rm(path);
  await writeFile(path, token, {mode: 0o600});
});

test("doctor reports a healthy deployment and warns when logs are on", async () => {
  const good = await run("doctor.mjs", ["--url", origin]);
  assert.equal(good.code, 0, good.stdout + good.stderr);
  assert.match(good.stdout, /ok .*aspen-shared-room 0\.1\.20/);
  assert.match(good.stdout, /ok .*Operator API accepts/);
  const path = join(pkg, "wrangler.local.jsonc");
  const text = await readFile(path, "utf8");
  await writeFile(path, text.replace('"enabled": false', '"enabled": true'));
  const warned = await run("doctor.mjs", ["--url", origin]);
  assert.match(warned.stdout, /warn .*Workers Logs/);
  await writeFile(path, text);
});

test("doctor fails when the operator API is off", async () => {
  const st = JSON.parse(await readFile(state, "utf8"));
  delete st.adminHash;
  await writeFile(state, JSON.stringify(st));
  const r = await run("doctor.mjs", ["--url", origin]);
  assert.equal(r.code, 1);
  assert.match(r.stdout, /FAIL .*Operator API is off/);
});

test("configure --worker checks the key file against the named Worker", async () => {
  const dir = await mkdtemp(join(scratch, "cfg-"));
  const config = join(dir, "c.json"), keys = join(dir, "k.json");
  await writeFile(config, JSON.stringify({ALIASES: {A: {backend: "b", publicKey: "22351e22105a19aabb42589162ad7f1ea0df1c25cebf0e4a9fcd261301274862", name: "A", password: "room"}},
    FRONTENDS: {one: {token: "public-local-fixture-token-00000001", aliases: ["A"]}}}), {mode: 0o600});
  await writeFile(keys, JSON.stringify({worker: "aspen-test", keyLocation: "worker",
    expandedKeys: {A: "18bf5297036bf9a0bdce8f050f58e57e50413a49ae37e7fb74a044e8b48363632a2ce0aed069264b9a988490fb6cca0cce7285994ad29f6ad23c2920fbb76f47"}}), {mode: 0o600});
  const tool = join(here, "tools/configure.mjs");
  const run2 = args => new Promise(resolve => execFile(process.execPath, [tool, ...args], (e, so, se) => resolve({code: e ? e.code : 0, so, se})));
  const named = await run2(["--file", config, "--keys", keys, "--validate", "--worker", "aspen-test"]);
  assert.equal(named.code, 0, named.se);
  const mismatch = await run2(["--file", config, "--keys", keys, "--validate"]);
  assert.equal(mismatch.code, 1);
  const badName = await run2(["--file", config, "--keys", keys, "--validate", "--worker", "Bad Name"]);
  assert.equal(badName.code, 1);
});
