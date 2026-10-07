import {
  SELF,
  env,
  reset,
  runInDurableObject,
  evictDurableObject,
} from "cloudflare:test";
import { afterEach, expect, it } from "vitest";
import fixture from "./native-fixtures.json";
import {
  NativeCrypto,
  base64,
  unbase64,
  fromHex,
  join,
  toHex,
} from "../src/native-crypto";
import { packet, le32 } from "../src/native";
import type { Env } from "../src/config";
import type { Transmit } from "../src/room";

const bindings = env as unknown as Env,
  sockets: WebSocket[] = [];
const crypto = new NativeCrypto();
const secret = crypto.secret(
  fromHex(fixture.reader.key),
  fromHex(fixture.room.publicKey),
)!;
afterEach(async () => {
  for (const ws of sockets.splice(0)) ws.close();
  await reset();
});
async function http(token: string, operation: unknown, alias = "A") {
  return SELF.fetch(`https://room.test/v1/aliases/${alias}/operations`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}` },
    body: JSON.stringify(operation),
  });
}
async function rf(token: string, wire: string) {
  const r = await http(token, { op: "rf", packet: wire });
  const result = await r.json<any>();
  expect(r.status, JSON.stringify(result)).toBe(200);
  return result;
}
async function connect(token: string, alias = "A") {
  const r = await SELF.fetch(`https://room.test/v1/aliases/${alias}/socket`, {
    headers: {
      Authorization: `Bearer ${token}`,
      Upgrade: "websocket",
      "Sec-WebSocket-Protocol": "aspen-room.v2.json",
    },
  });
  expect(r.status).toBe(101);
  const ws = r.webSocket!;
  sockets.push(ws);
  const inbox: any[] = [],
    waiting: Array<(e: any) => void> = [];
  ws.addEventListener("message", (e) => {
    const value = JSON.parse(e.data as string);
    if (waiting.length) waiting.shift()!(value);
    else inbox.push(value);
  });
  ws.accept();
  const next = async (label = "ready"): Promise<any> =>
    inbox.length
      ? inbox.shift()
      : new Promise((resolve, reject) => {
          const timer = setTimeout(
            () => reject(new Error(`Missing ${label} frame`)),
            1000,
          );
          waiting.push((e) => {
            clearTimeout(timer);
            resolve(e);
          });
        });
  expect(await next()).toMatchObject({
    type: "ready",
    version: 2,
    publicKey:
      alias === "A" ? fixture.room.publicKey : fixture.otherRoom.publicKey,
  });
  return { ws, inbox, next };
}
async function state() {
  return runInDurableObject(bindings.ROOMS.getByName("native"), (_room, s) => ({
    messages: s.storage.sql
      .exec("SELECT * FROM messages ORDER BY seq")
      .toArray(),
    sessions: s.storage.sql
      .exec("SELECT * FROM sessions ORDER BY client")
      .toArray(),
    pending: s.storage.sql.exec("SELECT * FROM pending").toArray(),
  }));
}
function history(tx: Transmit) {
  expect(tx.type).toBe("transmit");
  const p = packet(unbase64(tx.packet))!;
  expect(p.kind).toBe(2);
  const plain = crypto.crypt(secret, p.payload.slice(2), true)!;
  const text = plain.slice(9);
  const end = text.indexOf(0);
  const exact = plain.slice(0, 9 + (end < 0 ? text.length : end));
  const hash = crypto.sha(join(exact, fromHex(fixture.reader.publicKey)));
  return {
    text: new TextDecoder().decode(exact.slice(9)),
    timestamp: new DataView(plain.buffer, plain.byteOffset).getUint32(0, true),
    ack: base64(join(new Uint8Array([13, 0x80]), hash.slice(0, 4))),
  };
}

