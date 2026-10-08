import { expect, it } from "vitest";
import { runInDurableObject, evictDurableObject } from "cloudflare:test";
import { bindings, fixture, crypto, http, rf, connect, state, history } from "./native-helpers";
import { base64, unbase64, fromHex, join, toHex } from "../src/native-crypto";
import { packet, le32 } from "../src/native";
import {joinWeb, webPost, webRequest, webSocket} from "./web-helpers";
import {sockets} from "./native-helpers";

it("shares canonical web and native radio content without granting browser identities RF membership", async () => {
  const radio = await connect("two");
  await rf("two", fixture.readerLogin);
  await rf("two", fixture.path);
  const web = await joinWeb(), remote = await joinWeb("SharedB", 2, "Bob");
  const browser = await webSocket("SharedB", remote.cookie, sockets);
  const sent = await webPost("A", web.cookie, "Hello from IP");
  expect((await browser.next()).message).toEqual(sent.message);
  const received = history(await radio.next("web post over native radio"));
  expect(received.text).toBe("Alice: Hello from IP");
  expect(received.timestamp).toBe(sent.message.timestamp);
  await rf("two", received.ack);
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  expect((await browser.next()).message).toMatchObject({author: fixture.author.publicKey, text: "hello", seq: 2});
  const page = await (await webRequest("A", "history", web.cookie)).json<{messages: {text: string}[]}>();
  expect(page.messages.map(m => m.text)).toEqual(["Alice: Hello from IP", "hello"]);
  const stored = await state();
  expect(stored.sessions.some(s => s.client === web.author)).toBe(false);
  const native = history(await radio.next("native author history"));
  expect(native.text).toBe("hello");
  await rf("two", native.ack);
  const bounded = await webPost("A", web.cookie, "🙂".repeat(36));
  const full = history(await radio.next("151-byte UTF-8 web history"));
  expect(full.text).toBe(bounded.message.text);
  expect(new TextEncoder().encode(full.text)).toHaveLength(151);
});

it("verifies native logins/posts from two frontends, pushes ordered ciphertext and restores pending state", async () => {
  const one = await connect("one"),
    two = await connect("two");
  const login = await Promise.all([
    rf("one", fixture.authorLogin),
    rf("two", fixture.authorLogin),
  ]);
  expect(login.filter((r) => r.transmission)).toHaveLength(1);
  expect(login.find((r) => r.transmission).transmission.delayMs).toBe(300);
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
  expect(posts.find((r) => r.transmission).transmission.delayMs).toBe(0);
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
  // Reopen the pre-PATH-mode schema without discarding its pending state.
  await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, s) => {
    s.storage.sql.exec("ALTER TABLE pending DROP COLUMN require_path_ack");
  });
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
        "INSERT INTO pending(alias, client, delivery_id, seq, frontend, proof, state) VALUES (?, ?, ?, 1, 'two', ?, 'unknown')",
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
  const shared = await connect("one", "SharedB");
  const login = await http(
    "one",
    { op: "rf", packet: fixture.sharedReaderLogin },
    "SharedB",
  );
  expect(login.status).toBe(200);
  await login.text();
  await shared.next();
  const active = (await state()).pending;
  expect(active).toHaveLength(2);
  expect(new Set(active.map((p) => p.proof)).size).toBe(2);
  // A different modem hearing this bare ACK must not consume SharedB.
  expect((await rf("one", history(first).ack)).accepted).toBe(false);
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
