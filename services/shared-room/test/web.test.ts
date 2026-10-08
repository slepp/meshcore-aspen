import {SELF, env, reset, runInDurableObject, evictDurableObject} from "cloudflare:test";
import {afterEach, expect, it} from "vitest";
import type {Env} from "../src/config";
import {credential} from "../src/config";
import {joinWeb, webPost, webRequest, webSocket, device, origin} from "./web-helpers";

const bindings = env as unknown as Env;
const sockets: WebSocket[] = [];
afterEach(async () => {for (const ws of sockets.splice(0)) ws.close(); await reset();});

it("lists public room metadata without credentials, passwords or histories", async () => {
  const response = await SELF.fetch(`${origin}/v1/web/rooms`);
  expect(response.headers.get("Cache-Control")).toBe("no-store");
  const value = await response.json<{rooms: unknown[]; maxPostBytes: number}>();
  expect(value.maxPostBytes).toBe(151);
  expect(value.rooms).toContainEqual({id: "A", name: "A", publicKey: "11".repeat(32)});
  expect(JSON.stringify(value)).not.toMatch(/"(password|backend|token|messages)":/);
});

it("requires room-specific cookies and same-origin login/posts without accepting adapter tokens", async () => {
  expect((await webRequest("A", "history")).status).toBe(401);
  expect((await webRequest("A", "history", undefined, undefined, {Authorization: "Bearer local-token"})).status).toBe(401);
  expect((await webRequest("A", "login", undefined, {identity: device(1), name: "Alice", password: "wrong"})).status).toBe(403);
  expect((await webRequest("A", "login", undefined, {identity: device(1), name: "Alice", password: "room"}, {Origin: "https://evil.test"})).status).toBe(403);
  const session = await joinWeb();
  expect((await webRequest("B", "history", session.cookie)).status).toBe(401);
  expect((await webRequest("A", "posts", session.cookie, {id: crypto.randomUUID(), text: "no"}, {Origin: ""})).status).toBe(403);
  expect((await webRequest("A", "posts", session.cookie, {id: crypto.randomUUID(), text: "no"}, {Origin: "https://evil.test"})).status).toBe(403);
  expect((await webRequest("A", "socket?since=0", session.cookie, undefined,
    {Upgrade: "websocket", "Sec-WebSocket-Protocol": "aspen-room.web.v1", Origin: "https://evil.test"})).status).toBe(403);
});

it("issues HttpOnly scoped cookies, retains device author across logins and does not create radio membership", async () => {
  const response = await webRequest("A", "login", undefined, {identity: device(1), name: "Alice", password: "room"});
  expect(response.headers.get("Set-Cookie")).toMatch(/Path=\/v1\/web\/rooms\/A\/; Max-Age=2592000; HttpOnly; Secure; SameSite=Strict/);
  const first = await response.json<{author: string}>();
  const next = await joinWeb("A", 1, "New name");
  expect(next.author).toBe(first.author);
  expect((await joinWeb("B", 1, "New name", "room-b")).author).toBe(first.author);
  const n = await runInDurableObject(bindings.ROOMS.getByName("shared"), (_room, s) =>
    s.storage.sql.exec("SELECT COUNT(*) AS n FROM sessions").one().n);
  expect(n).toBe(0);
});

it("stores one canonical post across aliases, echoes to all web readers and isolates other backends", async () => {
  const a = await joinWeb(), b = await joinWeb("B", 2, "Bob", "room-b"), c = await joinWeb("C", 3, "Carol", "private");
  const readerA = await webSocket("A", a.cookie, sockets), readerB = await webSocket("B", b.cookie, sockets);
  const readerC = await webSocket("C", c.cookie, sockets);
  const sent = await webPost("A", a.cookie, "Hello, mesh.");
  expect(sent.message).toMatchObject({author: a.author, text: "Alice: Hello, mesh.", webName: "Alice", seq: 1});
  expect((await readerA.next()).message).toEqual(sent.message);
  expect((await readerB.next()).message).toEqual(sent.message);
  expect(readerC.inbox).toHaveLength(0);
  const shared = await webRequest("B", "history", b.cookie);
  expect((await shared.json<{messages: unknown[]}>()).messages).toEqual([sent.message]);
  expect((await (await webRequest("C", "history", c.cookie)).json<{messages: unknown[]}>()).messages).toEqual([]);
});

it("deduplicates uncertain post retries after reconnect, alias changes and display-name changes", async () => {
  const session = await joinWeb(), id = crypto.randomUUID();
  const sent = await webPost("A", session.cookie, "once", id);
  await evictDurableObject(bindings.ROOMS.getByName("shared"));
  const renamed = await joinWeb("B", 1, "Renamed", "room-b");
  const retry = await webPost("B", renamed.cookie, "once", id);
  expect(retry).toEqual({...sent, duplicate: true});
  expect((await webRequest("B", "posts", renamed.cookie, {id, text: "different"})).status).toBe(409);
  expect((await (await webRequest("B", "history", renamed.cookie)).json<{messages: unknown[]}>()).messages).toHaveLength(1);
});

