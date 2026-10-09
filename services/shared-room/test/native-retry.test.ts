import {expect, it} from "vitest";
import {evictDurableObject, runDurableObjectAlarm, runInDurableObject} from "cloudflare:test";
import {bindings, fixture, rf, http, connect, state, history} from "./native-helpers";
import {HISTORY_RETRY_DELAYS} from "../src/room";
import type {RadioDelivery} from "../src/room";
import {joinWeb, webRequest} from "./web-helpers";

async function deliveryStatus(cookie: string, seq = 1) {
  const response = await webRequest("A", "history", cookie);
  expect(response.status).toBe(200);
  const page = await response.json<{messages: Array<{seq: number; rf: RadioDelivery}>}>();
  return page.messages.find(message => message.seq === seq)!.rf;
}

async function alarmTime() {
  return runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) => state.storage.getAlarm());
}

async function retryNow() {
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) => {
    state.storage.sql.exec("UPDATE pending SET retry_at=? WHERE retry_at>0", Date.now() - 1);
  });
  expect(await runDurableObjectAlarm(bindings.ROOMS.getByName("native"))).toBe(true);
}

async function readerAndPost() {
  const radio = await connect("one");
  await rf("one", fixture.readerLogin);
  await rf("one", fixture.path);
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  return {radio, first: await radio.next("initial radio history")};
}

it("retries missing ACKs with bounded native variants after hibernation and accepts an earlier ACK", async () => {
  const before = Date.now();
  const {radio, first} = await readerAndPost();
  const original = history(first);
  const initial = (await state()).pending[0];
  expect(Number(initial.retry_at)).toBeGreaterThanOrEqual(before + HISTORY_RETRY_DELAYS[0]);
  expect(Number(initial.retry_at)).toBeLessThanOrEqual(Date.now() + HISTORY_RETRY_DELAYS[0]);
  expect(await alarmTime()).toBe(initial.retry_at);
  await rf("one", fixture.secondPost);
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  const start = Date.now();
  await retryNow();
  const retry = await radio.next("retry after alarm");
  const received = history(retry);
  expect(received).toMatchObject({text: original.text, timestamp: original.timestamp});
  expect(retry.dispatchId).not.toBe(first.dispatchId);
  expect(retry.packet).not.toBe(first.packet);
  const pending = (await state()).pending[0];
  expect(pending).toMatchObject({delivery_id: initial.delivery_id, seq: initial.seq, frontend: initial.frontend, retry_count: 1});
  expect(Number(pending.retry_at)).toBeGreaterThanOrEqual(start + HISTORY_RETRY_DELAYS[1]);
  expect(JSON.parse(String(pending.proofs))).toHaveLength(2);
  expect((await state()).sessions.find(s => s.client === fixture.reader.publicKey)?.cursor).toBe(0);
  expect((await rf("one", original.ack)).accepted).toBe(true);
  const second = history(await radio.next("next message after late ACK"));
  expect(second.text).toBe("second");
  expect((await rf("one", second.ack)).accepted).toBe(true);
  expect((await state()).pending).toHaveLength(0);
  expect(await alarmTime()).toBeNull();
});

it("stops after three additional attempts without skipping history and retains all late ACK proofs", async () => {
  const {radio, first} = await readerAndPost();
  const original = history(first);
  const packets = new Set([first.packet]), proofs = new Set([original.ack]);
  for (let n = 1; n <= HISTORY_RETRY_DELAYS.length; n++) {
    const start = Date.now();
    await retryNow();
    const retry = await radio.next(`retry ${n}`);
    const received = history(retry);
    packets.add(retry.packet); proofs.add(received.ack);
    expect(received).toMatchObject({text: original.text, timestamp: original.timestamp});
    const pending = (await state()).pending[0];
    expect(pending.retry_count).toBe(n);
    if (n < HISTORY_RETRY_DELAYS.length)
      expect(Number(pending.retry_at)).toBeGreaterThanOrEqual(start + HISTORY_RETRY_DELAYS[n]);
    else expect(pending.retry_at).toBe(0);
  }
  expect(packets.size).toBe(4);
  expect(proofs.size).toBe(4);
  expect(await alarmTime()).toBeNull();
  await rf("one", fixture.secondPost);
  expect(radio.inbox).toHaveLength(0);
  expect((await state()).sessions.find(s => s.client === fixture.reader.publicKey)?.cursor).toBe(0);
  expect((await rf("one", original.ack)).accepted).toBe(true);
  expect(history(await radio.next("history after exhausted-budget late ACK")).text).toBe("second");
});

