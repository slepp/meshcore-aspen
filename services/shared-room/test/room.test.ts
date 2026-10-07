import {SELF, env, reset, runInDurableObject, evictDurableObject} from "cloudflare:test";
import {afterEach, expect, it} from "vitest";
import type {Env} from "../src/config";
import type {Delivery, Operation, ServerEvent} from "../src/protocol";
import {Frontend, type RadioCodec, type Socket} from "../examples/frontend";
import worker from "../src/index";

const bindings = env as unknown as Env;
const client = (n: number) => n.toString(16).padStart(64, "0");
const attempt = client;
const sockets: WebSocket[] = [];
let requestId = 0;
const login = (who: string, n: number, password = "room", since = 0): Operation =>
  ({op: "login", client: who, timestamp: n, since, password, attempt: attempt(n), route: btoa("direct:abcd")});
const post = (who: string, n: number, text: string, rf = n): Operation =>
  ({op: "post", client: who, timestamp: n, text, source: "client", attempt: attempt(rf)});
async function http(alias: string, token: string, operation: unknown) {
  return SELF.fetch(`https://room.test/v1/aliases/${alias}/operations`, {
    method: "POST", headers: {Authorization: `Bearer ${token}`}, body: JSON.stringify(operation),
  });
}
async function ok(alias: string, token: string, operation: unknown) {
  const response = await http(alias, token, operation);
  const result = await response.json<any>();
  expect(response.status, JSON.stringify(result)).toBe(200);
  return result;
}
async function connect(alias: string, token: string) {
  const response = await SELF.fetch(`https://room.test/v1/aliases/${alias}/socket`, {
    headers: {Authorization: `Bearer ${token}`, Upgrade: "websocket"},
  });
  expect(response.status).toBe(101);
  const ws = response.webSocket!;
  sockets.push(ws);
  const inbox: ServerEvent[] = [];
  const waiting: Array<{match: (e: ServerEvent) => boolean; resolve: (e: any) => void}> = [];
  ws.addEventListener("message", event => {
    const value = JSON.parse(event.data as string) as ServerEvent;
    const i = waiting.findIndex(w => w.match(value));
    if (i >= 0) waiting.splice(i, 1)[0].resolve(value);
    else inbox.push(value);
  });
  ws.accept();
  const next = (match: (e: ServerEvent) => boolean): Promise<any> => {
    const i = inbox.findIndex(match);
    if (i >= 0) return Promise.resolve(inbox.splice(i, 1)[0]);
    return new Promise(resolve => waiting.push({match, resolve}));
  };
  await next(e => e.type === "ready");
  return {
    ws, inbox,
    delivery: () => next(e => e.type === "delivery") as Promise<Delivery>,
    async rpc(operation: Operation) {
      const id = String(++requestId);
      const promise = next(e => (e.type === "result" || e.type === "error") && e.id === id);
      ws.send(JSON.stringify({id, operation}));
      return promise;
    },
  };
}
async function count(backend: string) {
  return runInDurableObject(bindings.ROOMS.getByName(backend), (_room, state) =>
    state.storage.sql.exec("SELECT COUNT(*) AS n FROM messages").one().n);
}
async function acknowledge(alias: string, token: string, delivery: Delivery, proof = "01020304") {
  expect((await ok(alias, token, {op: "prepare", client: delivery.client, deliveryId: delivery.deliveryId, proof})).transmit).toBe(true);
  return ok(alias, token, {op: "ack", client: delivery.client, deliveryId: delivery.deliveryId, proof});
}
afterEach(async () => {for (const ws of sockets.splice(0)) ws.close(); await reset();});

