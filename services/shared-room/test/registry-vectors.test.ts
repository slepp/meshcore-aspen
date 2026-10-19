import {expect, it} from "vitest";
import vectors from "./node-protocol-vectors.json";
import {NativeCrypto, fromHex, toHex} from "../src/native-crypto";
import {canonicalCode, codeHash, createSubject, formatCode, randomCode, signedMessage, type NodeOp} from "../src/node-protocol";

const crypto = new NativeCrypto();
const text = new TextDecoder();

it("reproduces the golden code hash and canonical forms", async () => {
  expect(await codeHash(vectors.code)).toBe(vectors.codeHash);
  for (const form of ["abcd-efgh-jkmn-pqrs", "ABCD EFGH JKMN PQRS", "abcdefghjkmnpqrs"])
    expect(canonicalCode(form)).toBe(vectors.code);
  expect(canonicalCode("OIL0-0000-0000-0000")).toBe("0110000000000000");
  for (const bad of ["ABCDEFGHJKMNPQR", "ABCDEFGHJKMNPQRSU", "UBCDEFGHJKMNPQRS", "ABCDEFGHJKMNPQR*", 5, undefined])
    expect(canonicalCode(bad)).toBeUndefined();
  expect(formatCode(vectors.code)).toBe("ABCD-EFGH-JKMN-PQRS");
  expect(canonicalCode(randomCode())).toHaveLength(16);
});

it("signs the golden messages with the repo Ed25519 and verifies them", async () => {
  const {key, publicKey} = crypto.seed(fromHex(vectors.seedHex));
  expect(toHex(publicKey)).toBe(vectors.nodeHex);
  for (const c of vectors.cases) {
    const subject = c.op === "create" ? await createSubject(c.name) : "-";
    expect(subject).toBe(c.subject);
    const message = signedMessage({origin: vectors.origin, op: c.op as NodeOp, code: vectors.codeHash.slice(0, 32),
      node: vectors.nodeHex, ts: vectors.ts, subject});
    expect(text.decode(message)).toBe(c.message);
    expect(toHex(crypto.sign(key, message))).toBe(c.sig);
    expect(crypto.verify(publicKey, fromHex(c.sig), message)).toBe(true);
  }
});