it("does not let a late TX receipt overwrite the current attempt or turn receipt success into a reader ACK", async () => {
  const {radio, first} = await readerAndPost();
  await retryNow();
  const retry = await radio.next("retry with new receipt binding");
  for (const [dispatchId, outcome] of [[retry.dispatchId, "sent"], [first.dispatchId, "failed"]]) {
    const response = await http("one", {op: "txReceipt", dispatchId, outcome});
    expect(response.status).toBe(200);
    await response.text();
  }
  expect((await state()).pending[0]).toMatchObject({dispatch_id: retry.dispatchId, state: "sent"});
  expect((await state()).sessions.find(s => s.client === fixture.reader.publicKey)?.cursor).toBe(0);
  expect((await rf("one", history(first).ack)).accepted).toBe(true);
  expect((await state()).pending).toHaveLength(0);
});

it("pauses offline retries without choosing another frontend and rearms the same frontend on reconnect", async () => {
  const {radio, first} = await readerAndPost();
  const other = await connect("two");
  const closed = new Promise(resolve => radio.ws.addEventListener("close", resolve, {once: true}));
  radio.ws.close(); await closed;
  await retryNow();
  expect((await state()).pending[0]).toMatchObject({retry_count: 0, frontend: "one"});
  expect(other.inbox).toHaveLength(0);
  expect(await alarmTime()).toBeNull();
  const reconnected = await connect("one");
  expect(reconnected.inbox).toHaveLength(0);
  expect(await alarmTime()).not.toBeNull();
  await retryNow();
  expect(history(await reconnected.next("same-frontend retry")).text).toBe(history(first).text);
  expect(other.inbox).toHaveLength(0);
});

it("does not retry a pending delivery under changed frontend credentials or scope", async () => {
  const {radio} = await readerAndPost();
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("UPDATE pending SET retry_binding=?", JSON.stringify(["different-credential", "ab"])));
  await retryNow();
  expect(radio.inbox).toHaveLength(0);
  expect((await state()).pending[0].retry_count).toBe(0);
  expect(await alarmTime()).toBeNull();
});

it("keeps old pending deliveries paused during the additive retry migration and accepts their original ACK", async () => {
  const {radio, first} = await readerAndPost();
  await runInDurableObject(bindings.ROOMS.getByName("native"), async (_room, state) => {
    for (const column of ["proofs", "retry_at", "retry_count", "retry_binding", "dispatch_id"])
      state.storage.sql.exec(`ALTER TABLE pending DROP COLUMN ${column}`);
    await state.storage.deleteAlarm();
  });
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  await rf("one", fixture.secondPost);
  expect(radio.inbox).toHaveLength(0);
  expect((await state()).pending[0]).toMatchObject({retry_at: 0, proofs: "[]", retry_binding: null});
  expect(await alarmTime()).toBeNull();
  expect((await rf("one", history(first).ack)).accepted).toBe(true);
  expect(history(await radio.next("next history after migrated ACK")).text).toBe("second");
});

