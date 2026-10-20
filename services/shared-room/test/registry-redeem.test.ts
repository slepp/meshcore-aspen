import {expect, it} from "vitest";
import {runDurableObjectAlarm} from "cloudflare:test";
import {admin, call, control, convertFixture, createRoom, fixture, hostedSql, invite, joinRoom, json, mintCreate,
  nodeA, nodeB, nodeC, nodeFrom, now, redeem, registryDump, registrySql, registryStub, signedBody} from "./registry-helpers";
import {createSubject} from "../src/node-protocol";

it("serves a static info document and tracks the signup mode", async () => {
  const r = await call("/v1/info");
  expect(r.status).toBe(200);
  const body = await r.json<any>();
  expect(body).toMatchObject({service: "aspen-shared-room", version: "0.1.21", protocol: "aspen-node.v1", signup: "code"});
  expect(Math.abs(body.time - now())).toBeLessThan(5);
  expect((await (await call("/v1/info", {env: {ROOM_KEK: undefined}})).json<any>()).signup).toBe("closed");
  expect((await (await call("/v1/info", {env: {SIGNUP_MODE: "open"}})).json<any>()).signup).toBe("open");
  expect((await (await call("/v1/info", {env: {SIGNUP_MODE: "closed"}})).json<any>()).signup).toBe("closed");
});

it("creates a room, invites a frontend and an owner, lists, revokes and leaves", async () => {
  const room = await createRoom(nodeA, "Fleet");
  expect(room).toMatchObject({ok: 1, name: "Fleet", role: "owner"});
  expect(room.alias).toMatch(/^r[a-z2-7]{29}$/);
  expect(room.token).toMatch(/^ar1_[a-f0-9]{64}$/);
  expect(room.pub).toMatch(/^[a-f0-9]{64}$/);
  expect(["00", "ff"]).not.toContain(room.pub.slice(0, 2));

  const code = await invite(room);
  expect(code).toMatch(/^[0-9A-Z]{4}(-[0-9A-Z]{4}){3}$/);
  const joined = await redeem(nodeB, "join", {code});
  expect(joined.status, JSON.stringify(joined.body)).toBe(200);
  expect(joined.body).toMatchObject({alias: room.alias, name: "Fleet", pub: room.pub, role: "frontend"});
  expect(joined.body.token).not.toBe(room.token);

  const nodes = await control(room.alias, room.token, {op: "nodes"});
  expect(nodes.body).toMatchObject({ok: 1, total: 2, next: null});
  expect(nodes.body.nodes.map((n: any) => [n.id, n.r])).toEqual([[nodeA.pub.slice(0, 8), "o"], [nodeB.pub.slice(0, 8), "f"]]);

  // A frontend cannot use owner calls; an owner cannot revoke itself.
  expect((await control(room.alias, joined.body.token, {op: "invite"})).body.e).toBe("forbidden");
  expect((await control(room.alias, room.token, {op: "revoke", id: nodeA.pub.slice(0, 8)})).body.e).toBe("self");
  const revoked = await control(room.alias, room.token, {op: "revoke", id: nodeB.pub.slice(0, 12)});
  expect(revoked.body).toEqual({ok: 1});
  expect((await control(room.alias, joined.body.token, {op: "nodes"})).body.e).toBe("auth");
  expect((await registrySql("SELECT node FROM members WHERE alias=?", room.alias)).map(r => r.node)).toEqual([nodeA.pub]);

  // The last owner leaving an otherwise empty room deletes it and returns the password.
  const left = await control(room.alias, room.token, {op: "leave"});
  expect(left.body).toMatchObject({ok: 1, deleted: 1});
  expect(left.body.pw).toMatch(/^[0-9A-Z]{10}$/);
  expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(410);
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
});

