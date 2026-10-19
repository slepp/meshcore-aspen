import {expect, it} from "vitest";
import {runDurableObjectAlarm} from "cloudflare:test";
import {admin, control, createRoom, hostedSql, json, mintCreate, nodeA, nodeB, nodeC, now, redeem, registrySql, registryStub, socket} from "./registry-helpers";
import {roomOps} from "../src/room-ops";

it("exists only when ADMIN_TOKEN_HASH is set and refuses wrong tokens", async () => {
  expect((await json("/v1/admin/rooms", {}, {token: "fixture-admin-token", env: {ADMIN_TOKEN_HASH: undefined}})).status).toBe(404);
  expect((await json("/v1/admin/rooms", {}, {token: "wrong"})).body.e).toBe("auth");
  expect((await json("/v1/admin/rooms", {})).body.e).toBe("auth");
  expect((await json("/v1/admin/nope", {}, {token: "fixture-admin-token"})).body.e).toBe("not_found");
  expect((await json("/v1/admin/rooms", {}, {token: "fixture-admin-token", method: "GET"})).body.e).toBe("bad_request");
});

it("mints create codes within bounds and shows them once", async () => {
  for (const bad of [{kind: "create", count: 0}, {kind: "create", count: 21}, {kind: "join"}, {kind: "create", ttl: 59}, {kind: "create", ttl: 2592001}])
    expect((await admin("mint", bad)).body.e).toBe("bad_request");
  const r = await admin("mint", {kind: "create", count: 3, ttl: 120});
  expect(r.body.codes).toHaveLength(3);
  expect(r.body.expires).toBeGreaterThan(now() + 100);
  const listed = await admin("codes", {});
  expect(listed.body.codes).toHaveLength(3);
  expect(listed.body.codes[0]).toMatchObject({kind: "create", issuer: "admin", state: "new"});
  expect(listed.body.codes[0].h).toMatch(/^[a-f0-9]{8}$/);
  expect(JSON.stringify(listed.body)).not.toContain(r.body.codes[0].replace(/-/g, ""));
});

it("revokes codes by hash prefix, refusing ambiguous or unknown prefixes", async () => {
  const [code] = await mintCreate();
  const [h] = (await admin("codes", {})).body.codes.map((c: any) => c.h);
  expect((await admin("code", {h: "00000000", action: "revoke"})).body.e).toBe("not_found");
  expect((await admin("code", {h: "zz", action: "revoke"})).body.e).toBe("bad_request");
  expect((await admin("code", {h, action: "revoke"})).body).toEqual({ok: 1});
  expect((await redeem(nodeA, "create", {code, name: "N"})).body.e).toBe("code_invalid");
  await mintCreate(); await mintCreate();
  await registrySql("UPDATE codes SET hash='aa' || substr(hash, 3)");
  expect((await admin("code", {h: "aaaaaaaa".slice(0, 2).padEnd(8, "0"), action: "revoke"})).body.e).toMatch(/not_found|bad_request/);
});

it("lists rooms in pages of 20 and re-seats a lost sole owner", async () => {
  const room = await createRoom(nodeA, "Fleet");
  for (let n = 0; n < 22; n++)
    await registrySql("INSERT INTO rooms(alias, state, owner_node, created, last_active, pub_hash, name) VALUES (?, 'active', 'zz', ?, ?, ?, 'x')", "q" + n, now() + 10 + n, now(), "ph" + n);
  const page = await admin("rooms", {});
  expect(page.body.rooms).toHaveLength(20);
  expect(page.body.next).toBe(20);
  expect(page.body.rooms[0]).toMatchObject({alias: room.alias, name: "Fleet", state: "active", owner: nodeA.pub.slice(0, 8)});
  expect((await admin("rooms", {offset: 20})).body).toMatchObject({next: null});
  // The only owner node is lost: the operator mints an owner code for a new node.
  const invite = await admin("invite", {alias: room.alias, role: "owner"});
  expect(invite.body).toMatchObject({ok: 1, ttl: 900});
  const seated = await redeem(nodeB, "join", {code: invite.body.code});
  expect(seated.body.role).toBe("owner");
  expect((await control(room.alias, seated.body.token, {op: "nodes"})).body.total).toBe(2);
  expect((await admin("invite", {alias: room.alias, role: "frontend"})).body.e).toBe("bad_request");
  expect((await admin("invite", {alias: "rabcdefghijklmnopqrstuvwxyz234", role: "owner"})).body.e).toBe("not_found");
  expect((await registrySql("SELECT action FROM admin_audit WHERE action='invite-owner'"))).toHaveLength(1);
});