it("reserves earlier proofs across aliases and pauses instead of reusing all four occupied native variants", async () => {
  const a = await connect("one"), b = await connect("one", "SharedB");
  await rf("one", fixture.readerLogin);
  await rf("one", fixture.sharedReaderLogin, "SharedB");
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  const firstA = history(await a.next("A first history"));
  await b.next("SharedB first history");
  await retryNow();
  await a.next("A retry"); await b.next("SharedB retry");
  const pending = (await state()).pending;
  expect(new Set(pending.flatMap(p => JSON.parse(String(p.proofs)))).size).toBe(4);
  await retryNow();
  expect(a.inbox).toHaveLength(0); expect(b.inbox).toHaveLength(0);
  expect((await state()).pending.every(p => p.retry_at === 0)).toBe(true);
  expect((await rf("one", firstA.ack)).accepted).toBe(true);
  expect((await state()).pending.map(p => p.alias)).toEqual(["SharedB"]);
});

it("distinguishes saved, queued, transmitted and native-ACK-confirmed delivery after hibernation", async () => {
  const {radio, first} = await readerAndPost();
  const web = await joinWeb();
  expect(await deliveryStatus(web.cookie)).toMatchObject({
    recipients: 1, queued: 1, sent: 0, acknowledged: 0, paused: 0, attempts: 1,
  });
  await (await http("one", {op: "txReceipt", dispatchId: first.dispatchId, outcome: "sent"})).text();
  expect(await deliveryStatus(web.cookie)).toMatchObject({queued: 0, sent: 1, acknowledged: 0});
  await rf("one", fixture.secondPost);
  expect(await deliveryStatus(web.cookie, 2)).toMatchObject({queued: 1, acknowledged: 0, attempts: 0});
  expect((await state()).sessions.find(s => s.client === fixture.reader.publicKey)?.cursor).toBe(0);
  await rf("one", history(first).ack);
  await radio.next("next history after native ACK");
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  expect(await deliveryStatus(web.cookie)).toMatchObject({
    recipients: 1, queued: 0, sent: 0, acknowledged: 1, attempts: 1, nextRetryAt: null,
  });
  await (await http("one", {op: "txReceipt", dispatchId: first.dispatchId, outcome: "failed"})).text();
  expect((await deliveryStatus(web.cookie)).acknowledged).toBe(1);
});

it.each([["unknown", "uncertain"], ["failed", "failed"]] as const)(
  "reports %s transmission without inventing an ACK", async (outcome, field) => {
    const {first} = await readerAndPost();
    const web = await joinWeb();
    await (await http("one", {op: "txReceipt", dispatchId: first.dispatchId, outcome})).text();
    expect(await deliveryStatus(web.cookie)).toMatchObject({[field]: 1, acknowledged: 0, sent: 0});
  });

it("reports paused retries and exhausted budgets while accepting a late native ACK", async () => {
  const {radio, first} = await readerAndPost();
  const web = await joinWeb();
  const closed = new Promise(resolve => radio.ws.addEventListener("close", resolve, {once: true}));
  radio.ws.close(); await closed;
  expect(await deliveryStatus(web.cookie)).toMatchObject({paused: 1, nextRetryAt: null, acknowledged: 0});
  const reconnected = await connect("one");
  for (let n = 1; n <= HISTORY_RETRY_DELAYS.length; n++) {
    await retryNow();
    await reconnected.next(`visible retry ${n}`);
    expect(await deliveryStatus(web.cookie)).toMatchObject({retrying: 1, attempts: n + 1, acknowledged: 0});
  }
  expect(await deliveryStatus(web.cookie)).toMatchObject({exhausted: 1, nextRetryAt: null, attempts: 4});
  await rf("one", history(first).ack);
  expect(await deliveryStatus(web.cookie)).toMatchObject({acknowledged: 1, exhausted: 0, retrying: 0, attempts: 4});
});

it("backfills only active dispatch evidence when upgrading an existing room", async () => {
  const {first} = await readerAndPost();
  const web = await joinWeb();
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) => {
    state.storage.sql.exec("DROP INDEX dispatches_message");
    state.storage.sql.exec("ALTER TABLE dispatches DROP COLUMN seq");
  });
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  expect(await deliveryStatus(web.cookie)).toMatchObject({queued: 1, acknowledged: 0, attempts: 1});
  await rf("one", history(first).ack);
  expect((await deliveryStatus(web.cookie)).acknowledged).toBe(1);
});
