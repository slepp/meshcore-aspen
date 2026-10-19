import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import {canonicalCode, nodeFromSeed, originOf, redeemBody} from "../tools/node-sim.mjs";

const vectors = JSON.parse(readFileSync(new URL("./node-protocol-vectors.json", import.meta.url), "utf8"));

test("the simulator reproduces the golden node protocol signatures", () => {
  const node = nodeFromSeed(vectors.seedHex);
  assert.equal(node.pub, vectors.nodeHex);
  for (const c of vectors.cases) {
    const body = redeemBody(node, vectors.origin, c.op, {code: vectors.code, name: c.name, ts: vectors.ts});
    assert.equal(body.sig, c.sig);
    assert.equal(body.code, vectors.code);
  }
});

test("codes normalise like the device and the Worker", () => {
  assert.equal(canonicalCode("abcd-efgh-jkmn-pqrs"), vectors.code);
  assert.equal(canonicalCode("OIL0 0000 0000 0000"), "0110000000000000");
  assert.throws(() => canonicalCode("UBCD-EFGH-JKMN-PQRS"));
  assert.throws(() => canonicalCode("ABCD"));
});

test("the origin is the lowercase host without port or path", () => {
  assert.equal(originOf("https://Rooms.Example.workers.dev:443/x"), "https://rooms.example.workers.dev");
});
