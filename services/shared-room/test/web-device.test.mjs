import {test} from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {createDevice, checkDevice, hex, signChallenge} from "../web/device.js";

test("desktop keys are nonextractable and browser signatures verify with native MeshCore", async () => {
  const device = await checkDevice(await createDevice());
  assert.equal(device.privateKey.extractable, false);
  await assert.rejects(crypto.subtle.exportKey("pkcs8", device.privateKey));
  const origin = "https://room.test", alias = "A", username = "alice", nonce = "ab".repeat(32);
  const challenge = {protocol: "aspen-room.device.v1", nonce, expires: Math.floor(Date.now() / 1000) + 120,
    message: ["aspen-room.device.v1", origin, alias, username, device.publicKey, nonce].join("\n")};
  const signature = await signChallenge(device, challenge, origin, alias, username);
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(await readFile(new URL("../src/native-crypto.wasm", import.meta.url))), {}).exports;
  const arena = new Uint8Array(wasm.memory.buffer, wasm.mc_arena(), 2048);
  try {
    arena.set(Buffer.from(device.publicKey, "hex"), 64);
    arena.set(Buffer.from(signature, "hex"), 1088);
    const message = new TextEncoder().encode(challenge.message);
    arena.set(message, 160);
    assert.equal(wasm.mc_verify(message.length), 1);
  } finally {arena.fill(0);}
  assert.equal(hex(new Uint8Array([0, 255])), "00ff");
  await assert.rejects(signChallenge(device, challenge, origin, "B", username), /different/);
  await assert.rejects(signChallenge(device, {...challenge, expires: 0}, origin, alias, username), /expired/);
  await assert.rejects(checkDevice({...device, publicKey: "00".repeat(32)}), /do not match/);
});
