import {expect, it, vi} from "vitest";
import {evictDurableObject} from "cloudflare:test";
import {admin, bindings, call, control, convertFixture, createRoom, fixture, hostedSql, hostedStub, json, mintCreate, nodeA, nodeB, now, redeem,
  registryDump, registrySql, signedBody, socket} from "./registry-helpers";
import {b32, clientKey, ipv6Prefix, newAliasId, openRoom, parseKek, sealRoom, validAliasId} from "../src/seal";
import {fromHex, toHex} from "../src/native-crypto";

const kek = parseKek({ROOM_KEK: JSON.stringify({current: "k1", keys: {k1: "42".repeat(32)}})})!;
const key = fromHex(fixture.room.key);

it("seals and opens a room secret and fails closed on any tampering", async () => {
  const {kid, sealed} = await sealRoom(kek, "ralias", fixture.room.publicKey, key, "pass word");
  expect(kid).toBe("k1");
  expect(sealed.length).toBe(28 + 65 + "pass word".length + 16);
  const opened = await openRoom(kek, "ralias", fixture.room.publicKey, kid, sealed);
  expect(toHex(opened.key)).toBe(fixture.room.key);
  expect(opened.password).toBe("pass word");
  await expect(openRoom(kek, "rother", fixture.room.publicKey, kid, sealed)).rejects.toThrow();
  await expect(openRoom(kek, "ralias", fixture.otherRoom.publicKey, kid, sealed)).rejects.toThrow();
  await expect(openRoom(kek, "ralias", fixture.room.publicKey, "k2", sealed)).rejects.toThrow();
  for (const at of [0, 15, 16, 27, 28, 60, sealed.length - 1]) {
    const bad = sealed.slice(); bad[at] ^= 1;
    await expect(openRoom(kek, "ralias", fixture.room.publicKey, kid, bad), `byte ${at}`).rejects.toThrow();
  }
  await expect(openRoom(kek, "ralias", fixture.room.publicKey, kid, sealed.slice(0, 80))).rejects.toThrow();
  const other = parseKek({ROOM_KEK: JSON.stringify({current: "k1", keys: {k1: "43".repeat(32)}})})!;
  await expect(openRoom(other, "ralias", fixture.room.publicKey, kid, sealed)).rejects.toThrow();
  // Two seals of the same secret differ (fresh salt and nonce).
  expect(toHex((await sealRoom(kek, "ralias", fixture.room.publicKey, key, "pass word")).sealed)).not.toBe(toHex(sealed));
});

it("rotates KEK ids: old rooms open under their key id and new rooms use the current one", async () => {
  const rotated = parseKek({ROOM_KEK: JSON.stringify({current: "k2", keys: {k1: "42".repeat(32), k2: "24".repeat(32)}})})!;
  const old = await sealRoom(kek, "ralias", fixture.room.publicKey, key, "");
  expect((await openRoom(rotated, "ralias", fixture.room.publicKey, "k1", old.sealed)).password).toBe("");
  expect((await sealRoom(rotated, "ralias", fixture.room.publicKey, key, "")).kid).toBe("k2");
  const alias = await newAliasId(kek);
  expect(await validAliasId(rotated, alias)).toBe(true);
  expect(await validAliasId(parseKek({ROOM_KEK: JSON.stringify({current: "k2", keys: {k2: "24".repeat(32)}})})!, alias)).toBe(false);
  expect(parseKek({ROOM_KEK: "{}"})).toBeUndefined();
  expect(parseKek({ROOM_KEK: JSON.stringify({current: "k9", keys: {k1: "42".repeat(32)}})})).toBeUndefined();
  expect(parseKek({ROOM_KEK: JSON.stringify({current: "k1", keys: {k1: "zz"}})})).toBeUndefined();
  expect(parseKek({})).toBeUndefined();
});

it("issues 30 character self-authenticating alias ids", async () => {
  const alias = await newAliasId(kek);
  expect(alias).toMatch(/^r[a-z2-7]{29}$/);
  expect(await validAliasId(kek, alias)).toBe(true);
  expect(await validAliasId(kek, alias.slice(0, 14) + "a".repeat(16))).toBe(false);
  expect(await validAliasId(kek, "r" + "a".repeat(29))).toBe(false);
  const mutated = alias.slice(0, 5) + (alias[5] === "a" ? "b" : "a") + alias.slice(6);
  expect(await validAliasId(kek, mutated)).toBe(false);
  expect(b32(new Uint8Array([0xff, 0x00]))).toBe("74aa");
});

