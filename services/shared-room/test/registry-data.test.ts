import {expect, it} from "vitest";
import {runDurableObjectAlarm, runInDurableObject} from "cloudflare:test";
import {admin, bindings, call, control, convertFixture, createRoom, fixture, history, hostedSql, hostedStub, invite, joinRoom, json, nodeA, nodeB,
  nodeC, now, redeem, registrySql, rf, socket} from "./registry-helpers";

const ready = (alias: string) => ({type: "ready", version: 2, alias, publicKey: fixture.room.publicKey, name: "Converted"});

it("runs the opaque RF flow for a hosted frontend, whatever MODE says", async () => {
  const room = await convertFixture(nodeA, "room");
  const s = await socket(room.alias, room.token);
  expect(await s.next("ready")).toMatchObject(ready(room.alias));
  for (const mode of [undefined, "opaque", "decoded"]) {
    await runInDurableObject(hostedStub(room.alias), (inst: any) => {inst.env = {...inst.env, MODE: mode};});
    const login = await rf(room.alias, room.token, fixture.readerLogin);
    expect(login.status, JSON.stringify(login.body)).toBe(200);
    expect(login.body.accepted).toBe(true);
    // Only the first login produces a reply; repeats are duplicates under every MODE.
    expect(!!login.body.transmission).toBe(mode === undefined);
  }
  await rf(room.alias, room.token, fixture.path);
  await rf(room.alias, room.token, fixture.authorLogin);
  await rf(room.alias, room.token, fixture.post);
  const tx = await s.next("history");
  const received = history(tx);
  expect(received.text).toBe("hello");
  const acked = await rf(room.alias, room.token, received.ack);
  expect(acked.body.accepted).toBe(true);
  const state = await hostedSql(room.alias, "SELECT cursor FROM sessions WHERE client=?", fixture.reader.publicKey);
  expect(state[0].cursor).toBe(1);
  // The hosted room never requires the PATH ACK a multi-backend service needs.
  expect(await runInDurableObject(hostedStub(room.alias), (inst: any) => inst.needsPathAck())).toBe(false);
});

it("leaves static rooms, their listing and PATH ACK mode unchanged once hosted rooms exist", async () => {
  const before = await (await call("/v1/web/rooms")).text();
  const staticNeeds = () => runInDurableObject(bindings.ROOMS.getByName("static"), (inst: any) => inst.needsPathAck());
  const needsBefore = await staticNeeds();
  await createRoom(nodeA, "Hosted");
  expect(await (await call("/v1/web/rooms")).text()).toBe(before);
  expect(JSON.parse(before).rooms.map((r: any) => r.id)).toEqual(["S", "T"]);
  expect(await staticNeeds()).toBe(needsBefore);
  const sessions = await socket("S", "one");
  expect(await sessions.next("ready")).toMatchObject({type: "ready", version: 2, publicKey: fixture.otherRoom.publicKey});
  // Static aliases never touch the Registry: its tables stay empty of static ids.
  expect(await registrySql("SELECT alias FROM rooms WHERE alias IN ('S','T')")).toEqual([]);
  // The hosted control route and the Registry refuse static ids.
  expect((await control("S", "ar1_" + "a".repeat(64), {op: "nodes"})).body.e).toBe("not_found");
});

it("closes sockets immediately on revoke, leave, disable and delete, and refuses later connects", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const member = await joinRoom(room, nodeB);
  const second = await joinRoom(room, nodeC, "owner");
  const owner = await socket(room.alias, room.token), m = await socket(room.alias, member.token);
  await owner.next(); await m.next();
  // revoke
  await control(room.alias, room.token, {op: "revoke", id: nodeB.pub.slice(0, 8)});
  expect(await m.closed).toBe(1008);
  await socket(room.alias, member.token, 401);
  // disable: sockets close, tokens say disabled, enable restores the same token
  expect((await admin("room", {alias: room.alias, action: "disable"})).body).toEqual({ok: 1});
  expect(await owner.closed).toBe(1008);
  expect((await control(room.alias, room.token, {op: "nodes"})).body.e).toBe("disabled");
  await socket(room.alias, room.token, 403);
  expect((await admin("room", {alias: room.alias, action: "enable"})).body).toEqual({ok: 1});
  const again = await socket(room.alias, room.token);
  await again.next();
  // leave closes the leaver
  const sec = await socket(room.alias, second.token); await sec.next();
  expect((await control(room.alias, second.token, {op: "leave"})).body).toMatchObject({ok: 1, deleted: 0});
  expect(await sec.closed).toBe(1008);
  // delete (admin) closes everything and leaves a marker
  expect((await admin("room", {alias: room.alias, action: "delete"})).body).toEqual({ok: 1});
  expect(await again.closed).toBe(1008);
  await socket(room.alias, room.token, 403);
  expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(410);
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
  expect((await hostedSql(room.alias, "SELECT state, name, public_key FROM hosted_room"))[0]).toEqual({state: "deleted", name: "", public_key: ""});
  expect(await hostedSql(room.alias, "SELECT * FROM messages")).toEqual([]);
});

