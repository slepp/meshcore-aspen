import {expect, it} from "vitest";
import {runInDurableObject} from "cloudflare:test";
import {admin, bindings, call, control, createRoom, fixture, freshIp, hostedSql, invite, json, mintCreate, nodeA, nodeB, nodeC, nodeFrom, now,
  redeem, registryDump, registrySql, registryStub, signedBody, socket} from "./registry-helpers";
import {newAliasId, parseKek} from "../src/seal";
import type {ClaimParams} from "../src/registry";

const claim = (p: Partial<ClaimParams>) => runInDurableObject(registryStub(), (inst: any) => inst.claim({
  op: "join", hash: "ab".repeat(32), open: false, node: nodeA.pub, ts: now(), subject: "-", client: "c1", signup: "code", maxRooms: 2000,
  maxFrontends: 16, avoid: [], alias: "", ...p}));
const expireWindows = (seconds: number) => registrySql("UPDATE limits SET window_start=window_start-?", seconds);

it("writes nothing for unsigned, malformed or stale redeems", async () => {
  const [code] = await mintCreate();
  const before = await registryDump();
  const good = await signedBody(nodeA, "create", {code, name: "N"});
  for (let n = 0; n < 1000; n++) {
    const ip = `192.0.2.${n % 20}`;
    const body: Record<string, unknown> = n % 4 === 0 ? {...good, sig: "ab".repeat(64)} :
      n % 4 === 1 ? {...good, ts: 5} : n % 4 === 2 ? {junk: n} : {...good, key: "k".repeat(128), op: "convert"};
    await json("/v1/node/redeem", body, {ip});
  }
  expect(await registryDump()).toBe(before);
});

it("bounds Registry writes from one client sending signed requests for unknown codes", async () => {
  const ip = "192.0.2.77";
  let limited = 0;
  for (let n = 0; n < 1000; n++) {
    const r = await redeem(nodeA, "join", {code: "ABCD-EFGH-JKMN-PQRS", ts: now() - (n % 5), ip});
    if (r.status === 429) limited++;
  }
  expect(limited).toBeGreaterThan(900);
  const rows = await registrySql<{count: number}>("SELECT count FROM limits");
  expect(rows.reduce((sum, r) => sum + r.count, 0)).toBeLessThanOrEqual(40);
  const retry = await redeem(nodeA, "join", {code: "ABCD-EFGH-JKMN-PQRS", ip});
  expect(retry.status).toBe(429);
  expect(Number(retry.headers.get("Retry-After"))).toBeGreaterThan(0);
});

it("counts invalid codes and signed requests per client with expiring windows", async () => {
  for (let n = 0; n < 5; n++) expect((await claim({hash: "cd".repeat(32), client: "inv"})).e).toBe("code_invalid");
  const blocked = await claim({hash: "cd".repeat(32), client: "inv"});
  expect(blocked).toMatchObject({k: "err", e: "rate"});
  expect(blocked.retryAfter).toBeGreaterThan(0);
  await expireWindows(61);
  expect((await claim({hash: "cd".repeat(32), client: "inv"})).e).toBe("code_invalid");
  // The hourly invalid-code counter and the per-minute signed counter trip at their own thresholds.
  await registrySql("DELETE FROM limits");
  await registrySql("INSERT OR REPLACE INTO limits VALUES ('ci1h:inv', ?, 30)", now());
  expect((await claim({hash: "cd".repeat(32), client: "inv"})).e).toBe("rate");
  await registrySql("DELETE FROM limits");
  for (let n = 0; n < 10; n++) await claim({hash: "ee".repeat(32), client: "sig", op: "create"}).catch(() => undefined);
  await registrySql("UPDATE limits SET count=10 WHERE key='rq60:sig'");
  expect((await claim({hash: "ee".repeat(32), client: "sig"})).e).toBe("rate");
  await registrySql("DELETE FROM limits WHERE key='rq60:sig'");
  await registrySql("INSERT OR REPLACE INTO limits VALUES ('rq1h:sig', ?, 40)", now());
  expect((await claim({hash: "ee".repeat(32), client: "sig"})).e).toBe("rate");
  await expireWindows(3601);
  expect((await claim({hash: "ee".repeat(32), client: "sig"})).e).not.toBe("rate");
});