it("rejects requests during placeholder removal and resumes with private bindings without losing room state", async () => {
  const who = client(99);
  await ok("A", "local-token", login(who, 1));
  await ok("A", "local-token", post(who, 2, "retained through configuration"));
  const missing: Env = {...bindings, ALIASES: undefined, FRONTENDS: undefined, ROOM_KEYS: undefined};
  const placeholders: Env = {...missing, ALIASES: "{}", FRONTENDS: "{}"};
  for (const unconfigured of [placeholders, missing]) {
    for (const headers of [{}, {Authorization: "Bearer local-token"},
      {Authorization: "Bearer invalid"}, {Upgrade: "websocket", Authorization: "Bearer local-token"}]) {
      const response = await worker.fetch(new Request("https://room.test/v1/aliases/A/operations", {headers}), unconfigured);
      expect(response.status).toBe(401);
    }
  }
  const restored = await worker.fetch(new Request("https://room.test/v1/aliases/A/operations", {
    method: "POST", headers: {Authorization: "Bearer local-token"}, body: JSON.stringify({op: "members"}),
  }), bindings);
  expect(restored.status).toBe(200);
  expect((await restored.json<any>()).members.some((entry: any) => entry.client === who)).toBe(true);
  expect(await count("shared")).toBe(1);
});

it("elects one frontend for duplicate logins/posts and commits one logical message before success", async () => {
  const one = await connect("A", "local-token");
  const two = await connect("A", "remote-token");
  const reader = client(1), author = client(2);
  const logged = await Promise.all([ok("A", "local-token", login(reader, 1)), ok("A", "remote-token", login(reader, 1))]);
  expect(logged.filter(r => r.respond)).toHaveLength(1);
  await ok("A", "local-token", login(author, 2));
  const posted = await Promise.all([ok("A", "local-token", post(author, 3, "hello")), ok("A", "remote-token", post(author, 3, "hello"))]);
  expect(posted.filter(r => r.respond)).toHaveLength(1);
  expect(posted[0].message.seq).toBe(posted[1].message.seq);
  expect(await count("shared")).toBe(1);
  const chosen = logged[0].respond ? one : two;
  const other = logged[0].respond ? two : one;
  expect((await chosen.delivery()).message.text).toBe("hello");
  expect(other.inbox.filter(e => e.type === "delivery")).toHaveLength(0);
  const retry = await ok("A", "remote-token", post(author, 3, "hello", 4));
  expect(retry).toMatchObject({respond: true, duplicate: true});
  expect(await count("shared")).toBe(1);
});

it("keeps one pending delivery, rejects wrong ACKs, and recovers on a new client request", async () => {
  const one = await connect("A", "local-token");
  const two = await connect("A", "remote-token");
  const reader = client(1), author = client(2);
  await ok("A", "local-token", login(reader, 1));
  await ok("A", "local-token", login(author, 2));
  await ok("A", "local-token", post(author, 3, "first"));
  const first = await one.delivery();
  await ok("A", "local-token", post(author, 4, "second"));
  const prepare = {op: "prepare", client: reader, deliveryId: first.deliveryId, proof: "11223344"};
  expect((await ok("A", "local-token", prepare)).transmit).toBe(true);
  expect((await ok("A", "local-token", prepare)).transmit).toBe(false);
  await ok("A", "local-token", {op: "receipt", client: reader, deliveryId: first.deliveryId, outcome: "unknown"});
  expect((await http("A", "local-token", {op: "ack", client: reader, deliveryId: first.deliveryId, proof: "deadbeef"})).status).toBe(403);
  expect((await http("A", "remote-token", prepare)).status).toBe(403);
  await ok("A", "remote-token", login(reader, 1));
  expect(two.inbox.filter(e => e.type === "delivery")).toHaveLength(0);
  await ok("A", "remote-token", {op: "refresh", client: reader, timestamp: 5, attempt: attempt(5), route: btoa("direct:ef01")});
  const recovered = await two.delivery();
  expect(recovered.message.seq).toBe(first.message.seq);
  expect(recovered.deliveryId).not.toBe(first.deliveryId);
  expect((await http("A", "local-token", {op: "ack", client: reader, deliveryId: first.deliveryId, proof: "11223344"})).status).toBe(409);
  await acknowledge("A", "remote-token", recovered);
  const second = await two.delivery();
  expect(second.message.seq).toBeGreaterThan(first.message.seq);
  expect(second.message.timestamp).toBeGreaterThan(first.message.timestamp);
  expect(second.message.text).toBe("second");
  const members = await ok("A", "remote-token", {op: "members"});
  expect(members.members.find((m: any) => m.client === reader).cursor).toBe(first.message.seq);
});