it("rechecks the token on every frame after the socket was accepted", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const member = await joinRoom(room, nodeB);
  const m = await socket(room.alias, member.token); await m.next();
  await hostedSql(room.alias, "UPDATE hosted_frontend SET revoked=1 WHERE node=?", nodeB.pub);
  m.ws.send(JSON.stringify({id: "1", operation: {op: "rf", packet: fixture.readerLogin}}));
  expect((await m.next("error")).error).toMatch(/authorization changed/);
});

it("closes a socket that exceeds the frame or byte limit", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const a = await socket(room.alias, room.token); await a.next();
  for (let n = 0; n < 25; n++) a.ws.send("{}");
  expect(await a.closed).toBe(1008);
  const b = await socket(room.alias, (await joinRoom(room, nodeB)).token); await b.next();
  for (let n = 0; n < 18; n++) b.ws.send("x".repeat(3900));
  expect(await b.closed).toBe(1008);
});

it("disables a room that exhausts its daily write budget and records an audit row", async () => {
  const room = await convertFixture(nodeA);
  await runInDurableObject(hostedStub(room.alias), (inst: any) => {inst.budgetDay = Math.floor(now() / 86400); inst.budgetCount = 59;});
  expect((await rf(room.alias, room.token, fixture.readerLogin)).status).toBe(200);
  expect((await rf(room.alias, room.token, fixture.path)).status).toBe(403);
  expect((await registrySql("SELECT state FROM rooms"))[0].state).toBe("disabled");
  expect((await registrySql("SELECT action, target, result FROM audit WHERE action='write-budget'"))[0]).toEqual({action: "write-budget", target: room.alias, result: "disabled"});
});

it("counts only frames that name an operation and re-enables a spent room on the next UTC day", async () => {
  const room = await convertFixture(nodeA);
  const day = Math.floor(now() / 86400);
  await runInDurableObject(hostedStub(room.alias), (inst: any) => {inst.budgetDay = day; inst.budgetCount = 59;});
  const s = await socket(room.alias, room.token); await s.next();
  for (let n = 0; n < 5; n++) s.ws.send("{}");
  await new Promise(r => setTimeout(r, 50));
  expect((await registrySql("SELECT state FROM rooms"))[0].state).toBe("active");
  expect((await rf(room.alias, room.token, fixture.readerLogin)).status).toBe(200);
  expect((await rf(room.alias, room.token, fixture.path)).status).toBe(403);
  expect((await control(room.alias, room.token, {op: "nodes"})).body.e).toBe("disabled");
  await hostedSql(room.alias, "UPDATE hosted_meta SET v=? WHERE k='wbdis'", day - 1);
  expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(200);
  expect((await registrySql("SELECT state FROM rooms"))[0].state).toBe("active");
  expect((await registrySql("SELECT result FROM audit WHERE action='write-budget-reset'"))[0].result).toBe("active");
});

it("applies housekeeping thresholds: history, sessions, old rows, idle disable and purge", async () => {
  const room = await convertFixture(nodeA);
  const alias = room.alias, day = Math.floor(now() / 86400);
  const due = () => hostedSql(alias, "INSERT INTO hosted_meta(k, v) VALUES ('daily_at', 0) ON CONFLICT(k) DO UPDATE SET v=0");
  for (let n = 1; n <= 30; n++)
    await hostedSql(alias, "INSERT INTO messages(timestamp, origin_alias, author, client_timestamp, text) VALUES (?, ?, ?, ?, ?)", 1000 + n, alias, "aa", n, "m" + n);
  for (let n = 0; n < 1100; n++)
    await hostedSql(alias, "INSERT INTO sessions(alias, client, cursor, last_timestamp, frontend, route) VALUES (?, ?, 0, ?, 'f', 'r')", alias, String(n).padStart(64, "0"), n);
  for (let n = 0; n < 5; n++) await hostedSql(alias, "INSERT INTO attempts VALUES (?, 'c', 'k', ?, 'f')", alias, "a" + n);
  const maxAttempt = (await hostedSql(alias, "SELECT MAX(rowid) AS n FROM attempts"))[0].n;
  await hostedSql(alias, "INSERT INTO hosted_meta(k, v) VALUES (?, 1), (?, ?), (?, 0)", `mark:${day - 14}`, `amax:${day - 14}`, maxAttempt, `dmax:${day - 14}`);
  await due();
  await runDurableObjectAlarm(hostedStub(alias));
  expect((await hostedSql(alias, "SELECT COUNT(*) AS n FROM messages"))[0].n).toBe(20);
  expect((await hostedSql(alias, "SELECT MIN(seq) AS n FROM messages"))[0].n).toBe(11);
  expect((await hostedSql(alias, "SELECT COUNT(*) AS n FROM sessions"))[0].n).toBe(1000);
  expect((await hostedSql(alias, "SELECT COUNT(*) AS n FROM attempts"))[0].n).toBe(0);
  expect((await hostedSql(alias, "SELECT v FROM hosted_meta WHERE k='daily_at'"))[0].v).toBeGreaterThan(now());
  // idle for 180 days: disabled; 365: purged
  await hostedSql(alias, "UPDATE hosted_room SET last_seen=?", now() - 181 * 86400);
  await due();
  await runDurableObjectAlarm(hostedStub(alias));
  expect((await hostedSql(alias, "SELECT state FROM hosted_room"))[0].state).toBe("disabled");
  expect((await registrySql("SELECT state FROM rooms"))[0].state).toBe("disabled");
  await hostedSql(alias, "UPDATE hosted_room SET last_seen=?", now() - 366 * 86400);
  await due();
  await runDurableObjectAlarm(hostedStub(alias));
  expect((await hostedSql(alias, "SELECT state FROM hosted_room"))[0].state).toBe("deleted");
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
});

