// End-to-end lifecycle test: Aspen node code against this Worker.
//
// The firmware side is the real lifecycle code (grammar, link engine, saved records, signing)
// built as a loopback agent by `make -C firmware/esp32 cloudroom-e2e-agent`; the server side is
// this repository's Worker under `wrangler dev --local`. TLS is the only stand-in. Several
// fixture devices run in one agent process. All keys, codes and tokens are public fixtures.
//
//   make -C firmware/esp32 cloudroom-e2e-agent
//   E2E_AGENT=.tmp/onchip-tests/<variant>/cloudroom-e2e-agent \
//   E2E_AGENT_PROVIDER=.tmp/onchip-tests/<variant>/cloudroom-e2e-agent-provider \
//     node --test test/e2e/lifecycle.test.mjs
//
// It takes about two minutes: one case waits out the 60 s minimum lifetime of an invite code.
import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {createHash, randomBytes} from "node:crypto";
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from "node:fs";
import {request as httpRequest} from "node:http";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {after, before, describe, test} from "node:test";
import {createInterface} from "node:readline";
import {fileURLToPath} from "node:url";
import {nodeFromSeed, post, redeemBody} from "../../tools/node-sim.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const AGENT = process.env.E2E_AGENT, AGENT_PROVIDER = process.env.E2E_AGENT_PROVIDER;
const skip = !AGENT || !existsSync(AGENT) ? "set E2E_AGENT to a built cloudroom-e2e-agent" : false;
const wrangler = join(root, "node_modules/.bin/wrangler");