it("keeps a corrupted or unopenable hosted room closed", async () => {
  const room = await convertFixture(nodeA);
  await hostedSql(room.alias, "UPDATE hosted_room SET kid='k9'");
  await evictDurableObject(hostedStub(room.alias));
  const r = await call(`/v1/aliases/${room.alias}/socket`, {headers: {Authorization: `Bearer ${room.token}`, Upgrade: "websocket"}});
  expect(r.status).toBe(503);
  await r.text();
  expect((await control(room.alias, room.token, {op: "password", mode: "get"})).body.e).toBe("closed");
});

it("never logs or audits request secrets, including malformed conversions", async () => {
  const spies = (["log", "info", "warn", "error", "debug"] as const).map(m => vi.spyOn(console, m));
  const [code] = await mintCreate();
  const secretKey = "ab".repeat(64), secretPw = "SECRETPW";
  await redeem(nodeA, "convert", {code, name: "N", pub: "cd".repeat(32), key: secretKey, pw: secretPw});
  await redeem(nodeA, "convert", {code, name: "N", pub: fixture.room.publicKey, key: secretKey, pw: secretPw, mutate: b => {b.extra = secretKey;}});
  await redeem(nodeA, "convert", {code, name: "N", pub: fixture.room.publicKey, key: secretKey, pw: secretPw, origin: "https://elsewhere.example"});
  const good = await redeem(nodeA, "convert", {code, name: "Conv", pub: fixture.room.publicKey, key: fixture.room.key, pw: secretPw});
  expect(good.status).toBe(200);
  await json("/v1/node/redeem", "{\"key\":\"" + secretKey + "\"");
  await control(good.body.alias, good.body.token, {op: "password", mode: "get"});
  await control(good.body.alias, "ar1_" + "c".repeat(64), {op: "nodes"});
  const logged = JSON.stringify(spies.flatMap(s => s.mock.calls));
  const stored = await registryDump();
  for (const secret of [secretKey, secretPw, fixture.room.key, good.body.token, code.replace(/-/g, ""), nodeA.pub + "ff"]) {
    expect(logged).not.toContain(secret);
    expect(stored).not.toContain(secret);
  }
  // Authenticated events only, never an address.
  expect((await registrySql("PRAGMA table_info(audit)")).map(c => c.name)).toEqual(["id", "ts", "actor", "action", "target", "result"]);
  expect((await registrySql("SELECT action FROM audit")).map(r => r.action)).toEqual(["redeem-owner"]);
  void bindings; void admin; void nodeB; void now; void createRoom; void signedBody; void socket;
});

it("derives one client key per IPv6 /64 and per IPv4 address", async () => {
  expect(ipv6Prefix("2001:db8::1")).toBe("2001:db8:0:0");
  expect(ipv6Prefix("2001:0db8:0000:0000:aaaa:bbbb:cccc:dddd")).toBe("2001:db8:0:0");
  expect(ipv6Prefix("::1")).toBe("0:0:0:0");
  expect(ipv6Prefix("2001:db8:1:2:3:4:5:6")).toBe("2001:db8:1:2");
  const day = 20000 * 86400;
  const key = (ip: string) => clientKey(kek, ip, day);
  expect(await key("2001:db8::1")).toBe(await key("2001:db8::2"));
  expect(await key("2001:db8::1")).toBe(await key("2001:0db8:0:0:ffff:ffff:ffff:ffff"));
  expect(await key("2001:db8::1")).not.toBe(await key("2001:db8:0:1::1"));
  expect(await key("::ffff:192.0.2.1")).toBe(await key("192.0.2.1"));
  expect(await key("192.0.2.1")).not.toBe(await key("192.0.2.2"));
  expect(await clientKey(kek, "192.0.2.1", day + 86400)).not.toBe(await key("192.0.2.1"));
  expect(await key("192.0.2.1")).toMatch(/^[a-f0-9]{32}$/);
});
