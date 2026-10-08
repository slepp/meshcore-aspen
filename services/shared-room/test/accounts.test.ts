import {expect, it} from "vitest";
import {SELF, runInDurableObject, evictDurableObject} from "cloudflare:test";
import {bindings, crypto as nativeCrypto, fixture, rf, sockets, connect, history, state, seal} from "./native-helpers";
import {fromHex, toHex, join as bytes} from "../src/native-crypto";
import {le32} from "../src/native";
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

it("keeps native radio delivery working after an account browser reload and Durable Object wake", async () => {
  const radio = await connect("one");
  await rf("one", fixture.readerLogin);
  await rf("one", fixture.path);
  const account = await join();
  const browser = await webSocket("A", account.cookie, sockets);
  const first = await webPost("A", account.cookie, "before reload");
  const firstRadio = history(await radio.next("account post before reload"));
  expect(firstRadio.text).toBe(first.message.text);
  expect((await rf("one", firstRadio.ack)).accepted).toBe(true);
  browser.ws.close();
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  const restored = await webRequest("A", "session", account.cookie);
  expect(restored.status).toBe(200);
  expect(await restored.json()).toMatchObject({author: account.author, username: "alice"});
  await webSocket("A", account.cookie, sockets, first.message.seq);
  const second = await webPost("A", account.cookie, "after reload");
  const secondRadio = history(await radio.next("account post after reload"));
  expect(secondRadio.text).toBe(second.message.text);
  expect((await rf("one", secondRadio.ack)).accepted).toBe(true);
  expect((await state()).sessions.find(s => s.client === fixture.reader.publicKey)?.cursor).toBe(second.message.seq);
  expect((await state()).pending).toHaveLength(0);
});

it("holds later account posts behind a missing native ACK until a fresh radio login, not a browser reload", async () => {
  const radio = await connect("one");
  await rf("one", fixture.readerLogin);
  const account = await join();
  const browser = await webSocket("A", account.cookie, sockets);
  const first = await webPost("A", account.cookie, "waiting for the radio ACK");
  const missingAck = history(await radio.next("unconfirmed native history"));
  expect(missingAck.text).toBe(first.message.text);
  const pending = (await state()).pending;
  browser.ws.close();
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  await webSocket("A", account.cookie, sockets, first.message.seq);
  const second = await webPost("A", account.cookie, "queued after reload");
  expect((await state()).pending).toEqual(pending);
  expect(radio.inbox).toHaveLength(0);
  const timestamp = Math.floor(Date.now() / 1000);
  const login = seal(fixture.room, fixture.reader, 7,
    bytes(le32(timestamp), le32(0), new TextEncoder().encode("room\0")));
  expect((await rf("one", login)).accepted).toBe(true);
  const replay = history(await radio.next("fresh radio login replays unconfirmed history"));
  expect(replay.text).toBe(first.message.text);
  expect((await rf("one", replay.ack)).accepted).toBe(true);
  const queued = history(await radio.next("later account post after radio ACK"));
  expect(queued.text).toBe(second.message.text);
  expect((await rf("one", queued.ack)).accepted).toBe(true);
  expect((await state()).pending).toHaveLength(0);
});