it("validates native text budgets including display name, UTF-8, malformed JSON and post IDs", async () => {
  const session = await joinWeb("A", 1, "Zoë");
  const prefixBytes = new TextEncoder().encode("Zoë: ").length;
  const fits = "x".repeat(151 - prefixBytes);
  expect(new TextEncoder().encode((await webPost("A", session.cookie, fits)).message.text)).toHaveLength(151);
  for (const text of [fits + "x", "\0", "\ud800", ""]) {
    expect((await webRequest("A", "posts", session.cookie, {id: crypto.randomUUID(), text})).status).toBe(400);
  }
  expect((await webRequest("A", "posts", session.cookie, {id: "bad", text: "hello"})).status).toBe(400);
  expect((await webRequest("A", "posts", session.cookie, {id: crypto.randomUUID(), text: "hello"}, {"Content-Type": "text/plain"})).status).toBe(415);
  expect((await webRequest("A", "posts", session.cookie, {id: crypto.randomUUID(), text: "x".repeat(5000)})).status).toBe(413);
  for (const name of ["\0", "Bad: name", " name ", "🙂".repeat(7), "\ud800"]) {
    expect((await webRequest("A", "login", undefined, {identity: device(2), password: "room", name})).status).toBe(400);
  }
});

it("paginates reconnect catch-up and older history without mixing duplicate live messages", async () => {
  const session = await joinWeb();
  for (let i = 0; i < 105; i++) await webPost("A", session.cookie, `post ${i}`);
  const latest = await (await webRequest("A", "history", session.cookie)).json<{messages: {seq: number}[]; more: boolean}>();
  expect(latest.messages).toHaveLength(100);
  expect(latest.messages[0].seq).toBe(6);
  expect(latest.more).toBe(true);
  const older = await (await webRequest("A", "history?before=6", session.cookie)).json<{messages: {seq: number}[]; more: boolean}>();
  expect(older.messages.map(m => m.seq)).toEqual([1, 2, 3, 4, 5]);
  expect(older.more).toBe(false);
  const page = await (await webRequest("A", "history?after=0", session.cookie)).json<{messages: {seq: number}[]; more: boolean}>();
  expect(page.messages.at(-1)?.seq).toBe(100);
  expect(page.more).toBe(true);
  const connection = await webSocket("A", session.cookie, sockets, 100);
  for (let seq = 101; seq <= 105; seq++) expect((await connection.next()).message?.seq).toBe(seq);
  const empty = await (await webRequest("A", "history?after=105", session.cookie)).json<{messages: unknown[]; more: boolean}>();
  expect(empty).toMatchObject({messages: [], more: false});
  for (const query of ["?after=-1", "?after=1.2", "?after=01", "?after=9007199254740992", "?after=0&before=6"])
    expect((await webRequest("A", "history" + query, session.cookie)).status).toBe(400);
});

it("resumes browser delivery after hibernation while leaving radio cursors untouched", async () => {
  const a = await joinWeb(), b = await joinWeb("B", 2, "Bob", "room-b");
  const connection = await webSocket("B", b.cookie, sockets);
  await webPost("A", a.cookie, "first");
  expect((await connection.next()).message?.seq).toBe(1);
  await evictDurableObject(bindings.ROOMS.getByName("shared"));
  await webPost("A", a.cookie, "second");
  expect((await connection.next()).message?.seq).toBe(2);
  connection.ws.send(JSON.stringify({op: "sync", since: 1}));
  expect((await connection.next()).message?.seq).toBe(2);
});

it("invalidates expired, revoked and password-changed sessions and supports logout", async () => {
  const session = await joinWeb();
  await runInDurableObject(bindings.ROOMS.getByName("shared"), (_room, s) =>
    s.storage.sql.exec("UPDATE web_sessions SET expires=0"));
  expect((await webRequest("A", "history", session.cookie)).status).toBe(401);
  const again = await joinWeb();
  await runInDurableObject(bindings.ROOMS.getByName("shared"), (_room, s) =>
    s.storage.sql.exec("UPDATE web_sessions SET credential='different-password'"));
  expect((await webRequest("A", "history", again.cookie)).status).toBe(401);
  const fresh = await joinWeb();
  const socket = await webSocket("A", fresh.cookie, sockets);
  const closed = new Promise<number>(resolve => socket.ws.addEventListener("close", event => resolve(event.code)));
  const logout = await webRequest("A", "logout", fresh.cookie, {});
  expect(logout.status).toBe(200);
  expect(logout.headers.get("Set-Cookie")).toContain("Max-Age=0");
  expect(await closed).toBe(1008);
  expect((await webRequest("A", "session", fresh.cookie)).status).toBe(401);
});

it("limits repeated password attempts per address without revealing whether the password matched", async () => {
  for (let i = 0; i < 10; i++)
    expect((await webRequest("A", "login", undefined, {identity: device(i), name: "Alice", password: "wrong"})).status).toBe(403);
  expect((await webRequest("A", "login", undefined, {identity: device(1), name: "Alice", password: "room"})).status).toBe(429);
  const good = await webRequest("A", "login", undefined, {identity: device(1), name: "Alice", password: "room"}, {"CF-Connecting-IP": "192.0.2.2"});
  expect(good.status).toBe(200);
  const token = await credential(good.headers.get("Set-Cookie")!.split(";")[0].split("=")[1]);
  const stored = await runInDurableObject(bindings.ROOMS.getByName("shared"), (_room, s) =>
    s.storage.sql.exec("SELECT token FROM web_sessions").one().token);
  expect(stored).toBe(token);
});