it("converts a local room key and keeps the password sealed", async () => {
  const room = await convertFixture(nodeA, "room");
  expect(room).toMatchObject({pub: fixture.room.publicKey, role: "owner", name: "Converted"});
  expect((await control(room.alias, room.token, {op: "password", mode: "get"})).body).toEqual({ok: 1, pw: "room"});
  expect((await control(room.alias, room.token, {op: "password", mode: "open"})).body).toEqual({ok: 1, pw: ""});
  expect((await control(room.alias, room.token, {op: "password", mode: "set", pw: "abc 123"})).body.pw).toBe("abc 123");
  expect((await control(room.alias, room.token, {op: "password", mode: "set", pw: "x".repeat(16)})).body.e).toBe("bad_request");
  const fresh = await control(room.alias, room.token, {op: "password", mode: "new"});
  expect(fresh.body.pw).toMatch(/^[0-9A-Z]{10}$/);
  expect((await control(room.alias, room.token, {op: "password", mode: "get"})).body.pw).toBe(fresh.body.pw);
  // Nothing readable is left in storage: no key, password or token.
  const dump = JSON.stringify(await hostedSql(room.alias, "SELECT hex(sealed) AS s, public_key, kid FROM hosted_room")) +
    JSON.stringify(await hostedSql(room.alias, "SELECT * FROM hosted_frontend")) + await registryDump();
  for (const secret of [fixture.room.key, room.token, fresh.body.pw, "abc 123"]) expect(dump).not.toContain(secret);
  expect(dump).not.toContain(Buffer.from("room").toString("hex"));
});

it("rejects malformed, mis-signed, stale and unknown redeems with fixed error codes", async () => {
  const [code] = await mintCreate(3);
  const ok = (b: any) => ({status: b.status, e: b.body.e});
  expect(ok(await redeem(nodeA, "create", {code, name: "N", ts: now() + 1000}))).toEqual({status: 409, e: "clock"});
  expect((await redeem(nodeA, "create", {code, name: "N", ts: now() + 1000})).body.now).toBeTypeOf("number");
  expect(ok(await redeem(nodeA, "create", {code, name: "N", origin: "https://other.example"}))).toEqual({status: 401, e: "bad_sig"});
  expect(ok(await redeem(nodeA, "create", {code, name: "N", mutate: b => {b.name = "Other";}}))).toEqual({status: 401, e: "bad_sig"});
  expect(ok(await redeem(nodeA, "create", {code, name: "N", mutate: b => {b.sig = "00".repeat(64);}}))).toEqual({status: 401, e: "bad_sig"});
  expect(ok(await redeem(nodeA, "create", {code, name: "bad:name"}))).toEqual({status: 400, e: "name"});
  expect(ok(await redeem(nodeA, "create", {code, name: "x".repeat(32)}))).toEqual({status: 400, e: "name"});
  expect(ok(await redeem(nodeA, "create", {code: "ABCD-EFGH-JKMN-PQRS", name: "N"}))).toEqual({status: 400, e: "code_invalid"});
  expect(ok(await redeem(nodeA, "join", {code}))).toEqual({status: 400, e: "code_invalid"});
  for (const mutate of [(b: any) => {b.extra = 1;}, (b: any) => {b.p = "aspen-node.v2";}, (b: any) => {b.op = "leave";},
      (b: any) => {delete b.sig;}, (b: any) => {b.ts = "now";}, (b: any) => {b.avoid = "zz";}, (b: any) => {b.code = "TOO-SHORT";}]) {
    const r = await redeem(nodeA, "create", {code, name: "N", mutate});
    expect(r.body.e, JSON.stringify(r.body)).toBe("bad_request");
  }
  expect((await json("/v1/node/redeem", "{not json")).body.e).toBe("bad_request");
  expect((await json("/v1/node/redeem", "[]")).body.e).toBe("bad_request");
  expect((await json("/v1/node/redeem", "x".repeat(3000))).body.e).toBe("bad_request");
  expect((await call("/v1/node/redeem", {method: "GET"})).status).toBe(400);
  expect((await call("/v1/node/redeem", {method: "POST", headers: {"Content-Type": "text/plain"}, body: "{}"})).status).toBe(400);
  expect((await call("/v1/node/redeem", {method: "POST", headers: {"Content-Type": "application/json"}, body: "{}", ip: ""})).status).toBe(400);
  // Every error carries a fixed message and none echoes input.
  const bad = await redeem(nodeA, "create", {code, name: "N", origin: "https://other.example"});
  expect(bad.body.m.length).toBeLessThanOrEqual(100);
  expect(JSON.stringify(bad.body)).not.toContain(code.replace(/-/g, ""));
});

