import {expect, it} from "vitest";
import {env, SELF, runInDurableObject, evictDurableObject} from "cloudflare:test";
import type {Env} from "../src/config";
import {credential} from "../src/config";
import {auth, authenticator, proof, type AuthChallenge} from "./passkey-helpers";
import {fixture} from "./native-helpers";
import {webRequest, origin} from "./web-helpers";

const bindings = env as Env;
const authority = () => bindings.ACCOUNTS.getByName("accounts");
async function json<T>(response: Response): Promise<T> {
  expect(response.status, await response.clone().text()).toBe(200);
  return response.json<T>();
}
async function register(user = "alice", device = fixture.author) {
  const key = await authenticator();
  const challenge = await json<AuthChallenge>(await auth("register/options", {username: user, publicKey: device.publicKey,
    enrollment: (user === "alice" ? "aa" : "bb").repeat(32)}));
  const body = {...proof(challenge, device), response: await key.register(challenge)};
  const response = await auth("register/verify", body);
  await json(response);
  return {key, cookie: response.headers.get("Set-Cookie")!.split(";")[0], body};
}
async function challenge(purpose: string, device = fixture.reader) {
  return json<AuthChallenge>(await auth("device-challenge", {purpose, publicKey: device.publicKey}));
}
async function link(device = fixture.reader) {
  return json<{code: string; claim: string; expires: number; publicKey: string; url: string}>(await auth("link/start",
    {...proof(await challenge("link", device), device), label: "Second browser"}));
}
async function ticket(cookie: string, publicKey = fixture.author.publicKey, alias = "A") {
  return json<{ticket: string}>(await auth("room-ticket", {alias, publicKey}, cookie));
}

it("registers a UV passkey separately from the device key and reopens the same account after a wake", async () => {
  const listing = await (await SELF.fetch(origin + "/v1/web/rooms")).json<{loginMode: string}>();
  expect(listing.loginMode).toBe("passkey");
  const enrolled = await register();
  expect((await auth("register/verify", enrolled.body)).status).toBe(403);
  expect((await auth("register/options", {username: "alice", publicKey: fixture.reader.publicKey, enrollment: "aa".repeat(32)})).status).toBe(401);
  await evictDurableObject(authority());
  expect(await json(await auth("session", undefined, enrolled.cookie))).toMatchObject({username: "alice", publicKey: fixture.author.publicKey});
  const login = await json<AuthChallenge>(await auth("authenticate/options", {username: "alice", publicKey: fixture.reader.publicKey}));
  const response = await auth("authenticate/verify", {...proof(login, fixture.reader), response: await enrolled.key.authenticate(login)});
  expect(await json(response)).toMatchObject({username: "alice", publicKey: fixture.reader.publicKey, name: "Alice"});
  expect(await authority().deviceAllowed("bob", fixture.reader.publicKey)).toBe(false);
});

it("rejects wrong origin, missing UV, device signature forgery and registration challenge replay", async () => {
  const key = await authenticator();
  for (const mode of ["origin", "uv", "signature"]) {
    const c = await json<AuthChallenge>(await auth("register/options", {username: "alice", publicKey: fixture.author.publicKey, enrollment: "aa".repeat(32)}));
    const body = {...proof(c), response: await key.register(c, mode !== "uv", mode === "origin" ? "https://other.test" : origin)};
    if (mode === "signature") body.signature = "00".repeat(64);
    expect((await auth("register/verify", body)).status).toBe(403);
    expect((await auth("register/verify", body)).status).toBe(403);
  }
  expect((await auth("device-challenge", {purpose: "link", publicKey: fixture.reader.publicKey}, undefined, "https://other.test")).status).toBe(403);
  expect((await auth("register/options", {username: "alice", publicKey: fixture.author.publicKey, enrollment: "ee".repeat(32)})).status).toBe(401);
});