it("restores ordering and attachment authorization after hibernation and reconnect", async () => {
  const socket = await connect("A", "local-token");
  const reader = client(1), author = client(2);
  await ok("A", "local-token", login(reader, 1));
  await ok("A", "local-token", login(author, 2));
  await ok("A", "local-token", post(author, 3, "first"));
  const first = await socket.delivery();
  await ok("A", "local-token", {op: "prepare", client: reader, deliveryId: first.deliveryId, proof: "11111111"});
  await ok("A", "local-token", post(author, 4, "second"));
  await evictDurableObject(bindings.ROOMS.getByName("shared"));
  const resumed = await socket.rpc({op: "members"});
  expect(resumed.result.members.find((m: any) => m.client === reader).pending.proof).toBe("11111111");
  await socket.rpc({op: "ack", client: reader, deliveryId: first.deliveryId, proof: "11111111"});
  const second = await socket.delivery();
  expect(second.message.text).toBe("second");
  socket.ws.close();
  const reconnected = await connect("A", "local-token");
  const status = await reconnected.rpc({op: "members"});
  expect(status.result.members.find((m: any) => m.client === reader).cursor).toBe(first.message.seq);
  expect(reconnected.inbox.filter(e => e.type === "delivery")).toHaveLength(0);
  // A forged future since never skips the pending, unconfirmed post.
  await ok("A", "local-token", login(reader, 6, "room", 0xffffffff));
  expect((await reconnected.delivery()).message.seq).toBe(second.message.seq);
});

it("shares ordered content across distinct aliases and isolates a separate backend", async () => {
  const a = await connect("A", "local-token"), b = await connect("B", "local-token");
  const c = await connect("C", "local-token"), yeg = await connect("YEG", "remote-token");
  const yyc = await connect("YYC", "remote-token");
  const who = client(1);
  await ok("A", "local-token", login(who, 1));
  await ok("B", "local-token", login(who, 1, "room-b"));
  await ok("C", "local-token", login(who, 1, "private"));
  await ok("YEG", "remote-token", login(client(2), 1));
  await ok("YYC", "remote-token", login(client(3), 1));
  await ok("A", "local-token", post(who, 2, "from A"));
  const [local, regional, remote] = await Promise.all([b.delivery(), yeg.delivery(), yyc.delivery()]);
  expect(local.message).toMatchObject({originAlias: "A", author: who, text: "from A"});
  expect(regional.message.seq).toBe(local.message.seq);
  expect(remote.message.seq).toBe(local.message.seq);
  expect((await http("A", "local-token", {op: "ack", client: who, deliveryId: local.deliveryId, proof: "01020304"})).status).toBe(409);
  expect(c.inbox.filter(e => e.type === "delivery")).toHaveLength(0);
  expect(await count("separate")).toBe(0);
  await ok("YYC", "remote-token", post(client(3), 3, "from YYC"));
  expect((await a.delivery()).message).toMatchObject({originAlias: "YYC", text: "from YYC"});
  expect((await http("A", "local-token", {...post(who, 4, "remap"), backend: "separate"})).status).toBe(400);
  await acknowledge("B", "local-token", local);
  expect((await b.delivery()).message.text).toBe("from YYC");
  expect(await count("shared")).toBe(2);
  expect(await count("separate")).toBe(0);
});

it("rejects unauthorized frontends, wrong alias membership, passwords and relayed posts", async () => {
  expect((await http("A", "bad-token", {op: "members"})).status).toBe(401);
  expect((await http("YEG", "local-token", {op: "members"})).status).toBe(403);
  expect((await http("A", "local-token", login(client(1), 1, "wrong"))).status).toBe(403);
  expect((await http("A", "local-token", post(client(1), 2, "hello"))).status).toBe(403);
  await ok("A", "local-token", login(client(1), 1));
  expect((await http("B", "local-token", post(client(1), 2, "wrong identity"))).status).toBe(403);
  expect((await http("A", "local-token", {...post(client(1), 2, "relay"), source: "room"})).status).toBe(400);
  expect((await http("A", "local-token", post(client(1), 2, "\ud800"))).status).toBe(400);
  // Full-key candidates survive a short-prefix collision.
  await ok("A", "local-token", login(client(2), 1));
  expect((await ok("A", "local-token", {op: "members", prefix: "00"})).members).toHaveLength(2);
});