it("bans for a bounded time and lifts bans", async () => {
  expect((await admin("node", {node: nodeA.pub, action: "ban", until: now() - 5})).body.e).toBe("bad_request");
  expect((await admin("node", {node: "short", action: "ban"})).body.e).toBe("bad_request");
  expect((await admin("node", {node: nodeA.pub, action: "ban", until: now() + 3600})).body).toEqual({ok: 1});
  const [code] = await mintCreate();
  expect((await redeem(nodeA, "create", {code, name: "N"})).body.e).toBe("banned");
  await registrySql("UPDATE bans SET until=?", now() - 1);
  expect((await redeem(nodeA, "create", {code, name: "N"})).status).toBe(200);
});

it("takes rooms down with Room object first and lets the Registry alarm re-drive a failed call", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const original = roomOps.setState;
  roomOps.setState = async () => {throw new Error("boom");};
  try {
    expect((await admin("room", {alias: room.alias, action: "disable"})).body).toEqual({ok: 1});
  } finally {roomOps.setState = original;}
  expect((await registrySql("SELECT state, dirty FROM rooms"))[0]).toMatchObject({state: "disabled", dirty: 1});
  expect((await hostedSql(room.alias, "SELECT state FROM hosted_room"))[0].state).toBe("active");
  await runDurableObjectAlarm(registryStub());
  expect((await hostedSql(room.alias, "SELECT state FROM hosted_room"))[0].state).toBe("disabled");
  expect((await registrySql("SELECT dirty FROM rooms"))[0].dirty).toBe(0);
  expect((await control(room.alias, room.token, {op: "nodes"})).body.e).toBe("disabled");
  // Disabled rooms keep data for 30 days, then the alarm purges them and frees the quota.
  await registrySql("UPDATE rooms SET state_since=?", now() - 31 * 86400);
  await runDurableObjectAlarm(registryStub());
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
  expect((await hostedSql(room.alias, "SELECT state FROM hosted_room"))[0].state).toBe("deleted");
  expect((await admin("room", {alias: room.alias, action: "enable"})).body.e).toBe("not_found");
  // Leaving a disabled room is still possible: the last owner purges it.
  const other = await createRoom(nodeB, "Other");
  await admin("room", {alias: other.alias, action: "disable"});
  expect((await control(other.alias, other.token, {op: "leave"})).body).toMatchObject({ok: 1, deleted: 1});
  expect(await registrySql("SELECT * FROM rooms")).toEqual([]);
});

it("revokes a banned node in every room and the alarm retries what failed", async () => {
  const room = await createRoom(nodeA, "Fleet");
  const code = (await control(room.alias, room.token, {op: "invite"})).body.code;
  const joined = (await redeem(nodeC, "join", {code})).body;
  const original = roomOps.revokeNode;
  roomOps.revokeNode = async () => {throw new Error("boom");};
  try {
    await admin("node", {node: nodeC.pub, action: "ban"});
  } finally {roomOps.revokeNode = original;}
  expect(await registrySql("SELECT * FROM members WHERE node=?", nodeC.pub)).toHaveLength(1);
  expect((await control(room.alias, joined.token, {op: "nodes"})).status).toBe(403);
  await runDurableObjectAlarm(registryStub());
  expect(await registrySql("SELECT * FROM members WHERE node=?", nodeC.pub)).toEqual([]);
  expect((await control(room.alias, joined.token, {op: "nodes"})).body.e).toBe("auth");
  void socket;
});

it("purges expired codes, old audit rows and stale limits in the hourly alarm", async () => {
  await mintCreate();
  await registrySql("UPDATE codes SET expires=?", now() - 4000);
  await registrySql("INSERT INTO audit(ts, actor, action, target, result) VALUES (?, 'a', 'x', 't', 'ok')", now() - 31 * 86400);
  await registrySql("INSERT INTO limits VALUES ('old', ?, 1)", now() - 100000);
  await runDurableObjectAlarm(registryStub());
  expect(await registrySql("SELECT * FROM codes")).toEqual([]);
  expect(await registrySql("SELECT * FROM audit")).toEqual([]);
  expect(await registrySql("SELECT * FROM limits WHERE key='old'")).toEqual([]);
});
