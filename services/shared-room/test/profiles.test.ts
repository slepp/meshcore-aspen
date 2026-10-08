import {expect, it} from "vitest";
import {evictDurableObject, runInDurableObject} from "cloudflare:test";
import {bindings, crypto, fixture, http, rf, sockets} from "./native-helpers";
import {base64, fromHex, join} from "../src/native-crypto";
import {le32, RadioCodec} from "../src/native";
import {joinWeb, webPost, webRequest, webSocket} from "./web-helpers";

function advert(name: string, timestamp = Math.floor(Date.now() / 1000), flags = 0x81, optional = new Uint8Array()) {
  const publicKey = fromHex(fixture.author.publicKey), stamp = le32(timestamp);
  const app = join(new Uint8Array([flags]), optional, new TextEncoder().encode(name));
  const signature = crypto.sign(fromHex(fixture.author.key), join(publicKey, stamp, app));
  return join(new Uint8Array([17, 0x80]), publicKey, stamp, signature, app);
}

it("maps full radio keys from signed adverts, updates live readers and retains names after restart", async () => {
  const web = await joinWeb(), remote = await joinWeb("SharedB", 2, "Bob");
  const browser = await webSocket("SharedB", remote.cookie, sockets);
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  expect((await browser.next()).message?.author).toBe(fixture.author.publicKey);
  const stamp = Math.floor(Date.now() / 1000);
  expect(await rf("one", base64(advert("VE6SLP", stamp)))).toEqual({accepted: true});
  const profile = (await browser.next()).profile;
  expect(profile).toEqual({publicKey: fixture.author.publicKey, name: "VE6SLP", advertType: 1, timestamp: stamp, source: "radio"});
  await evictDurableObject(bindings.ROOMS.getByName("native"));
  const page = await (await webRequest("A", "history", web.cookie)).json<{profiles: unknown[]; messages: {text: string}[]}>();
  expect(page.profiles).toEqual([profile]);
  expect(page.messages[0].text).toBe("hello");
  expect(await rf("two", base64(advert("Renamed", stamp + 1)), "SharedB")).toEqual({accepted: true});
  expect((await browser.next()).profile?.name).toBe("Renamed");
  await rf("one", base64(advert("Old", stamp)));
  await rf("one", base64(advert("Conflicting", stamp + 1)));
  await rf("one", base64(advert("Renamed", stamp + 1)));
  expect(browser.inbox).toHaveLength(0);
  const again = await (await webRequest("SharedB", "history", remote.cookie)).json<{profiles: {name: string}[]}>();
  expect(again.profiles[0].name).toBe("Renamed");
  const memberCount = await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("SELECT COUNT(*) AS n FROM sessions").one().n);
  expect(memberCount).toBe(1);
});

it("rejects forged, malformed, excessive, future and wrong-region profile adverts", async () => {
  const web = await joinWeb(), wire = advert("Valid");
  const forged = wire.slice(); forged[39] ^= 1;
  for (const invalid of [forged, advert("\u001b[31m"), advert("x".repeat(32)), advert("\ud800").slice(0, 70),
    advert("future", Math.floor(Date.now() / 1000) + 301), advert("missing", undefined, 0x01),
    advert("wrong flags", undefined, 0xf1)]) {
    expect(await rf("one", base64(invalid))).toEqual({accepted: false});
  }
  await webPost("A", web.cookie, "still working");
  const count = await runInDurableObject(bindings.ROOMS.getByName("native"), (_room, state) =>
    state.storage.sql.exec("SELECT COUNT(*) AS n FROM profiles").one().n);
  expect(count).toBe(0);
  const codec = new RadioCodec(bindings);
  const plain = codec.identity("A"), scoped = codec.identity("A", "ab");
  expect(codec.profile(wire, plain)?.name).toBe("Valid");
  expect(codec.profile(wire, scoped)).toBeUndefined();
  const payload = wire.slice(2), wrapped = codec.routed(scoped, 4, payload, "");
  expect(codec.profile(wrapped, scoped)?.name).toBe("Valid");
  expect(codec.profile(wrapped, codec.identity("A", "edm"))).toBeUndefined();
  expect(codec.profile(wrapped, plain)).toBeUndefined();
});

it("parses optional signed location/features without confusing them with the advertised name", async () => {
  const codec = new RadioCodec(bindings), identity = codec.identity("A");
  const wire = advert("Zoë", undefined, 0xf1, new Uint8Array(12));
  expect(codec.profile(wire, identity)).toMatchObject({name: "Zoë", publicKey: fixture.author.publicKey});
  await http("one", {op: "rf", packet: base64(wire)});
  const web = await joinWeb(), browser = await webSocket("A", web.cookie, sockets);
  await rf("one", fixture.authorLogin);
  await rf("one", fixture.post);
  expect((await browser.next()).profile?.name).toBe("Zoë");
  expect((await browser.next()).message?.text).toBe("hello");
});