it("runs a thin physical frontend with A/B/C, shared remote history and rotating advertisements", async () => {
  const identities = [{alias: "A", publicKey: "11".repeat(32), name: "A"},
    {alias: "B", publicKey: "11" + "22".repeat(31), name: "B"},
    {alias: "C", publicKey: "55".repeat(32), name: "C"}];
  const sent: any[] = [], errors: unknown[] = [];
  const waiters: Array<{test: (packet: any) => boolean; resolve: (packet: any) => void}> = [];
  const waitFor = (test: (p: any) => boolean) => {
    const found = sent.find(test);
    return found ? Promise.resolve(found) : new Promise<any>(resolve => waiters.push({test, resolve}));
  };
  const encode = (value: unknown) => new TextEncoder().encode(JSON.stringify(value));
  // A decoded fixture exercises the service/adapter; it makes no raw crypto claim.
  const codec: RadioCodec = {
    async decode(packet) {return JSON.parse(new TextDecoder().decode(packet));},
    async response() {return null;},
    async delivery(identity, delivery) {return {packet: encode({alias: identity.alias, delivery}), proof: "12121212"};},
    async advertisement(identity) {return encode({advert: identity.alias});},
  };
  const frontend = new Frontend("https://room.test", "local-token", identities, {
    async submitWithReceipt(packet) {
      const value = JSON.parse(new TextDecoder().decode(packet)); sent.push(value);
      for (const waiter of [...waiters]) if (waiter.test(value)) {waiters.splice(waiters.indexOf(waiter), 1); waiter.resolve(value);}
      return "sent";
    },
  }, codec, async (url, token): Promise<Socket> => {
    const response = await SELF.fetch(url.replace(/^ws/, "http"), {headers: {Authorization: `Bearer ${token}`, Upgrade: "websocket"}});
    const ws = response.webSocket!; sockets.push(ws);
    let listener: ((frame: string) => void) | undefined;
    const buffered: string[] = [];
    ws.addEventListener("message", event => {const frame = event.data as string; if (listener) listener(frame); else buffered.push(frame);});
    ws.accept();
    return {send: frame => ws.send(frame), close: () => ws.close(),
      onMessage(handler) {listener = handler; for (const frame of buffered.splice(0)) handler(frame);},
      onClose(handler) {ws.addEventListener("close", handler);}};
  }, error => errors.push(error));
  await frontend.connect();
  for (const [alias, password] of [["A", "room"], ["B", "room-b"], ["C", "private"]]) {
    await frontend.receive(encode({alias, operation: login(client(1), 1, password)}));
  }
  const regional = await connect("YEG", "remote-token");
  await ok("YEG", "remote-token", login(client(2), 1));
  await frontend.receive(encode({alias: "A", operation: post(client(1), 2, "shared") }));
  expect((await regional.delivery()).message.originAlias).toBe("A");
  expect((await waitFor(p => p.alias === "B")).delivery.message.text).toBe("shared");
  expect(sent.filter(p => p.alias === "C")).toHaveLength(0);
  for (const alias of ["A", "B", "C"]) {frontend.advertNext(); await waitFor(p => p.advert === alias);}
  expect(sent.filter(p => p.advert).map(p => p.advert)).toEqual(["A", "B", "C"]);
  expect(errors).toHaveLength(0);
});

