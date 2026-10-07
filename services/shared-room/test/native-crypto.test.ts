import { it, expect } from "vitest";
import { NativeCrypto, fromHex, toHex, join } from "../src/native-crypto";
import { RadioCodec, le32 } from "../src/native";
import type { Env } from "../src/config";
import type { Delivery } from "../src/protocol";
it("reuses native expanded keys, ciphertext and recipient-bound ACK vector", () => {
  const c = new NativeCrypto();
  const seed = new Uint8Array(32);
  for (let i = 0; i < 32; i += 4) seed[i] = 1;
  const room = c.seed(seed),
    client = c.seed(new Uint8Array(32));
  expect(c.publicKey(room.key)).toEqual(room.publicKey);
  const secret = c.secret(room.key, client.publicKey)!;
  expect(secret).toEqual(c.secret(client.key, room.publicKey));
  const plain = c.crypt(
    secret,
    fromHex("f426cae8c664b3d0b2d1b203557e4eb86189"),
    true,
  )!;
  expect(new DataView(plain.buffer).getUint32(0, true)).toBe(1700000000);
  expect(new TextDecoder().decode(plain.slice(5, 10))).toBe("hello");
  expect(
    toHex(c.sha(join(plain.slice(0, 10), client.publicKey)).slice(0, 4)),
  ).toBe("8ae1e1f3");
  const tampered = fromHex("f526cae8c664b3d0b2d1b203557e4eb86189");
  expect(c.crypt(secret, tampered, true)).toBeUndefined();
  const message = fromHex("010203");
  const sig = c.sign(room.key, message);
  expect(c.verify(room.publicKey, sig, message)).toBe(true);
  expect(c.verify(client.publicKey, sig, message)).toBe(false);
  const codec = new RadioCodec({} as Env);
  const identity = {
    alias: "A",
    key: room.key,
    publicKey: room.publicKey,
    name: "A",
  };
  const delivery: Delivery = {
    type: "delivery",
    alias: "A",
    client: toHex(client.publicKey),
    deliveryId: "fixture",
    route: "AQCA",
    message: {
      seq: 1,
      timestamp: 1700000000,
      originAlias: "B",
      author: toHex(room.publicKey),
      clientTimestamp: 1,
      text: "hello",
    },
  };
  const proofs = new Set<string>();
  for (let n = 0; n < 4; n++) {
    const body = join(
      le32(1700000000),
      new Uint8Array([8 | n]),
      room.publicKey.slice(0, 4),
      new TextEncoder().encode("hello"),
    );
    const value = new DataView(c.sha(join(body, client.publicKey)).buffer)
      .getUint32(0, true)
      .toString(16)
      .padStart(8, "0");
    proofs.add(value);
  }
  const allowed = [...proofs][0];
  proofs.delete(allowed);
  expect(codec.delivery(identity, delivery, proofs)?.proof).toBe(allowed);
  proofs.add(allowed);
  expect(codec.delivery(identity, delivery, proofs)).toBeUndefined();
});
