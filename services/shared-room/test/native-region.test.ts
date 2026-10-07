import {expect, it} from "vitest";
import {evictDurableObject} from "cloudflare:test";
import {bindings, fixture, rf, http, connect, state, history} from "./native-helpers";
import {base64, unbase64, join} from "../src/native-crypto";
import {packet} from "../src/native";
import type {Transmit} from "../src/room";

// Independent WebCrypto reference for the public regional transport HMAC.
async function code(name: string, kind: number, payload: Uint8Array) {
  const bytes = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`#${name}`))).slice(0, 16);
  const key = await crypto.subtle.importKey("raw", bytes, {name: "HMAC", hash: "SHA-256"}, false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, join(new Uint8Array([kind]), payload));
  const n = new DataView(mac).getUint16(0, true);
  return n === 0 ? 1 : n === 65535 ? 65534 : n;
}
async function scoped(wire: string, name = "ab", route = 0) {
  const p = packet(unbase64(wire))!, codes = new Uint8Array(4);
  new DataView(codes.buffer).setUint16(0, await code(name, p.kind, p.payload), true);
  return base64(join(new Uint8Array([(p.kind << 2) | route]), codes, new Uint8Array([p.length]), p.path, p.payload));
}
async function expectScope(tx: Transmit, name = "ab") {
  const raw = unbase64(tx.packet), p = packet(raw)!;
  expect(raw[0] & 3).toBe(0);
  expect(p.transportCode).toBe(await code(name, p.kind, p.payload));
  expect(new DataView(raw.buffer).getUint16(3, true)).toBe(0);
}

it("authenticates scoped flood/direct packets and preserves scope on login, catch-up and reconnect", async () => {
  const one = await connect("one"), two = await connect("two");
  const replies = await Promise.all([rf("one", fixture.scopedAuthorLogin), rf("two", fixture.scopedAuthorLogin)]);
  expect(replies.filter(r => r.transmission)).toHaveLength(1);
  await expectScope(replies.find(r => r.transmission).transmission);
  expect(unbase64(fixture.scopedPost)[0] & 3).toBe(3);
  const posts = await Promise.all([rf("one", fixture.scopedPost), rf("two", fixture.scopedPost)]);
  expect(posts.filter(r => r.transmission)).toHaveLength(1);
  expect((await state()).messages).toHaveLength(1);
  await rf("two", fixture.scopedReaderLogin);
  const first = await two.next("scoped catch-up");
  await expectScope(first);
  const received = history(first);
  expect(received.text).toBe("hello");
  // Native companions send their PATH+ACK using their configured public scope.
  await rf("two", await scoped(received.pathAck));
  expect((await state()).pending).toHaveLength(0);
  await rf("two", fixture.scopedPath);
  // Ordinary native direct TXT/ACK frames remain compatible after learning PATH.
  await rf("one", fixture.secondPost);
  const direct = await two.next("direct history");
  expect(unbase64(direct.packet)[0] & 3).toBe(2);
  expect(history(direct).text).toBe("second");
  const ack = unbase64(history(direct).ack); ack[0] = (ack[0] & ~3) | 2;
  await rf("two", base64(ack));
  expect((await state()).pending).toHaveLength(0);
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  two.ws.close();
  const reconnected = await connect("two");
  expect(reconnected.inbox).toHaveLength(0);
  await rf("one", fixture.scopedRefresh);
  const replay = await one.next("refreshed history");
  // Fresh frontend ownership has no learned route: scope is restored from config.
  await expectScope(replay);
  expect(history(replay).text).toBe("hello");
  const advert = await (await http("one", {op:"advertise"})).json<any>();
  await expectScope(advert.transmission);
  expect(one.inbox).toHaveLength(0);
});

it("rejects unknown, wrong-case, unscoped and corrupted scopes without a fallback", async () => {
  for (const token of ["edm", "upper", "plain"])
    expect((await rf(token, fixture.scopedAuthorLogin)).accepted).toBe(false);
  expect((await rf("one", fixture.authorLogin)).accepted).toBe(false);
  const damaged = unbase64(fixture.scopedAuthorLogin); damaged[1] ^= 1;
  expect((await rf("one", base64(damaged))).accepted).toBe(false);
  const privateConfig = await http("private", {op:"rf",packet:fixture.scopedAuthorLogin});
  expect(privateConfig.status).toBe(503); await privateConfig.text();
  expect((await state()).messages).toHaveLength(0);
  expect((await state()).sessions).toHaveLength(0);
  // Region ancestry and the secondary code do not grant admission to another region.
  const can = await scoped(fixture.authorLogin, "can");
  const p = unbase64(can); p.set(unbase64(fixture.scopedAuthorLogin).slice(1,3), 3);
  expect((await rf("one", base64(p))).accepted).toBe(false);
});