it("reconnect releases an unanswered prepare so the shared physical TX queue can continue", async () => {
  let opened = 0;
  let oldMessage: ((frame: string) => void) | undefined;
  let oldClose: (() => void) | undefined;
  let releasePrepare!: () => void;
  const preparing = new Promise<void>(resolve => {releasePrepare = resolve;});
  let advertSent!: () => void;
  const advertisement = new Promise<void>(resolve => {advertSent = resolve;});
  const errors: unknown[] = [];
  const codec: RadioCodec = {
    async decode() {return null;}, async response() {return null;},
    async delivery() {return {packet: new Uint8Array([1]), proof: "12345678"};},
    async advertisement() {return new Uint8Array([2]);},
  };
  const frontend = new Frontend("https://room.test", "token", [{alias: "A", publicKey: "11".repeat(32), name: "A"}], {
    async submitWithReceipt(packet) {if (packet[0] === 2) advertSent(); return "sent";},
  }, codec, async (): Promise<Socket> => {
    const generation = ++opened;
    let message!: (frame: string) => void;
    let close!: () => void;
    return {
      send(frame) {
        const {id, operation} = JSON.parse(frame);
        if (operation.op === "members") message(JSON.stringify({type: "result", id, result: {members: []}}));
        if (operation.op === "prepare" && generation === 1) releasePrepare();
      },
      close() {oldClose = close;},
      onMessage(handler) {message = handler; if (generation === 1) oldMessage = handler;},
      onClose(handler) {close = handler;},
    };
  }, error => errors.push(error));
  await frontend.connect();
  oldMessage!(JSON.stringify({type: "delivery", alias: "A", client: client(1), deliveryId: "old", route: "",
    message: {seq: 1, timestamp: 1, originAlias: "A", author: client(2), clientTimestamp: 1, text: "test"}}));
  await preparing;
  await frontend.connectAlias("A");
  oldClose!(); // The old socket's delayed close must not affect its replacement.
  frontend.advertNext();
  await advertisement;
  expect(errors.map(String)).toEqual([expect.stringContaining("operation outcome may be unknown")]);
});

it("applies native PATH ACK and learned route atomically, and keeps blank-password ACL alias scoped", async () => {
  const socket = await connect("A", "local-token");
  const reader = client(1), author = client(2);
  await ok("A", "local-token", login(reader, 1));
  await ok("A", "local-token", login(author, 2));
  await ok("A", "local-token", post(author, 3, "first"));
  const first = await socket.delivery();
  await ok("A", "local-token", {op: "prepare", client: reader, deliveryId: first.deliveryId, proof: "11223344"});
  await ok("A", "local-token", post(author, 4, "second"));
  const path = {op: "path", client: reader, attempt: attempt(5), route: btoa("learned route"), deliveryId: first.deliveryId, proof: "11223344"};
  expect((await ok("A", "remote-token", path)).respond).toBe(false);
  expect((await http("A", "local-token", {...path, proof: "deadbeef"})).status).toBe(403);
  await ok("A", "local-token", path); // Failed proof rolled back route and attempt claim.
  const second = await socket.delivery();
  expect(second.route).toBe(path.route);
  expect(second.message.text).toBe("second");
  expect((await ok("A", "local-token", {...login(reader, 0, ""), attempt: attempt(6)})).respond).toBe(true);
  expect((await http("B", "local-token", login(reader, 0, ""))).status).toBe(403);
  const acl = (await ok("A", "local-token", {op: "members", prefix: reader})).members[0];
  expect(acl.pending.deliveryId).toBe(second.deliveryId);
  expect(acl.cursor).toBe(first.message.seq);
});

it("negotiates JSON v1 and keeps member replies bounded with arbitrary route bytes", async () => {
  const response = await SELF.fetch("https://room.test/v1/aliases/A/socket", {headers: {
    Authorization: "Bearer local-token", Upgrade: "websocket", "Sec-WebSocket-Protocol": "aspen-room.v1.json",
  }});
  expect(response.status).toBe(101);
  expect(response.headers.get("Sec-WebSocket-Protocol")).toBe("aspen-room.v1.json");
  const ws = response.webSocket!; sockets.push(ws); ws.accept();
  const binary = String.fromCharCode(...Array.from({length: 255}, (_, i) => i + 1));
  const route = btoa(binary);
  for (let i = 0; i < 12; i++) {
    const key = i.toString(16).padStart(2, "0") + "ab".repeat(31);
    await ok("A", "local-token", {...login(key, 1), route});
  }
  expect((await http("A", "local-token", {op: "members"})).status).toBe(413);
  const result = await ok("A", "local-token", {op: "members", prefix: "00"});
  expect(atob(result.members[0].route)).toBe(binary);
  expect((await http("A", "local-token", {...login(client(20), 1), route: "AB=="})).status).toBe(400);
  expect((await SELF.fetch("https://room.test/v1/aliases/A/socket", {headers: {
    Authorization: "Bearer local-token", Upgrade: "websocket", "Sec-WebSocket-Protocol": "other.v2",
  }})).status).toBe(426);
});