const HOSTED = "rooms.e2e.test", SELF = "self.e2e.test", ADMIN = "e2e-public-fixture-admin-token";
// The static fixture room of .dev.vars.fixture stands in for a legacy compile-time provider alias.
const FIXTURE = Object.fromEntries(readFileSync(join(root, ".dev.vars.fixture"), "utf8").split("\n")
  .filter(line => /^[A-Z_]+=/.test(line)).map(line => [line.split("=")[0], line.slice(line.indexOf("=") + 1).replace(/^(['"])(.*)\1$/, "$2")]));
const LEGACY_PUB = JSON.parse(FIXTURE.ALIASES).A.publicKey, LEGACY_TOKEN = JSON.parse(FIXTURE.FRONTENDS).one.token;

const sha256 = text => createHash("sha256").update(text).digest("hex");
const randomCode = () => Array.from(randomBytes(16), b => "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[b % 32]).join("");
const sleep = ms => new Promise(done => setTimeout(done, ms));

// ---- servers ----------------------------------------------------------------------------
class Server {
  constructor(name, port, vars) { this.name = name; this.port = port; this.vars = vars; }
  get base() { return `http://127.0.0.1:${this.port}`; }
  async start(dir) {
    const config = JSON.parse(readFileSync(join(root, "wrangler.jsonc"), "utf8"));
    delete config.routes; delete config.build; delete config.$schema;
    config.main = join(root, "src/index.ts");
    config.assets.directory = join(root, "web");
    const folder = join(dir, this.name);
    mkdirSync(folder, {recursive: true});
    writeFileSync(join(folder, "wrangler.jsonc"), JSON.stringify(config));
    writeFileSync(join(folder, ".dev.vars"), Object.entries(this.vars).map(([k, v]) => `${k}='${v}'`).join("\n") + "\n");
    this.process = spawn(wrangler, ["dev", "--local", "--port", String(this.port), "--inspector-port", String(this.port + 100), "--persist-to", join(folder, "state"),
      "--config", join(folder, "wrangler.jsonc")], {cwd: folder, stdio: ["ignore", "pipe", "pipe"], env: {...process.env, CI: "1", WRANGLER_SEND_METRICS: "false"}});
    let log = "";
    this.process.stdout.on("data", d => { log += d; });
    this.process.stderr.on("data", d => { log += d; });
    const until = Date.now() + 300000;
    while (Date.now() < until) {
      try { if ((await fetch(`${this.base}/v1/info`)).ok) return; } catch {}
      await sleep(500);
    }
    throw new Error(`${this.name} did not start:\n${log.slice(-1500)}`);
  }
  stop() { this.process?.kill("SIGTERM"); }
  admin(path, body) { return post(this.base, `/v1/admin/${path}`, body, ADMIN); }
  async mint(count = 1) { const r = await this.admin("mint", {kind: "create", count}); assert.equal(r.status, 200); return r.body.codes; }
  control(alias, token, body) { return post(this.base, `/v1/aliases/${alias}/control`, body, token); }
  info() { return fetch(`${this.base}/v1/info`).then(r => r.json()); }
}
const kek = byte => JSON.stringify({current: "k1", keys: {k1: byte.repeat(32)}});
const adminHash = sha256(ADMIN);

// A WebSocket upgrade with a device token. Resolves to the HTTP status of the answer.
function upgrade(server, alias, token) {
  return new Promise((done, fail) => {
    const req = httpRequest({host: "127.0.0.1", port: server.port, path: `/v1/aliases/${alias}/socket`, headers: {
      Connection: "Upgrade", Upgrade: "websocket", "Sec-WebSocket-Version": "13", "Sec-WebSocket-Key": randomBytes(16).toString("base64"),
      "Sec-WebSocket-Protocol": "aspen-room.v2.json", Authorization: `Bearer ${token}`}});
    req.on("upgrade", (res, socket) => { socket.destroy(); done(res.statusCode); });
    req.on("response", res => { res.resume(); done(res.statusCode); });
    req.on("error", fail);
    req.end();
  });
}

// ---- device agent -------------------------------------------------------------------------
class Agent {
  constructor(binary, env = {}) {
    this.process = spawn(binary, [], {stdio: ["pipe", "pipe", "inherit"], env: {...process.env, ...env}});
    this.lines = [];
    this.waiters = [];
    createInterface({input: this.process.stdout}).on("line", line => {
      const waiter = this.waiters.shift();
      if (waiter) waiter(line); else this.lines.push(line);
    });
  }
  send(command) {
    this.process.stdin.write(command + "\n");
    return new Promise(done => { const queued = this.lines.shift(); if (queued !== undefined) done(queued); else this.waiters.push(done); });
  }
  async device(name) { assert.equal(await this.send(`device ${name}`), "OK"); }
  async say(text) {
    const code = text.match(/^(?:join|create|convert) ([0-9A-Za-z-]{16,19})(?:@|\s|$)/)?.[1];
    if (code && code.toLowerCase() !== "open") secrets.add(code.toUpperCase().replace(/-/g, ""));
    const reply = await this.send(`say ${text}`);
    const password = text.match(/^password (\S+)$/)?.[1];
    if (password && !["open", "new"].includes(password)) secrets.add(password);
    return reply;
  }
  // Queue a job, run it and return its result text and the restart state.
  async run(text) {
    const queued = await this.say(text);
    assert.match(queued, /^Contacting /, `command not queued: ${queued}`);
    await this.send("pump");
    return this.say("result");
  }
  async restart() { assert.equal(await this.send("restart"), "OK"); }
  async saved() {
    const text = await this.send("saved");
    if (text === "none" || text === "invalid") return {text, aliases: []};
    const [head, ...rows] = text.split(" | ");
    for (const row of rows) secrets.add(row.match(/ token=(\S+)/)[1]);
    const fields = row => Object.fromEntries(row.split(" ").map(pair => [pair.slice(0, pair.indexOf("=")), pair.slice(pair.indexOf("=") + 1)]));
    return {text, ...fields(head), aliases: rows.map(fields)};
  }
  async state() { return Object.fromEntries((await this.send("state")).split(" ").map(p => p.split("="))); }
  stop() { this.process.stdin.write("quit\n"); this.process.kill(); }
}
const secrets = new Set(["FIXTUREPW"]);
const codeIn = text => text.match(/\b([0-9A-Z]{4}(?:-[0-9A-Z]{4}){3})\b/)?.[1];

// ---- the lifecycle -------------------------------------------------------------------------
describe("shared-room lifecycle, node to Worker", {skip}, () => {
  let dir, hosted, self, agent;
  before(async () => {
    dir = mkdtempSync(join(tmpdir(), "e2e-"));
    hosted = new Server("hosted", 8841, {ROOM_KEK: kek("ab"), ADMIN_TOKEN_HASH: adminHash, SIGNUP_MODE: "code", ...FIXTURE});
    self = new Server("self", 8842, {ROOM_KEK: kek("cd"), ADMIN_TOKEN_HASH: adminHash, SIGNUP_MODE: "open"});
    await Promise.all([hosted.start(dir), self.start(dir)]);
    agent = new Agent(AGENT);
    await agent.send(`host ${HOSTED} ${hosted.port}`);
    await agent.send(`host ${SELF} ${self.port}`);
  });
  after(() => { agent?.stop(); hosted?.stop(); self?.stop(); if (dir) rmSync(dir, {recursive: true, force: true}); });

  let room;   // the room created on node A

  test("a server answers the probe and reports its sign-up mode", async () => {
    await agent.device("A");
    assert.match(await agent.say(`probe ${HOSTED}`), /^Contacting /);
    await agent.send("pump");
    assert.match(await agent.say("result"), new RegExp(`^Server ${HOSTED} ok: aspen-shared-room 0\\.1\\.20 signup=code`));
    assert.equal((await self.info()).signup, "open");
  });

  test("create: node A makes a room with a code from the server operator", async () => {
    const [code] = await hosted.mint();
    assert.match(await agent.run(`create ${code}@${HOSTED} Fleet`), /^Linked\. Restarts in 10 s\./);
    assert.equal((await agent.state()).restarts, "1");
    await agent.restart();
    const saved = await agent.saved();
    assert.equal(saved.magic, "v2");
    const [alias] = saved.aliases;
    assert.equal(alias.host, HOSTED);
    assert.equal(alias.role, "1");           // owner
    assert.equal(alias.ca, "0");             // system roots
    assert.equal(alias.flags, "1");          // advert once
    room = {alias: alias.id, name: alias.name, token: alias.token, ownerPub: alias.pub};
    assert.equal(alias.name, "Fleet");
    assert.equal(await upgrade(hosted, room.alias, room.token), 101, "owner token opens the room socket");
    assert.equal(await upgrade(hosted, room.alias, "ar1_" + "0".repeat(64)), 401, "an unknown token does not");
    await agent.say("password"); await agent.send("pump");
    assert.match(await agent.say("result"), /^Room password [0-9A-Z]{10} /, "the server generated a password");
  });

  test("join: node B enters the room by pasting an owner's code", async () => {
    const invited = await agent.say("invite");
    assert.match(invited, /^Contacting /);
    await agent.send("pump");
    const text = await agent.say("result");
    const code = codeIn(text);
    assert.ok(code, `no code in: ${text}`);
    assert.match(text, new RegExp(`cloudroom join ${code.replace(/-/g, "-")}@${HOSTED}|cloudroom join ${code}`));
    await agent.device("B");
    assert.match(await agent.run(`join ${code}@${HOSTED}`), /^Linked\. Restarts in 10 s\. Ask the room owner for the password/);
    await agent.restart();
    const [alias] = (await agent.saved()).aliases;
    assert.equal(alias.id, room.alias);
    assert.equal(alias.role, "0");
    assert.equal(alias.flags, "0");
    assert.notEqual(alias.token, room.token, "each node has its own token");
    assert.equal(await agent.say("invite"), "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\".");
    room.frontendToken = alias.token;
    room.frontendPub = await agent.send("node?");   // management public key
    assert.equal(await upgrade(hosted, room.alias, alias.token), 101);
    // The code was single use.
    await agent.device("B2");
    assert.match(await agent.run(`join ${code}@${HOSTED}`), /^Link failed: code already used/);
    assert.equal((await agent.saved()).text, "none");
  });

  test("owner tools: nodes, password, revoke", async () => {
    await agent.device("A");
    await agent.say("nodes"); await agent.send("pump");
    assert.match(await agent.say("result"), /^Nodes 1-2 of 2:/);
    await agent.say("password SecretPW1"); await agent.send("pump");
    assert.match(await agent.say("result"), /SecretPW1/);
    // The frontend cannot read or change the password.
    await agent.device("B");
    assert.match(await agent.say("password"), /^Only an owner node/);
  });

  test("convert: node C turns its local room into a shared room", async () => {
    await agent.device("C");
    assert.equal(await agent.send("room Backyard FIXTUREPW"), "OK");
    assert.match(await agent.send("room?"), /^local pw=FIXTUREPW/);
    const roomPub = (await agent.send("room?")).match(/pub=([0-9a-f]{64})/)[1];
    const [code] = await hosted.mint();
    // The preview names the server in full and sends nothing.
    const before = (await hosted.info()).time;
    assert.match(await agent.say(`convert ${code}@${HOSTED}`),
      new RegExp(`^Key goes to ${HOSTED}; it can speak as the room\\. Admin logins stay\\. Add "yes ${HOSTED}"\\.$`));
    assert.equal((await agent.saved()).text, "none");
    assert.ok(before);
    assert.match(await agent.run(`convert ${code}@${HOSTED} yes ${HOSTED}`), /^Linked\. Restarts in 10 s\./);
    await agent.restart();
    const saved = await agent.saved();
    const [alias] = saved.aliases;
    assert.equal(alias.pub, roomPub, "the server holds the node's own room key");
    assert.equal(alias.role, "1");
    assert.equal(alias.flags, "3");           // converted + advert once
    assert.match(await agent.send("room?"), /^cloud /, "the local Room role stops while the shared room serves it");
    assert.equal(await upgrade(hosted, alias.id, alias.token), 101);
    // Members keep their password: the server holds the one the room had.
    await agent.say("password"); await agent.send("pump");
    assert.match(await agent.say("result"), /FIXTUREPW/);
    room.converted = {alias: alias.id, token: alias.token, pub: roomPub};
  });

  test("unlink: a converted node hands the room back, and the guard stops a split room", async () => {
    // Another node joins the converted room, so unlinking C would leave the server copy live.
    await agent.say("invite owner"); await agent.send("pump");
    const code = codeIn(await agent.say("result"));
    assert.ok(code);
    await agent.device("D1");
    assert.match(await agent.run(`join ${code}@${HOSTED}`), /^Linked\./);
    await agent.restart();
    await agent.device("C");
    assert.match(await agent.say("unlink"), /Contacting|Room stays live/);
    await agent.send("pump");
    const refused = await agent.say("result");
    assert.match(refused, /Other nodes still use this room|Room stays live|Owner added less than 24 h/);
    assert.equal((await agent.saved()).count, "1", "nothing changed on a refusal");
    // D1 leaves, then C unlinks and the local Room resumes with its original password.
    await agent.device("D1");
    assert.match(await agent.run("unlink"), /^Unlinked\./);
    await agent.restart();
    assert.equal((await agent.saved()).count, "0");
    await agent.device("C");
    assert.match(await agent.run("unlink"), /^Unlinked\./);
    await agent.restart();
    assert.equal((await agent.saved()).count, "0");
    assert.match(await agent.send("room?"), /^local pw=FIXTUREPW/);
    assert.equal(await upgrade(hosted, room.converted.alias, room.converted.token), 403, "the server revoked the node");
  });

  test("own server: the same flow against a self-hosted Worker with open sign-up", async () => {
    await agent.device("E");
    assert.match(await agent.run(`create open@${SELF} Garage`), /^Linked\. Restarts in 10 s\./);
    await agent.restart();
    const [owned] = (await agent.saved()).aliases;
    assert.equal(owned.host, SELF);
    assert.equal(await upgrade(self, owned.id, owned.token), 101);
    assert.equal(await upgrade(hosted, owned.id, owned.token), 404, "another server does not know the room");
    await agent.say("invite"); await agent.send("pump");
    const code = codeIn(await agent.say("result"));
    await agent.device("F");
    // The code belongs to the self-hosted server; the hosted server refuses it.
    assert.match(await agent.run(`join ${code}@${HOSTED}`), /^Link failed: code not valid/);
    assert.match(await agent.run(`join ${code}@${SELF}`), /^Linked\./);
    await agent.restart();
    assert.equal((await agent.saved()).aliases[0].id, owned.id);
  });

  test("slots: a node with two rooms refuses a third", async () => {
    await agent.device("F");
    const [code] = await hosted.mint();
    assert.match(await agent.run(`create ${code}@${HOSTED} Second`), /^Linked\./);
    await agent.restart();
    assert.equal((await agent.saved()).count, "2");
    const [more] = await hosted.mint();
    assert.match(await agent.say(`create ${more}@${HOSTED} Third`), /^All 2 room slots are in use/);
  });

  test("expired, used and over-tried codes", {timeout: 180000}, async () => {
    // An owner invite with the shortest lifetime expires at the server, whatever the node does.
    const owner = await (async () => { await agent.device("A"); return (await agent.saved()).aliases[0]; })();
    const minted = await hosted.control(owner.id, owner.token, {op: "invite", role: "frontend", ttl: 60});
    assert.equal(minted.status, 200);
    await sleep(62000);
    await agent.device("H");
    assert.match(await agent.run(`join ${minted.body.code}@${HOSTED}`), /^Link failed: code expired/);
    // A code that was already used answers "used", and wrong tries on it end with it being deleted.
    const fresh = await hosted.control(owner.id, owner.token, {op: "invite", role: "frontend", ttl: 600});
    const first = nodeFromSeed(randomBytes(32).toString("hex"));
    const body = redeemBody(first, "https://127.0.0.1", "join", {code: fresh.body.code});
    assert.equal((await post(hosted.base, "/v1/node/redeem", body)).status, 200);
    const outcomes = [];
    for (let i = 0; i < 6; i += 1) {
      const other = nodeFromSeed(randomBytes(32).toString("hex"));
      outcomes.push((await post(hosted.base, "/v1/node/redeem", redeemBody(other, "https://127.0.0.1", "join", {code: fresh.body.code}))).body.e);
    }
    assert.deepEqual(outcomes.slice(0, 4), ["code_used", "code_used", "code_used", "code_used"]);
    assert.ok(outcomes.slice(4).every(e => e === "code_invalid" || e === "code_used"), outcomes.join());
  });

  test("abuse: guessing codes, the node's failure limit and the server's quota", async () => {
    // Guessing codes: the node refuses further attempts after five failures until it restarts.
    await agent.device("G");
    const replies = [];
    for (let i = 0; i < 5; i += 1) replies.push(await agent.run(`join ${randomCode()}@${SELF}`));
    assert.ok(replies.every(text => /^Link failed: (code not valid|rate limited)/.test(text)), replies.join("\n"));
    assert.equal(await agent.say(`join ABCDEFGHJKMNPQRS@${SELF}`), "Too many failed attempts. Send \"reboot\", then retry.");
    assert.equal((await agent.saved()).text, "none");
    await agent.restart();
    assert.match(await agent.say(`join ABCDEFGHJKMNPQRS@${SELF}`), /^Contacting /, "a restart clears the node's limit");
    await agent.send("pump");
    // The server stops a client that keeps guessing, whatever the node does.
    let limited = false;
    const bot = nodeFromSeed(randomBytes(32).toString("hex"));
    for (let i = 0; i < 12 && !limited; i += 1) {
      const r = await post(self.base, "/v1/node/redeem", redeemBody(bot, "https://127.0.0.1", "join", {code: randomCode()}));
      limited = r.status === 429;
      assert.ok([400, 429].includes(r.status));
    }
    assert.ok(limited, "the server limits repeated wrong codes from one client");
  });

  test("abuse: a node may create two rooms a day and no more", async () => {
    await sleep(62000);   // let the per-client windows of the earlier cases lapse
    const bot = nodeFromSeed(randomBytes(32).toString("hex"));
    const results = [];
    for (const code of await hosted.mint(3)) {
      results.push(await post(hosted.base, "/v1/node/redeem", redeemBody(bot, "https://127.0.0.1", "create", {code, name: "Quota"})));
    }
    assert.deepEqual(results.map(r => r.status), [200, 200, 403], JSON.stringify(results.map(r => r.body.e)));
    assert.equal(results[2].body.e, "cap");
  });

  test("a revoked node loses the room and learns it", async () => {
    await agent.device("A");
    const owner = (await agent.saved()).aliases[0];
    assert.equal(await upgrade(hosted, room.alias, room.frontendToken), 101);
    const nodes = await hosted.control(owner.id, owner.token, {op: "nodes"});
    assert.equal(nodes.status, 200);
    const revoked = await hosted.control(owner.id, owner.token, {op: "revoke", id: room.frontendPub.slice(0, 8)});
    assert.equal(revoked.status, 200);
    assert.equal(await upgrade(hosted, room.alias, room.frontendToken), 401);
  });

  test("operator takedown stops the room", async () => {
    const [target] = (await hosted.admin("rooms", {offset: 0})).body.rooms.filter(r => r.alias === room.alias);
    assert.ok(target);
    assert.equal((await hosted.admin("room", {alias: room.alias, action: "disable"})).status, 200);
    assert.equal(await upgrade(hosted, room.alias, room.token), 403);
    assert.equal((await hosted.admin("room", {alias: room.alias, action: "enable"})).status, 200);
    assert.equal(await upgrade(hosted, room.alias, room.token), 101);
  });
  test("the node's console never shows a code, token or password", async () => {
    const printed = await agent.send("serial");
    assert.ok(secrets.size > 5);
    for (const secret of secrets) assert.ok(!printed.includes(secret), "a secret reached the console");
    assert.ok(!/ar1_[0-9a-f]{64}/.test(printed));
  });
});

describe("legacy compile-time provider", {skip: skip || (!AGENT_PROVIDER || !existsSync(AGENT_PROVIDER) ? "set E2E_AGENT_PROVIDER" : false)}, () => {
  let dir, hosted, agent;
  before(async () => {
    dir = mkdtempSync(join(tmpdir(), "e2e-legacy-"));
    hosted = new Server("legacy", 8843, {ROOM_KEK: kek("ef"), ADMIN_TOKEN_HASH: adminHash, SIGNUP_MODE: "code", ...FIXTURE});
    await hosted.start(dir);
    agent = new Agent(AGENT_PROVIDER, {E2E_PROVIDER_ALIAS: "A", E2E_PROVIDER_NAME: "A", E2E_PROVIDER_TOKEN: LEGACY_TOKEN,
      E2E_PROVIDER_HOST: HOSTED, E2E_PROVIDER_PUB: LEGACY_PUB});
    await agent.send(`host ${HOSTED} ${hosted.port}`);
  });
  after(() => { agent?.stop(); hosted?.stop(); if (dir) rmSync(dir, {recursive: true, force: true}); });

  test("the provider is adopted as the saved record and keeps working", async () => {
    await agent.device("P");
    const before = await agent.saved();
    assert.equal(before.magic, "v1", "a v1 record, readable by the previous release");
    assert.equal(before.aliases.length, 1);
    const [alias] = before.aliases;
    assert.deepEqual([alias.id, alias.host, alias.role, alias.ca, alias.flags], ["A", HOSTED, "0", "1", "0"]);
    assert.equal(alias.token, LEGACY_TOKEN);
    assert.equal(await upgrade(hosted, "A", alias.token), 101, "the static room accepts the adopted alias");
    await agent.restart();
    assert.equal((await agent.saved()).text, before.text, "a second boot loads the same record");
  });

  test("a full node refuses a new room, and unlinking keeps the provider from returning", async () => {
    const [code] = await hosted.mint();
    assert.match(await agent.say(`create ${code}@${HOSTED} Extra`), /^All 1 room slot is in use \(A\)/);
    // The provider's room is not a hosted room, so there is nothing to tell the server.
    assert.equal(await agent.say("unlink A delete"), "This room is not hosted on a shared-room server. Run \"cloudroom unlink ALIAS\" without delete.");
    assert.equal(await agent.say("unlink A"), "Alias removed locally.");
    await agent.restart();
    const after = await agent.saved();
    assert.equal(after.count, "0");
    assert.equal(after.magic, "v2", "an unlinked node needs a v2 record");
    await agent.restart();
    assert.equal((await agent.saved()).count, "0", "the provider is not adopted again");
    // The freed slot takes a hosted room.
    assert.match(await agent.run(`create ${code}@${HOSTED} Extra`), /^Linked\./);
  });
});