it("checks assertion UV, exact origin and counter, and consumes failed assertions", async () => {
  const enrolled = await register();
  for (const mode of ["uv", "origin", "counter"]) {
    const c = await json<AuthChallenge>(await auth("authenticate/options", {username: "alice", publicKey: fixture.author.publicKey}));
    const body = {...proof(c), response: await enrolled.key.authenticate(c, mode === "counter" ? 1 : 2, mode !== "uv",
      mode === "origin" ? "https://other.test" : origin)};
    if (mode === "counter") {
      await runInDurableObject(authority(), (_object, state) =>
        state.storage.sql.exec("UPDATE account_records SET data=json_set(data,'$.counter',2) WHERE kind='passkey'"));
    }
    expect((await auth("authenticate/verify", body)).status).toBe(403);
    expect((await auth("authenticate/verify", body)).status).toBe(403);
  }
});

it("issues one-use room-scoped tickets, disables password login and preserves radio credentials", async () => {
  const enrolled = await register(), first = await ticket(enrolled.cookie);
  const response = await webRequest("A", "login", undefined, first);
  expect(await json(response.clone())).toMatchObject({author: fixture.author.publicKey, username: "alice", name: "Alice"});
  expect((await webRequest("A", "login", undefined, first)).status).toBe(401);
  const wrong = await ticket(enrolled.cookie);
  expect((await webRequest("SharedB", "login", undefined, wrong)).status).toBe(401);
  expect((await webRequest("A", "login", undefined, {password: "fixture password only", username: "alice"})).status).toBe(400);
  expect((await webRequest("A", "challenge", undefined, {publicKey: fixture.author.publicKey, username: "alice"})).status).toBe(403);
  const native = await SELF.fetch(origin + "/v1/aliases/A/operations", {method: "POST", headers: {Authorization: "Bearer one",
    "Content-Type": "application/json"}, body: JSON.stringify({operations: []})});
  expect(native.status).not.toBe(401);
});

it("links distinct device keys only after signed-in inspection and approval, then claims once", async () => {
  const enrolled = await register(), request = await link();
  expect(request.expires - Math.floor(Date.now() / 1000)).toBeGreaterThanOrEqual(299);
  expect(request.expires - Math.floor(Date.now() / 1000)).toBeLessThanOrEqual(300);
  expect(request.url).toBe(origin + "/#link=" + request.code);
  expect((await auth("link/claim", request)).status).toBe(403);
  expect((await auth("link/approve", request)).status).toBe(401);
  const inspected = await json(await auth("link/inspect", {code: request.code}, enrolled.cookie));
  expect(inspected).toMatchObject({publicKey: fixture.reader.publicKey, label: "Second browser"});
  expect(JSON.stringify(inspected)).not.toContain(request.claim);
  expect((await auth("link/approve", {code: request.code, publicKey: fixture.author.publicKey}, enrolled.cookie)).status).toBe(409);
  expect(await json(await auth("link/approve", {code: request.code, publicKey: request.publicKey}, enrolled.cookie))).toMatchObject({approved: true});
  await evictDurableObject(authority());
  expect(await json(await auth("link/status", request))).toMatchObject({approved: true});
  const claimed = await auth("link/claim", request), session = await json(claimed.clone());
  expect(session).toMatchObject({username: "alice", publicKey: fixture.reader.publicKey});
  expect((await auth("link/claim", request)).status).toBe(403);
  const secondCookie = claimed.headers.get("Set-Cookie")!.split(";")[0];
  const room = await ticket(secondCookie, fixture.reader.publicKey, "Independent");
  expect(await json(await webRequest("Independent", "login", undefined, room))).toMatchObject({author: fixture.reader.publicKey, username: "alice"});
  const deviceLogin = await json(await auth("device-login", proof(await challenge("device-login"), fixture.reader)));
  expect(deviceLogin).toMatchObject({username: "alice", publicKey: fixture.reader.publicKey});
});