it("verifies native logins/posts from two frontends, pushes ordered ciphertext and restores pending state", async () => {
  const one = await connect("one"),
    two = await connect("two");
  const login = await Promise.all([
    rf("one", fixture.authorLogin),
    rf("two", fixture.authorLogin),
  ]);
  expect(login.filter((r) => r.transmission)).toHaveLength(1);
  const response = packet(
    unbase64(login.find((r) => r.transmission).transmission.packet),
  )!;
  const authorSecret = crypto.secret(
    fromHex(fixture.author.key),
    fromHex(fixture.room.publicKey),
  )!;
  const reply = crypto.crypt(authorSecret, response.payload.slice(2), true)!;
  expect(response.kind).toBe(8);
  expect(Array.from(reply.slice(0, 8))).toEqual([0x82, 1, 2, 3, 4, 5, 6, 1]);
  expect(reply[20]).toBe(1);
  await rf("two", fixture.readerLogin);
  await rf("two", fixture.path);
  const posts = await Promise.all([
    rf("one", fixture.post),
    rf("two", fixture.post),
  ]);
  expect(posts.filter((r) => r.transmission)).toHaveLength(1);
  expect(
    toHex(
      packet(unbase64(posts.find((r) => r.transmission).transmission.packet))!
        .payload,
    ),
  ).toBe("8ae1e1f3");
  expect((await state()).messages).toHaveLength(1);
  const first = await two.next("first history");
  const received = history(first);
  expect(received.text).toBe("hello");
  expect(first).not.toHaveProperty("client");
  expect(first).not.toHaveProperty("message");
  expect(packet(unbase64(first.packet))!.path).toEqual(
    new Uint8Array([6, 5, 4, 3, 2, 1]),
  );
  await rf("one", fixture.secondPost);
  const denied = await http("one", {
    op: "txReceipt",
    dispatchId: first.dispatchId,
    outcome: "sent",
  });
  expect(denied.status).toBe(403);
  await denied.text();
  const receipt = await http("two", {
    op: "txReceipt",
    dispatchId: first.dispatchId,
    outcome: "unknown",
  });
  expect(receipt.status).toBe(200);
  await receipt.text();
  expect((await state()).pending).toHaveLength(1);
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  expect((await state()).pending[0].state).toBe("unknown");
  two.ws.close();
  const reconnected = await connect("two");
  expect(reconnected.inbox).toHaveLength(0);
  const refresh = await rf("one", fixture.refresh);
  expect(packet(unbase64(refresh.transmission.packet))!.flood).toBe(true);
  const replayTx = await one.next("refresh history");
  expect(packet(unbase64(replayTx.packet))!.path).toHaveLength(0);
  const replay = history(replayTx);
  expect(replay.text).toBe("hello");
  await rf("one", replay.ack);
  const second = history(await one.next("second history"));
  expect(second.text).toBe("second");
  expect(second.timestamp).toBeGreaterThan(replay.timestamp);
  const reader = (await state()).sessions.find(
    (s) => s.client === fixture.reader.publicKey,
  )!;
  expect(reader.cursor).toBe(1);
  expect((await state()).pending).toHaveLength(1);
});

it("catches up on login and rejects decoded claims, bad crypto and unauthorized requests", async () => {
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  // Retained records for revoked aliases must not occupy all ACK variants.
  const stamp = Number((await state()).messages[0].timestamp);
  const revokedProofs = Array.from({ length: 4 }, (_, n) => {
    const body = join(
      le32(stamp),
      new Uint8Array([8 | n]),
      fromHex(fixture.author.publicKey).slice(0, 4),
      new TextEncoder().encode("hello"),
    );
    return new DataView(
      crypto.sha(join(body, fromHex(fixture.reader.publicKey))).buffer,
    )
      .getUint32(0, true)
      .toString(16)
      .padStart(8, "0");
  });
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, s) => {
    for (let n = 0; n < 4; n++)
      s.storage.sql.exec(
        "INSERT INTO pending VALUES (?, ?, ?, 1, 'two', ?, 'unknown')",
        `revoked${n}`,
        fixture.reader.publicKey,
        `old${n}`,
        revokedProofs[n],
      );
  });
  const reader = await connect("two");
  await rf("two", fixture.readerLogin);
  const first = await reader.next();
  expect(history(first).text).toBe("hello");
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, s) => {
    s.storage.sql.exec("DELETE FROM pending WHERE alias LIKE 'revoked%'");
  });
  const shared = await connect("two", "SharedB");
  const login = await http(
    "two",
    { op: "rf", packet: fixture.sharedReaderLogin },
    "SharedB",
  );
  expect(login.status).toBe(200);
  await login.text();
  await shared.next();
  const active = (await state()).pending;
  expect(active).toHaveLength(2);
  expect(new Set(active.map((p) => p.proof)).size).toBe(2);
  await rf("two", history(first).ack);
  expect((await state()).pending.map((p) => p.alias)).toEqual(["SharedB"]);
  for (const op of ["login", "post", "members", "ack", "prepare"])
    expect(
      (
        await http("one", {
          op,
          client: fixture.author.publicKey,
          proof: "12345678",
        })
      ).status,
    ).toBe(403);
  expect((await http("bad", { op: "rf", packet: fixture.post })).status).toBe(
    401,
  );
  const damaged = unbase64(fixture.post);
  damaged[damaged.length - 1] ^= 1;
  expect((await rf("one", base64(damaged))).accepted).toBe(false);
  // Even correctly MACed signed TXT format cannot become an original post.
  const reflected = unbase64(first.packet),
    offset = 2 + packet(reflected)!.path.length;
  reflected[offset] = fromHex(fixture.room.publicKey)[0];
  reflected[offset + 1] = fromHex(fixture.reader.publicKey)[0];
  expect((await rf("two", base64(reflected))).accepted).toBe(false);
  const pending = (await state()).pending[0];
  expect(pending.proof).not.toBeNull();
  const advert = await (await http("one", { op: "advertise" })).json<any>();
  const p = packet(unbase64(advert.transmission.packet))!;
  expect(p.kind).toBe(4);
  expect(
    crypto.verify(
      p.payload.slice(0, 32),
      p.payload.slice(36, 100),
      join(p.payload.slice(0, 36), p.payload.slice(100)),
    ),
  ).toBe(true);
  expect((await state()).messages).toHaveLength(1);
});
