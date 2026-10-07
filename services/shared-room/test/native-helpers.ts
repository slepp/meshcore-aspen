import {
  SELF,
  env,
  reset,
  runInDurableObject,
  evictDurableObject,
} from "cloudflare:test";
import { afterEach, expect, vi } from "vitest";
import fixture from "./native-fixtures.json";
export {fixture};
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

export const bindings = env as unknown as Env,
  sockets: WebSocket[] = [];
export const crypto = new NativeCrypto();
afterEach(async () => {
  vi.restoreAllMocks();
  for (const ws of sockets.splice(0)) ws.close();
  await reset();
});
export async function http(token: string, operation: unknown, alias = "A") {
  return SELF.fetch(`https://room.test/v1/aliases/${alias}/operations`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}` },
    body: JSON.stringify(operation),
  });
}
export async function rf(token: string, wire: string, alias = "A") {
  const r = await http(token, { op: "rf", packet: wire }, alias);
  const result = await r.json<any>();
  expect(r.status, JSON.stringify(result)).toBe(200);
  return result;
}
export async function connect(token: string, alias = "A") {
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
      alias === "A" ? fixture.room.publicKey : alias === "B" ? fixture.thirdRoom.publicKey : fixture.otherRoom.publicKey,
  });
  return { ws, inbox, next };
}
export async function state(backend = "native") {
  return runInDurableObject(bindings.ROOMS.getByName(backend), (_room, s) => ({
    messages: s.storage.sql
      .exec("SELECT * FROM messages ORDER BY seq")
      .toArray(),
    sessions: s.storage.sql
      .exec("SELECT * FROM sessions ORDER BY client")
      .toArray(),
    pending: s.storage.sql.exec("SELECT * FROM pending").toArray(),
  }));
}
export function history(tx: Transmit, room = fixture.room) {
  expect(tx.type).toBe("transmit");
  expect(tx.delayMs).toBe(1500);
  const p = packet(unbase64(tx.packet))!;
  expect(p.kind).toBe(2);
  const roomSecret = crypto.secret(fromHex(fixture.reader.key), fromHex(room.publicKey))!;
  const plain = crypto.crypt(roomSecret, p.payload.slice(2), true)!;
  const text = plain.slice(9);
  const end = text.indexOf(0);
  const exact = plain.slice(0, 9 + (end < 0 ? text.length : end));
  const hash = crypto.sha(join(exact, fromHex(fixture.reader.publicKey)));
  return {
    text: new TextDecoder().decode(exact.slice(9)),
    timestamp: new DataView(plain.buffer, plain.byteOffset).getUint32(0, true),
    ack: base64(join(new Uint8Array([13, 0x80]), hash.slice(0, 4))),
    pathAck: seal(room, fixture.reader, 8, join(new Uint8Array([0x80, 3]), hash.slice(0, 4))),
  };
}
export function seal(room: typeof fixture.room, peer: typeof fixture.reader, kind: number, plain: Uint8Array) {
  const roomPub = fromHex(room.publicKey), peerPub = fromHex(peer.publicKey);
  const sealed = crypto.crypt(crypto.secret(fromHex(peer.key), roomPub)!, plain)!;
  return base64(join(new Uint8Array([(kind << 2) | (kind === 7 ? 1 : 2), 0x80, roomPub[0]]),
    kind === 7 ? peerPub : peerPub.slice(0, 1), sealed));
}