it("enforces owner rules: last owner, too-new owners, deferred delete and invite limits", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const member = await joinRoom(room, nodeB);
  // Last owner cannot leave while others remain; a frontend can.
  expect((await control(room.alias, room.token, {op: "leave"})).body.e).toBe("last_owner");
  expect((await control(room.alias, room.token, {op: "revoke", id: nodeB.pub.slice(0, 8)})).status).toBe(200);
  const second = await joinRoom(room, nodeC, "owner");
  expect(second.role).toBe("owner");
  // A freshly seated owner cannot revoke or delete for 24 h.
  expect((await control(room.alias, second.token, {op: "revoke", id: nodeA.pub.slice(0, 8)})).body.e).toBe("too_new");
  expect((await control(room.alias, second.token, {op: "delete", confirm: room.alias})).body.e).toBe("too_new");
  // The founder can delete: with another owner present the delete is deferred and cancellable.
  const pending = await control(room.alias, room.token, {op: "delete", confirm: room.alias});
  expect(pending.body).toEqual({ok: 1, pending: 86400});
  expect((await control(room.alias, room.token, {op: "delete", confirm: "wrong"})).body.e).toBe("bad_request");
  // Any other owner may cancel, even a newly seated one.
  expect((await control(room.alias, second.token, {op: "cancel_delete"})).body).toEqual({ok: 1});
  expect((await hostedSql(room.alias, "SELECT pending_delete FROM hosted_room"))[0].pending_delete).toBeNull();
  // The deferred delete fires from the daily alarm.
  await control(room.alias, room.token, {op: "delete", confirm: room.alias});
  await hostedSql(room.alias, "UPDATE hosted_room SET pending_delete=?", now() - 1);
  await hostedSql(room.alias, "INSERT INTO hosted_meta(k, v) VALUES ('daily_at', 0) ON CONFLICT(k) DO UPDATE SET v=0");
  await runDurableObjectAlarm(hostedStub(room.alias));
  expect((await control(room.alias, room.token, {op: "nodes"})).status).toBe(410);
  void member;
});

it("limits invites: four live codes, owner ttl cap and codes per day", async () => {
  const room = await createRoom(nodeA, "Fleet");
  for (let n = 0; n < 4; n++) await invite(room);
  expect((await control(room.alias, room.token, {op: "invite"})).body.e).toBe("cap");
  const [fresh] = [await createRoom(nodeB, "Other")];
  const ok = await control(fresh.alias, fresh.token, {op: "invite", role: "owner"});
  expect(ok.body.ttl).toBe(120);
  expect((await control(fresh.alias, fresh.token, {op: "invite", role: "owner", ttl: 901})).body.e).toBe("bad_request");
  expect((await control(fresh.alias, fresh.token, {op: "invite", role: "frontend", ttl: 86400})).body.ttl).toBe(86400);
  expect((await control(fresh.alias, fresh.token, {op: "invite", ttl: 59})).body.e).toBe("bad_request");
  expect((await control(fresh.alias, fresh.token, {op: "bogus"})).body.e).toBe("bad_request");
  // 20 minted per day per room.
  await registrySql("DELETE FROM codes WHERE alias=?", fresh.alias);
  await registrySql("UPDATE limits SET count=20 WHERE key=?", `mj:${fresh.alias}`);
  expect((await control(fresh.alias, fresh.token, {op: "invite"})).body.e).toBe("cap");
});

it("pages node listings five at a time", async () => {
  const room = await createRoom(nodeA, "Fleet");
  for (let n = 0; n < 6; n++) {
    const code = await invite(room);
    const r = await redeem((await import("./registry-helpers")).nodeFrom(0x50 + n), "join", {code});
    expect(r.status, JSON.stringify(r.body)).toBe(200);
  }
  const first = await control(room.alias, room.token, {op: "nodes"});
  expect(first.body.nodes).toHaveLength(5);
  expect(first.body).toMatchObject({total: 7, next: 5});
  const last = await control(room.alias, room.token, {op: "nodes", offset: 5});
  expect(last.body.nodes).toHaveLength(2);
  expect(last.body.next).toBeNull();
  void json;
});
