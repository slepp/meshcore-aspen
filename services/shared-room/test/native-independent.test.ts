import { expect, it, vi } from "vitest";
import { evictDurableObject, runDurableObjectAlarm, runInDurableObject } from "cloudflare:test";
import { bindings, fixture, rf, connect, state, history, seal } from "./native-helpers";
import { base64, unbase64, join } from "../src/native-crypto";
import { packet, le32 } from "../src/native";

it("uses authenticated PATH ACKs for identical simultaneous history from independent backends", async () => {
  vi.spyOn(Date, "now").mockReturnValue(1800000000000);
  vi.spyOn(globalThis.crypto, "getRandomValues").mockImplementation((array: any) => {
    array.fill(0); return array;
  });
  const a = await connect("aonly"), b = await connect("bonly", "B");
  const login = join(le32(1699999900), le32(0), new TextEncoder().encode("private\0"));
  await rf("aonly", fixture.authorLogin);
  await rf("bonly", seal(fixture.thirdRoom, fixture.author, 7, login), "B");
  await rf("aonly", fixture.readerLogin);
  await rf("bonly", seal(fixture.thirdRoom, fixture.reader, 7, login), "B");
  // Both clients have learned direct paths; history still needs PATH+ACK.
  await rf("aonly", fixture.path);
  await rf("bonly", seal(fixture.thirdRoom, fixture.reader, 8, new Uint8Array([0x80, 15])), "B");
  const body = join(le32(1700000000), new Uint8Array([0]), new TextEncoder().encode("hello"));
  await Promise.all([
    rf("aonly", fixture.post),
    rf("bonly", seal(fixture.thirdRoom, fixture.author, 2, body), "B"),
  ]);
  const atx = await a.next("A history"), btx = await b.next("B history");
  const ah = history(atx), bh = history(btx, fixture.thirdRoom);
  expect(ah.timestamp).toBe(bh.timestamp);
  expect(ah.ack).toBe(bh.ack); // Deliberately reproduce the old cross-DO race.
  expect(packet(unbase64(atx.packet))!.flood).toBe(true);
  expect(packet(unbase64(btx.packet))!.flood).toBe(true);
  expect((await rf("aonly", ah.ack)).accepted).toBe(false);
  expect((await rf("bonly", ah.ack, "B")).accepted).toBe(false);
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("UPDATE pending SET retry_at=?", Date.now() - 1));
  expect(await runDurableObjectAlarm(bindings.ROOMS.getByName("native"))).toBe(true);
  const retry = history(await a.next("A native retry"));
  expect(retry.text).toBe(ah.text);
  expect(retry.ack).not.toBe(ah.ack);
  await evictDurableObject(bindings.ROOMS.getByName("other"));
  expect((await state("other")).pending[0].require_path_ack).toBe(1);
  expect((await rf("aonly", ah.pathAck)).accepted).toBe(true);
  expect((await state()).pending).toHaveLength(0);
  expect((await state("other")).pending).toHaveLength(1);
  await rf("aonly", fixture.refresh);
  const replay = history(await a.next("replayed A history"));
  expect(replay.pathAck).toBe(ah.pathAck);
  // The same RF attempt now acknowledges a fresh delivery ID after replay.
  expect((await rf("aonly", replay.pathAck)).accepted).toBe(true);
  expect((await state()).pending).toHaveLength(0);
  expect((await rf("bonly", bh.pathAck, "B")).accepted).toBe(true);
  expect((await state("other")).pending).toHaveLength(0);
  for (const backend of ["native", "other"])
    expect((await state(backend)).sessions.find(s => s.client === fixture.reader.publicKey)!.cursor).toBe(1);
  // Scoped transport frames need separate region keys and fail closed.
  const scoped = unbase64(fixture.post);
  scoped[0] = (scoped[0] & ~3) | 3;
  expect((await rf("aonly", base64(scoped))).accepted).toBe(false);
});