it("rejects expired five-minute links, bad claims, self-linking and cross-account device keys", async () => {
  const first = await register(), bob = await register("bob", fixture.reader);
  const request = await link();
  expect((await auth("link/inspect", {code: request.code}, first.cookie)).status).toBe(409);
  expect((await auth("link/inspect", {code: request.code}, bob.cookie)).status).toBe(409);
  const self = await link(fixture.author);
  expect((await auth("link/inspect", {code: self.code}, first.cookie)).status).toBe(409);
  expect((await auth("link/status", {...self, claim: "00".repeat(32)})).status).toBe(403);
  await runInDurableObject(authority(), (_object, state) =>
    state.storage.sql.exec("UPDATE account_records SET expires=0,data=json_set(data,'$.expires',0) WHERE kind='link'"));
  expect((await auth("link/approve", {code: request.code, publicKey: request.publicKey}, first.cookie)).status).toBe(403);
});

it("revokes account and room sessions without deleting device keys or allowing account reassignment", async () => {
  const enrolled = await register(), body = await ticket(enrolled.cookie);
  const roomResponse = await webRequest("A", "login", undefined, body);
  await json(roomResponse.clone());
  const roomCookie = roomResponse.headers.get("Set-Cookie")!.split(";")[0];
  expect((await auth("device/revoke", {publicKey: fixture.author.publicKey}, enrolled.cookie)).status).toBe(200);
  expect((await webRequest("A", "session", roomCookie)).status).toBe(401);
  expect((await auth("session", undefined, enrolled.cookie)).status).toBe(401);
  expect(await authority().deviceAllowed("bob", fixture.author.publicKey)).toBe(false);
});

it("cancels an unclaimed approval when its approving device is revoked", async () => {
  const enrolled = await register(), request = await link();
  await json(await auth("link/approve", {code: request.code, publicKey: request.publicKey}, enrolled.cookie));
  await json(await auth("device/revoke", {publicKey: fixture.author.publicKey}, enrolled.cookie));
  expect((await auth("link/claim", request)).status).toBe(403);
  expect((await auth("device-login", proof(await challenge("device-login"), fixture.reader))).status).toBe(401);
});

it("adds another UV passkey only from the same approved device account", async () => {
  const enrolled = await register(), second = await authenticator();
  expect((await auth("register/options", {username: "alice", publicKey: fixture.reader.publicKey}, enrolled.cookie)).status).toBe(403);
  const challenge = await json<AuthChallenge>(await auth("register/options", {username: "alice", publicKey: fixture.author.publicKey}, enrolled.cookie));
  const response = await auth("register/verify", {...proof(challenge), response: await second.register(challenge)});
  await json(response);
  const options = await json<{options: {allowCredentials: {id: string}[]}}>(await auth("authenticate/options",
    {username: "alice", publicKey: fixture.reader.publicKey}));
  expect(options.options.allowCredentials.map(c => c.id).sort()).toEqual([enrolled.key.id, second.id].sort());
});

it("takes enrollment tokens once under concurrent requests without registering the losing device", async () => {
  const key = await authenticator(), other = await authenticator();
  const first = await json<AuthChallenge>(await auth("register/options", {username: "alice", publicKey: fixture.author.publicKey, enrollment: "aa".repeat(32)}));
  const second = await json<AuthChallenge>(await auth("register/options", {username: "alice", publicKey: fixture.reader.publicKey, enrollment: "aa".repeat(32)}));
  const responses = await Promise.all([
    auth("register/verify", {...proof(first), response: await key.register(first)}),
    auth("register/verify", {...proof(second, fixture.reader), response: await other.register(second)}),
  ]);
  expect(responses.map(r => r.status).sort()).toEqual([200, 403]);
  for (let i = 0; i < responses.length; i++) {
    await responses[i].text();
    if (responses[i].status === 403) {
      const publicKey = i === 0 ? fixture.author.publicKey : fixture.reader.publicKey;
      await runInDurableObject(authority(), (_object, state) =>
        expect(state.storage.sql.exec("SELECT id FROM account_records WHERE kind='device' AND id=?", publicKey).toArray()).toHaveLength(0));
    }
  }
  expect(await credential("aa".repeat(32))).not.toBe("aa".repeat(32));
});