it("limits creations per node, live rooms per node and open signups", async () => {
  await createRoom(nodeA, "R1");
  await createRoom(nodeA, "R2");
  const [code] = await mintCreate();
  expect((await redeem(nodeA, "create", {code, name: "R3"})).body.e).toBe("cap");
  await expireWindows(86401);
  // Four live rooms is the node maximum even when the daily counter is clear.
  await registrySql("INSERT INTO rooms(alias, state, owner_node, created, last_active, pub_hash) VALUES ('x1','active',?,1,1,'p1'),('x2','active',?,1,1,'p2')", nodeA.pub, nodeA.pub);
  expect((await redeem(nodeA, "create", {code, name: "R3"})).body.e).toBe("cap");
});

it("limits open signup per client and for the whole service", async () => {
  const env = {SIGNUP_MODE: "open"};
  const ip = "192.0.2.201";
  const open = (seed: number, ip: string, name = "Open") => redeem(nodeFrom(seed), "create", {code: "OPEN", name, ip, env});
  const first = await open(1, ip);
  expect(first.status, JSON.stringify(first.body)).toBe(200);
  // A lost reply is recoverable: same node, name and later ts returns the same room.
  const again = await redeem(nodeFrom(1), "create", {code: "OPEN", name: "Open", ip, env, ts: now() + 2});
  expect(again.body.alias).toBe(first.body.alias);
  expect((await open(2, ip)).status).toBe(200);
  expect((await open(3, ip)).status).toBe(200);
  expect((await open(4, ip)).body.e).toBe("rate");
  // Code mode refuses OPEN, and closed mode refuses everything that creates.
  expect((await redeem(nodeFrom(5), "create", {code: "OPEN", name: "Open"})).body.e).toBe("code_invalid");
  expect((await redeem(nodeFrom(5), "create", {code: "OPEN", name: "Open", env: {SIGNUP_MODE: "closed"}})).body.e).toBe("closed");
  // 20 per hour for the service as a whole.
  await registrySql("DELETE FROM limits");
  await registrySql("INSERT OR REPLACE INTO limits VALUES ('os:service', ?, 20)", now());
  expect((await open(6, "192.0.2.202")).body.e).toBe("rate");
  await expireWindows(3601);
  expect((await open(6, "192.0.2.202")).status).toBe(200);
});

it("keeps closed signup open for owner invites", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const code = await invite(room);
  expect((await redeem(nodeB, "join", {code, env: {SIGNUP_MODE: "closed"}})).status).toBe(200);
});

it("limits live invite codes per issuer and per day", async () => {
  const room = await createRoom(nodeA, "Fleet");
  for (let n = 0; n < 8; n++)
    await registrySql("INSERT INTO codes(hash, kind, alias, role, issuer, expires) VALUES (?, 'join', ?, 0, ?, ?)", "f".repeat(63) + n, "other" + n, nodeA.pub.slice(0, 8), now() + 600);
  expect((await control(room.alias, room.token, {op: "invite"})).body.e).toBe("cap");
  // Expired codes are evicted before counting.
  await registrySql("UPDATE codes SET expires=?", now() - 1);
  expect((await control(room.alias, room.token, {op: "invite"})).status).toBe(200);
});

it("evicts the oldest limits row when the table is full and still serves a valid code", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const code = await invite(room);
  await registrySql("DELETE FROM limits");
  await registrySql("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i<4094) INSERT INTO limits SELECT 'fill:' || i, ? + i, 1 FROM n", now() - 5000);
  expect((await registrySql("SELECT COUNT(*) AS n FROM limits"))[0].n).toBe(4095);
  const r = await redeem(nodeB, "join", {code});
  expect(r.status, JSON.stringify(r.body)).toBe(200);
  // New counters displaced the oldest rows instead of being refused.
  expect((await registrySql("SELECT COUNT(*) AS n FROM limits"))[0].n).toBeLessThanOrEqual(4096);
  expect((await registrySql("SELECT COUNT(*) AS n FROM limits WHERE key LIKE 'rq60:%'"))[0].n).toBe(1);
  expect((await registrySql("SELECT COUNT(*) AS n FROM limits WHERE key='fill:0'"))[0].n).toBe(0);
});

it("trips the daily Registry write budget and the per-minute budget without touching hosted traffic", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const env = {REGISTRY_WRITES_PER_DAY: "3"};
  // One write was already counted by createRoom's redeem in this isolate; two more fit, then closed.
  const codes = await mintCreate(4);
  const results = [];
  for (let n = 0; n < 4; n++) results.push((await redeem(nodeFrom(0x60 + n), "create", {code: codes[n], name: "R" + n, env})).body.e ?? "ok");
  expect(results).toContain("closed");
  expect(results[3]).toBe("closed");
  // Owner calls and sockets keep working while redeem is closed.
  expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(200);
  const s = await socket(room.alias, room.token);
  expect(await s.next("ready")).toMatchObject({type: "ready"});
});