it("answers closed without a KEK or in closed signup mode", async () => {
  const [code] = await mintCreate();
  expect((await redeem(nodeA, "create", {code, name: "N", env: {ROOM_KEK: undefined}})).body.e).toBe("closed");
  expect((await redeem(nodeA, "create", {code, name: "N", env: {SIGNUP_MODE: "closed"}})).body.e).toBe("closed");
  expect((await json("/v1/aliases/r2q7x3bndkamw4qzfxe7abcdmn2xyz/control", {op: "nodes"}, {token: "ar1_" + "a".repeat(64), env: {ROOM_KEK: undefined}})).status).toBe(503);
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
});

it("enforces code lifetime, single use and the replay rule", async () => {
  const [code] = await mintCreate();
  const first = await redeem(nodeA, "create", {code, name: "Fleet", ts: now()});
  expect(first.status).toBe(200);
  // Identical body: code_used. Another node, op or subject: code_used.
  const body = await signedBody(nodeA, "create", {code, name: "Fleet", ts: now()});
  expect((await json("/v1/node/redeem", body)).body.e).toBe("code_used");
  expect((await json("/v1/node/redeem", body)).body.e).toBe("code_used");
  expect((await redeem(nodeB, "create", {code, name: "Fleet", ts: now() + 5})).body.e).toBe("code_used");
  expect((await redeem(nodeA, "create", {code, name: "Other", ts: now() + 5})).body.e).toBe("code_used");
  // Same node, op and subject with a later ts and a new signature: same room, fresh token, old token stops.
  const again = await redeem(nodeA, "create", {code, name: "Fleet", ts: now() + 5});
  expect(again.status, JSON.stringify(again.body)).toBe(200);
  expect(again.body.alias).toBe(first.body.alias);
  expect(again.body.pub).toBe(first.body.pub);
  expect(again.body.token).not.toBe(first.body.token);
  expect((await control(first.body.alias, first.body.token, {op: "nodes"})).body.e).toBe("auth");
  expect((await control(first.body.alias, again.body.token, {op: "nodes"})).status).toBe(200);
  // An earlier ts is not a replay.
  expect((await redeem(nodeA, "create", {code, name: "Fleet", ts: now() - 100})).body.e).toBe("code_used");
});

it("expires codes and counts wrong-state tries", async () => {
  const [code] = await mintCreate();
  await registrySql("UPDATE codes SET expires=?", now() - 5);
  expect((await redeem(nodeA, "create", {code, name: "N"})).body.e).toBe("code_expired");
  // Five wrong-state tries on an existing code delete it.
  const [other] = await mintCreate();
  for (let n = 0; n < 5; n++) expect((await redeem(nodeA, "join", {code: other, ip: `203.0.113.${n + 1}`})).body.e).toBe("code_invalid");
  expect(await registrySql("SELECT * FROM codes WHERE state='new'")).toHaveLength(1);
  const gone = await redeem(nodeA, "create", {code: other, name: "N"});
  expect(gone.status).toBe(400);
  expect(gone.body.e).toBe("code_invalid");
});

it("allows one winner when a code is redeemed twice at once", async () => {
  const [code] = await mintCreate();
  const results = await Promise.all([redeem(nodeA, "create", {code, name: "One"}), redeem(nodeB, "create", {code, name: "Two"})]);
  expect(results.filter(r => r.status === 200)).toHaveLength(1);
  expect(results.find(r => r.status !== 200)!.body.e).toBe("code_used");
  expect(await registrySql("SELECT * FROM rooms")).toHaveLength(1);
});

