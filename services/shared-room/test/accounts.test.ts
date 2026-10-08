import {expect, it} from "vitest";
import {SELF, runInDurableObject, evictDurableObject} from "cloudflare:test";
import {bindings, crypto as nativeCrypto, fixture, rf, sockets} from "./native-helpers";
import {fromHex, toHex} from "../src/native-crypto";
import {accountCredential, checkPassword, deviceChallenge, webUsers} from "../src/accounts";
import {webRequest, webPost, webSocket, origin} from "./web-helpers";

async function proof(alias = "A", username = "alice", peer = fixture.author) {
  const response = await webRequest(alias, "challenge", undefined, {username, publicKey: peer.publicKey});
  expect(response.status, await response.clone().text()).toBe(200);
  const challenge = await response.json<{message: string; nonce: string; expires: number}>();
  expect(challenge.message).toBe(deviceChallenge(origin, alias, username, peer.publicKey, challenge.nonce));
  return {username, publicKey: peer.publicKey, nonce: challenge.nonce,
    signature: toHex(nativeCrypto.sign(fromHex(peer.key), new TextEncoder().encode(challenge.message))),
    password: username === "bob" ? "another fixture password" : "fixture password only"};
}
async function join(alias = "A", username = "alice", peer = fixture.author) {
  const response = await webRequest(alias, "login", undefined, await proof(alias, username, peer));
  expect(response.status, await response.clone().text()).toBe(200);
  return {cookie: response.headers.get("Set-Cookie")!.split(";")[0],
    ...await response.json<{author: string; name: string; username: string}>()};
}

it("uses operator accounts and local device keys without creating radio membership", async () => {
  const listing = await (await SELF.fetch(`${origin}/v1/web/rooms`)).json<Record<string, unknown>>();
  expect(listing).toMatchObject({loginMode: "account", deviceProtocol: "aspen-room.device.v1"});
  expect(JSON.stringify(listing)).not.toMatch(/alice|salt|hash|fixture password/);
  const first = await join();
  expect(first).toMatchObject({author: fixture.author.publicKey, name: "Alice", username: "alice"});
  const otherDevice = await join("A", "alice", fixture.reader);
  expect(otherDevice.author).not.toBe(first.author);
  const message = await webPost("A", first.cookie, "real device key");
  expect(message.message).toMatchObject({author: fixture.author.publicKey, text: "Alice: real device key"});
  const page = await (await webRequest("A", "history", first.cookie)).json<{profiles: unknown[]}>();
  expect(page.profiles).toContainEqual(expect.objectContaining({publicKey: fixture.author.publicKey, source: "desktop", name: "Alice"}));
  expect(await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("SELECT COUNT(*) AS n FROM sessions").one().n)).toBe(0);
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  expect((await webRequest("A", "session", first.cookie)).status).toBe(200);
});

it("rejects passwords, room grants, unsigned identities and reused or mismatched challenges", async () => {
  const body = await proof();
  expect((await webRequest("A", "login", undefined, {...body, password: "wrong"})).status).toBe(403);
  expect((await webRequest("A", "login", undefined, body)).status).toBe(403);
  expect((await webRequest("A", "login", undefined, {identity: "aa".repeat(32), name: "Spoof", password: "room"})).status).toBe(400);
  const forged = await proof();
  expect((await webRequest("A", "login", undefined, {...forged, signature: "00".repeat(64)})).status).toBe(403);
  const wrongRoom = await proof();
  expect((await webRequest("SharedB", "login", undefined, wrongRoom)).status).toBe(403);
  const denied = await proof("A", "bob");
  expect((await webRequest("A", "login", undefined, denied)).status).toBe(403);
  const unknown = await proof("A", "nobody");
  expect((await webRequest("A", "login", undefined, unknown)).status).toBe(403);
  const expired = await proof();
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("UPDATE web_challenges SET expires=0 WHERE nonce=?", expired.nonce));
  expect((await webRequest("A", "login", undefined, expired)).status).toBe(403);
});

it("binds a device to one account and deduplicates uncertain posts across relogin and aliases", async () => {
  const session = await join(), id = crypto.randomUUID();
  const sent = await webPost("A", session.cookie, "once", id);
  const remote = await join("SharedB");
  expect(await webPost("SharedB", remote.cookie, "once", id)).toEqual({...sent, duplicate: true});
  const collision = await proof("SharedB", "bob");
  expect((await webRequest("SharedB", "login", undefined, collision)).status).toBe(409);
  const unrelated = await join("SharedB", "bob", fixture.reader);
  expect(unrelated.author).toBe(fixture.reader.publicKey);
});

it("invalidates account changes/removal and keeps radio logins independent of web accounts", async () => {
  const session = await join(), socket = await webSocket("A", session.cookie, sockets);
  const changed = {...bindings, WEB_USERS: JSON.stringify({...webUsers(bindings),
    alice: {...webUsers(bindings)!.alice, name: "Changed"}})};
  const changedGrant = {...bindings, WEB_USERS: JSON.stringify({...webUsers(bindings),
    alice: {...webUsers(bindings)!.alice, aliases: ["SharedB"]}})};
  expect(await accountCredential(changed, "A", "alice")).not.toBe(await accountCredential(bindings, "A", "alice"));
  await expect(accountCredential(changedGrant, "A", "alice")).rejects.toThrow(/access changed/);
  await expect(accountCredential({...bindings, WEB_USERS: "{}"}, "A", "alice")).rejects.toThrow(/access changed/);
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("UPDATE web_sessions SET credential='revoked'"));
  expect((await webRequest("A", "history", session.cookie)).status).toBe(401);
  const closed = new Promise<number>(resolve => socket.ws.addEventListener("close", event => resolve(event.code)));
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  expect(await closed).toBe(1008);
});

it("matches browser/standard Ed25519 ownership proofs with native MeshCore verification", async () => {
  const pair = await crypto.subtle.generateKey("Ed25519", false, ["sign", "verify"]);
  const publicKey = toHex(new Uint8Array(await crypto.subtle.exportKey("raw", pair.publicKey)));
  const challenge = await (await webRequest("A", "challenge", undefined, {username: "alice", publicKey}))
    .json<{nonce: string; message: string}>();
  const signature = toHex(new Uint8Array(await crypto.subtle.sign("Ed25519", pair.privateKey,
    new TextEncoder().encode(challenge.message))));
  const response = await webRequest("A", "login", undefined,
    {username: "alice", publicKey, nonce: challenge.nonce, signature, password: "fixture password only"});
  expect(response.status, await response.clone().text()).toBe(200);
  expect(await checkPassword("fixture password only", webUsers(bindings)!.alice)).toBe(true);
  expect(await checkPassword("fixture password only")).toBe(false);
});