it("answers rate once 600 anonymous redeems arrive in a minute", async () => {
  let rate = 0;
  for (let n = 0; n < 610; n++) {
    const r = await json("/v1/node/redeem", {junk: 1}, {ip: `10.${n >> 8}.${n & 255}.1`});
    if (r.body.e === "rate") rate++;
  }
  expect(rate).toBe(10);
});

it("creates no Durable Object storage for random or never-provisioned hosted ids", async () => {
  const kek = parseKek(bindings)!;
  const token = "ar1_" + "a".repeat(64);
  for (let n = 0; n < 50; n++) {
    const random = "r" + Array.from({length: 29}, () => "abcdefghijklmnopqrstuvwxyz234567"[Math.floor(Math.random() * 32)]).join("");
    expect((await call(`/v1/aliases/${random}/socket`, {headers: {Authorization: `Bearer ${token}`, Upgrade: "websocket"}})).status).toBe(404);
    expect((await control(random, token, {op: "nodes"})).status).toBe(404);
    expect((await call(`/v1/aliases/${random}/operations`, {method: "POST", headers: {Authorization: `Bearer ${token}`}, body: "{}"})).status).toBe(404);
  }
  // A valid tag on a room that was never provisioned reaches the object, which answers 404 and writes nothing.
  for (let n = 0; n < 5; n++) {
    const alias = await newAliasId(kek);
    expect((await control(alias, token, {op: "nodes"})).body.e).toBe("not_found");
    expect((await call(`/v1/aliases/${alias}/socket`, {headers: {Authorization: `Bearer ${token}`, Upgrade: "websocket"}})).status).toBe(404);
    expect((await call(`/v1/aliases/${alias}/operations`, {method: "POST", headers: {Authorization: `Bearer ${token}`}, body: "{}"})).status).toBe(404);
    expect(await hostedSql(alias, "SELECT name FROM sqlite_master")).toEqual([]);
  }
  // Static aliases are never reinterpreted: the old authorisation path answers.
  expect((await call("/v1/aliases/S/socket", {headers: {Authorization: "Bearer wrong", Upgrade: "websocket"}})).status).toBe(401);
});

it("limits control calls, bad tokens and socket connects per token or client without limiting a valid token", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const ip = freshIp();
  for (let n = 0; n < 20; n++) expect((await control(room.alias, "ar1_" + "b".repeat(64), {op: "nodes"}, ip)).body.e).toBe("auth");
  expect((await control(room.alias, "ar1_" + "b".repeat(64), {op: "nodes"}, ip)).body.e).toBe("rate");
  // The same client with a valid token is never limited by the bad-token counter.
  expect((await control(room.alias, room.token, {op: "nodes"}, ip)).status).toBe(200);
  for (let n = 0; n < 29; n++) expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(200);
  const limited = await control(room.alias, room.token, {op: "nodes"});
  expect(limited.body.e).toBe("rate");
  expect(Number(limited.headers.get("Retry-After"))).toBeGreaterThan(0);
  const second = await createRoom(nodeB, "Other");
  for (let n = 0; n < 12; n++) (await socket(second.alias, second.token)).ws.close();
  const refused = await call(`/v1/aliases/${second.alias}/socket`, {headers: {Authorization: `Bearer ${second.token}`, Upgrade: "websocket"}});
  expect(refused.status).toBe(429);
  await refused.text();
});

it("limits the operator API per client and counts failures without auditing them", async () => {
  const ip = freshIp();
  for (let n = 0; n < 10; n++) expect((await json("/v1/admin/rooms", {}, {token: "wrong", ip})).body.e).toBe("auth");
  expect((await json("/v1/admin/rooms", {}, {token: "wrong", ip})).body.e).toBe("rate");
  expect(await registrySql("SELECT * FROM audit")).toEqual([]);
  expect(await registrySql("SELECT * FROM admin_audit")).toEqual([]);
});

it("keeps the audit rings bounded", async () => {
  await registrySql("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i<5100) INSERT INTO audit(ts, actor, action, target, result) SELECT ?, 'a', 'x', 't', 'ok' FROM n", now());
  await registrySql("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM n WHERE i<1100) INSERT INTO admin_audit(ts, action, target, result) SELECT ?, 'x', 't', 'ok' FROM n", now());
  await createRoom(nodeA, "Fleet");
  await mintCreate();
  expect((await registrySql("SELECT COUNT(*) AS n FROM audit"))[0].n).toBe(5000);
  expect((await registrySql("SELECT COUNT(*) AS n FROM admin_audit"))[0].n).toBe(1000);
  void fixture; void nodeC;
});