it("rejects a second conversion of one key and re-converts after delete", async () => {
  const [one, two] = await mintCreate(2);
  const results = await Promise.all([
    redeem(nodeA, "convert", {code: one, name: "R", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"}),
    redeem(nodeB, "convert", {code: two, name: "R", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"})]);
  expect(results.filter(r => r.status === 200)).toHaveLength(1);
  expect(results.find(r => r.status !== 200)!.body.e).toBe("exists");
  const winner = results.find(r => r.status === 200)!.body;
  const delRoom = await control(winner.alias, winner.token, {op: "delete", confirm: winner.alias});
  expect(delRoom.body).toEqual({ok: 1});
  const [three] = await mintCreate();
  const again = await redeem(nodeA, "convert", {code: three, name: "R", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"});
  expect(again.status, JSON.stringify(again.body)).toBe(200);
  expect(again.body.alias).not.toBe(winner.alias);
});

it("recovers a lost conversion reply for the owner node and refuses other nodes", async () => {
  const first = await convertFixture(nodeA);
  const [code] = await mintCreate();
  const lost = await redeem(nodeA, "convert", {code, name: "Converted", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"});
  expect(lost.status).toBe(200);
  expect(lost.body.alias).toBe(first.alias);
  expect(lost.body.role).toBe("owner");
  expect(lost.body.token).not.toBe(first.token);
  const [other] = await mintCreate();
  expect((await redeem(nodeB, "convert", {code: other, name: "Converted", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"})).body.e).toBe("exists");
});

it("validates converted keys against the public key, clamp, first byte and static rooms", async () => {
  const [code] = await mintCreate();
  const convert = (o: any) => redeem(nodeA, "convert", {code, name: "N", ...o});
  const k = fixture.room.key, p = fixture.room.publicKey;
  expect((await convert({pub: fixture.thirdRoom.publicKey, key: k})).body.e).toBe("key");
  const badClamp = (parseInt(k.slice(0, 2), 16) | 7).toString(16).padStart(2, "0") + k.slice(2);
  expect((await convert({pub: p, key: badClamp})).body.e).toBe("key");
  expect((await convert({pub: "00" + p.slice(2), key: k})).body.e).toBe("key");
  expect((await convert({pub: "ff" + p.slice(2), key: k})).body.e).toBe("key");
  expect((await convert({pub: p, key: k, pw: "x".repeat(16)})).body.e).toBe("bad_request");
  // A static room's public key is never re-hosted.
  expect((await convert({pub: fixture.otherRoom.publicKey, key: fixture.otherRoom.key})).body.e).toBe("exists");
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
  expect((await registrySql("SELECT state FROM codes"))[0].state).toBe("new");
});

it("regenerates created keys to avoid clashes and flags join collisions", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const first = room.pub.slice(0, 2);
  const [code] = await mintCreate();
  const created = await redeem(nodeB, "create", {code, name: "Two", avoid: first + "01"});
  expect(created.status).toBe(200);
  expect(created.body.pub.slice(0, 2)).not.toBe(first);
  expect(created.body.pub.slice(0, 2)).not.toBe("01");
  const invited = await invite(room);
  const clash = await redeem(nodeC, "join", {code: invited, avoid: "aa" + first});
  expect(clash.body.e).toBe("collision");
  // A collision does not consume the code.
  expect((await redeem(nodeC, "join", {code: invited, avoid: "aa"})).status).toBe(200);
});

it("enforces room, node and frontend quotas", async () => {
  const room = await createRoom(nodeA, "R1");
  expect((await createRoom(nodeA, "R2")).ok).toBe(1);
  const [third] = await mintCreate();
  const capped = await redeem(nodeA, "create", {code: third, name: "R3"});
  expect(capped.body.e).toBe("cap");
  expect(capped.status).toBe(403);
  // MAX_ROOMS applies to all nodes.
  const [fourth] = await mintCreate();
  expect((await redeem(nodeB, "create", {code: fourth, name: "R4", env: {MAX_ROOMS: "2"}})).body.e).toBe("cap");
  // MAX_FRONTENDS counts members.
  const a = await invite(room), b = await invite(room);
  expect((await redeem(nodeB, "join", {code: a, env: {MAX_FRONTENDS: "2"}})).status).toBe(200);
  expect((await redeem(nodeC, "join", {code: b, env: {MAX_FRONTENDS: "2"}})).body.e).toBe("cap");
});

it("refuses banned nodes and revokes their tokens everywhere", async () => {
  const room = await createRoom(nodeA, "R1");
  const joined = await joinRoom(room, nodeB);
  const other = await createRoom(nodeC, "R2");
  const codeToo = await invite(other);
  expect((await redeem(nodeB, "join", {code: codeToo})).status).toBe(200);
  expect((await admin("node", {node: nodeB.pub, action: "ban"})).body).toEqual({ok: 1});
  expect((await control(room.alias, joined.token, {op: "nodes"})).body.e).toBe("auth");
  const [c3] = await mintCreate();
  expect((await redeem(nodeB, "create", {code: c3, name: "N"})).body.e).toBe("banned");
  expect(await registrySql("SELECT * FROM members WHERE node=?", nodeB.pub)).toEqual([]);
  expect((await admin("node", {node: nodeB.pub, action: "unban"})).body).toEqual({ok: 1});
  expect((await redeem(nodeB, "create", {code: c3, name: "N"})).status).toBe(200);
});

it("re-claims a stale claim for the same node and releases stale claims in the alarm", async () => {
  const stale = "rabcdefghijklmnopqrstuvwxyz234", dead = "rzyxwvutsrqponmlkjihgfedcba765";
  const [code] = await mintCreate();
  const hash = (await registrySql("SELECT hash FROM codes"))[0].hash;
  const claimedAt = now() - 120;
  // A claim whose Room call never completed leaves a claimed code and a pending room.
  await registrySql("UPDATE codes SET state='claimed', claimed_by=?, claimed_at=?, op='create', subject=?, first_ts=?, alias=? WHERE hash=?",
    nodeA.pub, claimedAt, await createSubject("Fleet"), now() - 50, stale, hash);
  await registrySql("INSERT INTO rooms(alias, state, owner_node, created, last_active, pub_hash, name, pub_first, state_since) VALUES (?,'pending',?,?,?,'hh','Fleet',1,?)",
    stale, nodeA.pub, claimedAt, claimedAt, claimedAt);
  // Another node still sees code_used; the same node with a later ts succeeds and keeps the alias.
  expect((await redeem(nodeB, "create", {code, name: "Fleet"})).body.e).toBe("code_used");
  const retry = await redeem(nodeA, "create", {code, name: "Fleet", ts: now()});
  expect(retry.status, JSON.stringify(retry.body)).toBe(200);
  expect(retry.body.alias).toBe(stale);
  // A claim whose room never became active is released by the alarm, freeing the quota.
  await mintCreate();
  const fresh = (await registrySql("SELECT hash FROM codes WHERE state='new'"))[0].hash;
  await registrySql("UPDATE codes SET state='claimed', claimed_by=?, claimed_at=?, op='create', subject='x', first_ts=1, alias=? WHERE hash=?",
    nodeB.pub, now() - 600, dead, fresh);
  await registrySql("INSERT INTO rooms(alias, state, owner_node, created, last_active, pub_hash, name, pub_first, state_since) VALUES (?,'pending',?,?,?,'h2','X',1,?)",
    dead, nodeB.pub, now() - 600, now() - 600, now() - 600);
  await runDurableObjectAlarm(registryStub());
  expect(await registrySql("SELECT * FROM rooms WHERE alias=?", dead)).toEqual([]);
  expect((await registrySql("SELECT state FROM codes WHERE hash=?", fresh))[0].state).toBe("new");
});

it("releases the claim, pending room and quota when the Room object call fails", async () => {
  const down = {getByName: () => ({provision: async () => {throw new Error("down")}, addFrontend: async () => {throw new Error("down")}})};
  const [code] = await mintCreate();
  const failed = await redeem(nodeA, "create", {code, name: "Fleet", env: {ROOMS: down as any}});
  expect(failed.status).toBe(500);
  expect(failed.body.e).toBe("internal");
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
  expect((await registrySql("SELECT state, claimed_by FROM codes"))[0]).toEqual({state: "new", claimed_by: null});
  expect((await registrySql("SELECT count FROM limits WHERE key LIKE 'cc:%'"))[0].count).toBe(0);
  // The same code works once the Room object is back, and a failed join keeps its invite.
  const created = await redeem(nodeA, "create", {code, name: "Fleet"});
  expect(created.status).toBe(200);
  const inviteCode = await invite(created.body);
  expect((await redeem(nodeB, "join", {code: inviteCode, env: {ROOMS: down as any}})).status).toBe(500);
  expect((await registrySql("SELECT state FROM codes WHERE kind='join'"))[0].state).toBe("new");
  expect((await redeem(nodeB, "join", {code: inviteCode})).status).toBe(200);
});

it("does not let a revoked, departed or banned node back in by replaying its redeem", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const replay = async (node: any, code: string, ts: number) => (await redeem(node, "join", {code, ts})).body.e;
  // Revoked by an owner.
  const codeB = await invite(room);
  const first = await redeem(nodeB, "join", {code: codeB, ts: now()});
  expect(first.status).toBe(200);
  await control(room.alias, room.token, {op: "revoke", id: nodeB.pub.slice(0, 8)});
  expect(await replay(nodeB, codeB, now() + 5)).toBe("code_used");
  // Left on its own.
  const codeC = await invite(room);
  const second = await redeem(nodeC, "join", {code: codeC, ts: now()});
  expect((await control(room.alias, second.body.token, {op: "leave"})).body).toMatchObject({deleted: 0});
  expect(await replay(nodeC, codeC, now() + 5)).toBe("code_used");
  // Banned, then unbanned: the old code is still spent.
  const codeD = await invite(room);
  const nodeD = nodeFrom(0x44);
  expect((await redeem(nodeD, "join", {code: codeD, ts: now()})).status).toBe(200);
  await admin("node", {node: nodeD.pub, action: "ban"});
  await admin("node", {node: nodeD.pub, action: "unban"});
  expect(await replay(nodeD, codeD, now() + 5)).toBe("code_used");
  expect((await control(room.alias, room.token, {op: "nodes"})).body.total).toBe(1);
  // A fresh invite is the deliberate way back, and it is a member again.
  const again = await redeem(nodeB, "join", {code: await invite(room)});
  expect(again.status).toBe(200);
  expect((await registrySql("SELECT node FROM members WHERE node=?", nodeB.pub))).toHaveLength(1);
});

it("does not return owner rights to a converter that another owner revoked", async () => {
  const room = await convertFixture(nodeA);
  const owner = await joinRoom(room, nodeB, "owner");
  await hostedSql(room.alias, "UPDATE hosted_frontend SET owner_since=? WHERE node=?", now() - 90000, nodeB.pub);
  expect((await control(room.alias, owner.token, {op: "revoke", id: nodeA.pub.slice(0, 8)})).status).toBe(200);
  const [code] = await mintCreate();
  const retry = await redeem(nodeA, "convert", {code, name: "Converted", pub: fixture.room.publicKey, key: fixture.room.key, pw: "room"});
  expect(retry.body.e).toBe("exists");
  expect((await control(room.alias, room.token, {op: "nodes"})).body.e).toBe("auth");
});
